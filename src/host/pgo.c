// Profile-guided optimization of the game's code, the training build (tools/build_game.py --pgo gen, ULTRS_PGO_GEN):
// the generated game code counts its functions and branches (clang -fprofile-generate), and this file writes the
// counts to ../pgo/ultratears-<pid>.profraw beside the game folder - once a minute, since the game leaves by _Exit
// (no atexit) and a run may end in a hang or a crash, and at the window's close. The counts are of the lifted
// functions only (their names and branch counts, no bytes of the game); tools/build_game.py --pgo use builds with
// them (llvm-profdata merge). In other builds both functions do nothing.
#include <stdio.h>

void pgo_start(void);
void pgo_flush(void);

#ifdef ULTRS_PGO_GEN
#include <pthread.h>
#include <unistd.h>
#ifdef _WIN32
#include <direct.h>
#define pgo_mkdir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define pgo_mkdir(p) mkdir(p, 0777)
#endif

void __llvm_profile_set_filename(const char *name);
int __llvm_profile_write_file(void);

// The runtime truncates its file once, at the start, and appends every later write (the first training run's file held
// 21 whole snapshots and a cut-off last one, which llvm-profdata refuses). So each write goes to a .tmp, which naming
// truncates, and replaces the .profraw only when complete: the file holds the latest whole snapshot (the counts are
// cumulative), and a write cut off by the exit leaves the previous one.
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
void pgo_flush(void) {
    char tmp[64], out[64];
    snprintf(tmp, sizeof tmp, "../pgo/ultratears-%d.tmp", (int)getpid());
    snprintf(out, sizeof out, "../pgo/ultratears-%d.profraw", (int)getpid());
    pthread_mutex_lock(&g_mu);
    __llvm_profile_set_filename(tmp);
    int r = __llvm_profile_write_file();
    if (!r) {
        remove(out);
        r = rename(tmp, out);
    }
    pthread_mutex_unlock(&g_mu);
    fprintf(stderr, "[totk] PGO training: counts written%s\n", r ? " - FAILED" : "");
}
static void *pgo_thread(void *unused) {
    (void)unused;
    for (;;) {
        sleep(60);
        pgo_flush();
    }
    return NULL;
}
void pgo_start(void) {
    pgo_mkdir("../pgo");   // the working directory is the game folder (boot.c)
    fprintf(stderr, "[totk] PGO training build: the game's code counts itself, written to ../pgo once a minute\n");
    pthread_t t;
    if (pthread_create(&t, NULL, pgo_thread, NULL) == 0) pthread_detach(t);
}
#else
void pgo_start(void) {}
void pgo_flush(void) {}
#endif
