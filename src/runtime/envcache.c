// getenv from a table: the runtime reads its switches (TOTK_*) with getenv in hot paths (every command buffer, every
// texture); the C runtime's getenv takes a lock and walks the whole environment each time, which came to 1-3 % of the
// game threads in a profile of a run (2026-10-31). The link's --wrap (tools/build_game.py) sends every getenv of our code
// here; an answer, absent included, is kept until the environment changes (runtime_env_changed).
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
static SRWLOCK g_lock = SRWLOCK_INIT;
#define RD_LOCK() AcquireSRWLockShared(&g_lock)
#define RD_UNLOCK() ReleaseSRWLockShared(&g_lock)
#define WR_LOCK() AcquireSRWLockExclusive(&g_lock)
#define WR_UNLOCK() ReleaseSRWLockExclusive(&g_lock)
#define strdup _strdup
#else   // Linux: glibc walks the whole environment on every getenv, and Steam's is large
#include <pthread.h>
static pthread_rwlock_t g_lock = PTHREAD_RWLOCK_INITIALIZER;
#define RD_LOCK() pthread_rwlock_rdlock(&g_lock)
#define RD_UNLOCK() pthread_rwlock_unlock(&g_lock)
#define WR_LOCK() pthread_rwlock_wrlock(&g_lock)
#define WR_UNLOCK() pthread_rwlock_unlock(&g_lock)
#endif

char *__real_getenv(const char *name);

enum { SLOTS = 1024 };
typedef struct { char *name; char *value; } slot;
static slot g_tab[SLOTS];
static int g_used;

static uint32_t hash(const char *s) { uint32_t h = 2166136261u; while (*s) h = (h ^ (uint8_t)*s++) * 16777619u; return h; }

char *__wrap_getenv(const char *name) {
    if (!name) return __real_getenv(name);
    uint32_t h = hash(name);
    RD_LOCK();
    for (uint32_t i = 0; i < SLOTS; i++) {
        slot *s = &g_tab[(h + i) & (SLOTS - 1)];
        if (!s->name) break;
        if (!strcmp(s->name, name)) { char *v = s->value; RD_UNLOCK(); return v; }
    }
    RD_UNLOCK();
    WR_LOCK();
    char *v = __real_getenv(name);
    if (g_used < SLOTS / 2) {   // a full table just asks the C runtime
        for (uint32_t i = 0; i < SLOTS; i++) {
            slot *s = &g_tab[(h + i) & (SLOTS - 1)];
            if (s->name && !strcmp(s->name, name)) break;
            if (!s->name) {
                char *n = strdup(name);
                if (n) { s->value = v; s->name = n; g_used++; }
                break;
            }
        }
    }
    WR_UNLOCK();
    return v;
}

// Any change of the environment (platform/win_compat.h setenv / unsetenv, platform/windows.c): the values kept point into
// the environment as it was. (_putenv_s is called through the DLL import, which the link's --wrap does not reach.)
void runtime_env_changed(void) {
    WR_LOCK();
    for (int i = 0; i < SLOTS; i++) { free(g_tab[i].name); g_tab[i].name = NULL; }
    g_used = 0;
    WR_UNLOCK();
}
#ifndef _WIN32
// Linux: the link wraps the environment's changes too (tools/build_game.py linux_libs), so the table follows them.
int __real_setenv(const char *name, const char *value, int overwrite);
int __real_unsetenv(const char *name);
int __real_putenv(char *s);
int __wrap_setenv(const char *name, const char *value, int overwrite) { int r = __real_setenv(name, value, overwrite); runtime_env_changed(); return r; }
int __wrap_unsetenv(const char *name) { int r = __real_unsetenv(name); runtime_env_changed(); return r; }
int __wrap_putenv(char *s) { int r = __real_putenv(s); runtime_env_changed(); return r; }
#endif
