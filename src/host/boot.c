/* Starts the recompiled game the way MacNativeGames' loader.c boot() does: the main guest thread enters the SDK's own
 * nn::init::Start(main thread handle, 0, loader callback, module initializers, module finalizers).
 *   totk <exefs dir>
 */
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "version.h"   // build/version.h (tools/build_game.py, tools/version.py)
#ifdef _WIN32
#include <windows.h>
#endif

static void loader_ready(Cpu *c) { (void)c; fprintf(stderr, "[totk] rtld callback: modules linked\n"); }

static void module_inits(Cpu *c) {
    (void)c;
    fprintf(stderr, "[totk] SDK startup returned; running module constructors\n");
    for (int i = 0; i < g_nmodules; i++) {
        Module *m = &g_modules[i];
        if (!m->init) continue;
        fprintf(stderr, "[totk] ENTER %s DT_INIT\n", m->name);
        GCALL0(m->init);
        fprintf(stderr, "[totk] RETURN %s DT_INIT\n", m->name);
    }
}

static void module_finis(Cpu *c) { (void)c; runtime_fail("guest exited: module finalizers not yet implemented"); }

// The guest window is claimed before anything else in the process can allocate there (the C runtime, thread stacks).
// The executable is also linked without high-entropy ASLR, which otherwise scatters the process's own allocations
// over the whole 47-bit space, the window included.
static bool g_window;
__attribute__((constructor(101))) static void claim_guest_window(void) { g_window = plat_reserve_guest(GUEST_LO, GUEST_HI); }

static void *game_main(void *unused);

// The game's folder (tools/setup.py makes it: data\\, userdata\\, ultratears.cfg, UltraTears.exe) becomes the working
// directory: the exe's folder, or its parent for build\\UltraTears.exe. Every path below is relative to it; the defaults
// are setup.py's layout (data\\exefs, data\\romfs, data\\shaders\\ryujinx, data\\textures, data\\fonts, userdata).
// ultratears.cfg there may name other places: lines "name=value" give the ExeFS (exefs=), the RomFS image (romfs=),
// shaders, texture cache, saves and fonts, each as the TOTK_* variable it stands for unless that is already set (a
// TOTK_* line sets one directly).
static char g_exefs[4096];
static const char *default_exefs(void) {
    const char *exefs = "data/exefs";
#ifdef _WIN32
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, sizeof path);
    char *slash = n && n < sizeof path ? strrchr(path, '\\') : NULL;
    if (!slash) return exefs;
    *slash = 0;
    char cfg[MAX_PATH + 32];
    snprintf(cfg, sizeof cfg, "%s\\ultratears.cfg", path);
    FILE *f = fopen(cfg, "r");
    if (!f && (slash = strrchr(path, '\\')) && !_stricmp(slash, "\\build")) {   // build\\UltraTears.exe: the folder above
        *slash = 0;
        snprintf(cfg, sizeof cfg, "%s\\ultratears.cfg", path);
        f = fopen(cfg, "r");
    }
    if (!SetCurrentDirectoryA(path)) fprintf(stderr, "[totk] cannot enter %s\n", path);
    if (f) {
        char line[4600];
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *eq = strchr(line, '=');
            if (!eq || line[0] == '#') continue;
            *eq = 0;
            static const char *const keys[][2] = {   // the file's names -> the runtime's variables
                {"romfs", "TOTK_ROMFS"}, {"shaders", "TOTK_SHADER_DIR"}, {"compiled", "TOTK_SHADER_BIN"}, {"shaderpack", "TOTK_SHADER_PACK"}, {"textures", "TOTK_BC7_DIR"},
                {"saves", "TOTK_PROFILE_DIR"}, {"fonts", "TOTK_SHARED_FONTS"}, {"fps", "TOTK_FPS_LIMIT"},
                // the player's settings (the launcher's Play screen writes them)
                {"window", "TOTK_WINDOW"}, {"vsync", "TOTK_VSYNC"}, {"lang", "TOTK_LANG"}, {"debug", "TOTK_STATS"},
                {"resolution", "TOTK_OUTPUT_RES"}, {"scale", "TOTK_RENDER_SCALE"},
                {"sharpness", "TOTK_SHARPNESS"}, {"upscale", "TOTK_UPSCALER"}};
            if (!strcmp(line, "exefs")) snprintf(g_exefs, sizeof g_exefs, "%s", eq + 1), exefs = g_exefs;
            else if (!strcmp(line, "snapshots")) {   // the launcher's "Extended debugging with snapshots": F9 saves the frame into diag\f<frame>
                if (!strcmp(eq + 1, "1")) setenv("TOTK_DUMP_KEY", "diag", 0);
            }
            else if (!strncmp(line, "TOTK_", 5)) setenv(line, eq + 1, 0);
            else
                for (unsigned i = 0; i < sizeof keys / sizeof *keys; i++)
                    if (!strcmp(line, keys[i][0])) setenv(keys[i][1], eq + 1, 0);
        }
        fclose(f);
        fprintf(stderr, "[totk] game folder %s (%s)\n", path, cfg);
    } else
        fprintf(stderr, "[totk] game folder %s (no ultratears.cfg: setup.py's layout)\n", path);
