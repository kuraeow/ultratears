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

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
void pgo_flush(void) {
    pthread_mutex_lock(&g_mu);
    int r = __llvm_profile_write_file();
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
    __llvm_profile_set_filename("../pgo/ultratears-%p.profraw");
    fprintf(stderr, "[totk] PGO training build: the game's code counts itself, written to ../pgo once a minute\n");
    pthread_t t;
    if (pthread_create(&t, NULL, pgo_thread, NULL) == 0) pthread_detach(t);
}
#else
void pgo_start(void) {}
void pgo_flush(void) {}
#endif
