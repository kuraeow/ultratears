// getenv from a table: the runtime reads its switches (TOTK_*) with getenv in hot paths (every command buffer, every
// texture); the C runtime's getenv takes a lock and walks the whole environment each time, which came to 1-3 % of the
// game threads in a profile of a run (2026-10-31). The link's --wrap (tools/build_game.py) sends every getenv of our code
// here; an answer, absent included, is kept until the environment changes (runtime_env_changed).
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

char *__real_getenv(const char *name);

enum { SLOTS = 1024 };
typedef struct { char *name; char *value; } slot;
static slot g_tab[SLOTS];
static int g_used;
static SRWLOCK g_lock = SRWLOCK_INIT;

static uint32_t hash(const char *s) { uint32_t h = 2166136261u; while (*s) h = (h ^ (uint8_t)*s++) * 16777619u; return h; }

char *__wrap_getenv(const char *name) {
    if (!name) return __real_getenv(name);
    uint32_t h = hash(name);
    AcquireSRWLockShared(&g_lock);
    for (uint32_t i = 0; i < SLOTS; i++) {
        slot *s = &g_tab[(h + i) & (SLOTS - 1)];
        if (!s->name) break;
        if (!strcmp(s->name, name)) { char *v = s->value; ReleaseSRWLockShared(&g_lock); return v; }
    }
    ReleaseSRWLockShared(&g_lock);
    AcquireSRWLockExclusive(&g_lock);
    char *v = __real_getenv(name);
    if (g_used < SLOTS / 2) {   // a full table just asks the C runtime
        for (uint32_t i = 0; i < SLOTS; i++) {
            slot *s = &g_tab[(h + i) & (SLOTS - 1)];
            if (s->name && !strcmp(s->name, name)) break;
            if (!s->name) {
                char *n = _strdup(name);
                if (n) { s->value = v; s->name = n; g_used++; }
                break;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return v;
}

// Any change of the environment (platform/win_compat.h setenv / unsetenv, platform/windows.c): the values kept point into
// the environment as it was. (_putenv_s is called through the DLL import, which the link's --wrap does not reach.)
void runtime_env_changed(void) {
    AcquireSRWLockExclusive(&g_lock);
    for (int i = 0; i < SLOTS; i++) { free(g_tab[i].name); g_tab[i].name = NULL; }
    g_used = 0;
    ReleaseSRWLockExclusive(&g_lock);
}