#endif
    return exefs;
}

int main(int argc, char **argv) {
    setvbuf(stderr, NULL, _IONBF, 0);
    fprintf(stderr, "UltraTears %s (commit %s)\n", ULTRS_VERSION, ULTRS_COMMIT);   // first line of every log
    // --debug: the statistics lines in the log (TOTK_STATS=1; the launcher's Debug mode writes debug=1 instead)
    const char *arg = NULL;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--debug")) setenv("TOTK_STATS", "1", 1);
        else if (!arg) arg = argv[i];
        else { fprintf(stderr, "usage: UltraTears [--debug] [exefs dir]\n"); return 2; }
    const char *exefs = arg ? arg : default_exefs();
    plat_debug_console(getenv("TOTK_STATS") != NULL);   // after ultratears.cfg (debug=1)
    plat_init();
    plat_crash_handler_install();
    if (!g_window) {
#ifdef _WIN32
        for (uint64_t a = GUEST_LO; a < GUEST_HI;) {   // what already occupies the window
            MEMORY_BASIC_INFORMATION mi;
            if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi)) break;
            if (mi.State != MEM_FREE)
                fprintf(stderr, "  occupied %llx +%llx state %lx type %lx\n", (unsigned long long)(uintptr_t)mi.BaseAddress,
                        (unsigned long long)mi.RegionSize, mi.State, mi.Type);
            a = (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize;
        }
#endif
        runtime_fail("cannot reserve the guest address window");
    }
    uint64_t t0 = runtime_now_ns();
    loader_load(exefs);
    dispatch_init();
    fprintf(stderr, "[totk] modules loaded and linked in %.0f ms\n", (runtime_now_ns() - t0) / 1e6);

    runtime_profiler_start();

    // The window takes the main thread (SDL wants its events there); the game gets its own. TOTK_HEADLESS=1: no
    // window, no sound device, the game on the main thread (automated runs).
    const char *headless = getenv("TOTK_HEADLESS");
    if (headless && strcmp(headless, "0")) game_main(NULL);
    if (!window_init()) runtime_fail("window (TOTK_HEADLESS=1 runs without one)");
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 16 << 20);   // the host stack under the main guest thread's calls
    pthread_t t;
    if (pthread_create(&t, &a, game_main, NULL)) runtime_fail("game thread");
    window_loop();
}

static void *game_main(void *unused) {
    (void)unused;
    runtime_memory_init();
    host_services_register();
    uint32_t main_handle = svc_thread_main_init();
    Cpu *c = guest_cpu_new(runtime_main_stack(0x100000), guest_tls_alloc());   // main.npdm: 1 MiB main stack
    guest_cpu_set(c);

    uint64_t start = (uint64_t)(uintptr_t)runtime_symbol("_ZN2nn4init5StartEmmPFvvES2_S2_");
    if (!start) runtime_fail("nn::init::Start not found");
    uint64_t ready = hle_register("host_loader_ready", loader_ready);
    uint64_t inits = hle_register("host_module_inits", module_inits);
    uint64_t finis = hle_register("host_module_finis", module_finis);
    fprintf(stderr, "[totk] ENTER nn::init::Start\n");
    GCALL(start, main_handle, 0, ready, inits, finis);
    runtime_fail("SDK Start unexpectedly returned");
}
