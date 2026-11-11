/* Starts the recompiled game the way MacNativeGames' loader.c boot() does: the main guest thread enters the SDK's own
 * nn::init::Start(main thread handle, 0, loader callback, module initializers, module finalizers).
 *   totk <exefs dir>
 */
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
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
// The game executable's folder (Windows: GetModuleFileName; Linux: /proc/self/exe), with its separator.
#ifdef _WIN32
#define SEP '\\'
static int exe_dir(char *path, size_t n) {
    DWORD k = GetModuleFileNameA(NULL, path, (DWORD)n);
    char *slash = k && k < n ? strrchr(path, SEP) : NULL;
    if (!slash) return 0;
    *slash = 0;
    return 1;
}
static int enter_dir(const char *path) { return SetCurrentDirectoryA(path) != 0; }
#define name_eq _stricmp
#else
#define SEP '/'
static int exe_dir(char *path, size_t n) {
    ssize_t k = readlink("/proc/self/exe", path, n - 1);
    if (k <= 0) return 0;
    path[k] = 0;
    char *slash = strrchr(path, SEP);
    if (!slash) return 0;
    *slash = 0;
    return 1;
}
static int enter_dir(const char *path) { return chdir(path) == 0; }
#define name_eq strcmp
#endif

/* The Steam Deck's defaults, for what ultratears.cfg leaves unset: its
   screen's 1280x800 (16:10: the camera and the UI follow, host/render_res.c), fullscreen; the frame limit is the
   launcher's, as on a PC (no 40 of its own since 2026-11-06, see below). The Deck: SteamDeck=1 (Steam sets it there) or Valve's board, product Jupiter
   (LCD) or Galileo (OLED). Any key the player set wins. */
static void deck_defaults(void) {
#ifndef _WIN32
    const char *env = getenv("SteamDeck");
    int deck = env && !strcmp(env, "1");
    // started by Steam on the Deck: SDL's text input opens Steam's on-screen keyboard (host/text_entry.c, the game's
    // software keyboard); only then, as without Steam the steam:// link would start Steam itself
    if (deck) setenv("SDL_ENABLE_STEAM_SCREEN_KEYBOARD", "1", 0);
    if (!deck) {
        char vendor[64] = "", product[64] = "";
        FILE *v = fopen("/sys/class/dmi/id/board_vendor", "r"), *p = fopen("/sys/class/dmi/id/product_name", "r");
        if (v && fgets(vendor, sizeof vendor, v) && p && fgets(product, sizeof product, p)) {
            vendor[strcspn(vendor, "\r\n")] = 0, product[strcspn(product, "\r\n")] = 0;
            deck = !strcmp(vendor, "Valve") && (!strcmp(product, "Jupiter") || !strcmp(product, "Galileo"));
        }
        if (v) fclose(v);
        if (p) fclose(p);
    }
    if (!deck) return;
    setenv("TOTK_OUTPUT_RES", "1280x800", 0);
    setenv("TOTK_WINDOW", "fullscreen", 0);
    // No frame limit of its own: the launcher's. Its 40 here made the launcher's 60 impossible to choose (2026-11-06);
    // a new game folder on the Deck starts with fps=40 in ultratears.cfg (tools/setup.py first_settings), a setting
    // like any other.
    // The recording threads below the game's (nice 5): the user's runs (2026-11-05, routes not the same) had p95 31.8
    // against 33.8 ms and fewer frames over 50 ms; a burst of uploads may take longer (one of 1.2 s in that run)
    setenv("TOTK_RECORD_NICE", "5", 0);
    // The Deck's buttons as labelled on it (A the bottom one: the Xbox layout, window_sdl.c binds_load), with the
    // prompts for it (ui_prompts.c); pad_layout=nintendo in ultratears.cfg (the launcher's Controls tab) keeps the Switch's
    setenv("TOTK_BIND_pad_layout", "xbox", 0);
    fprintf(stderr, "[totk] Steam Deck: output %s, window %s, %s button layout (the Deck's defaults where "
            "ultratears.cfg sets none)\n", getenv("TOTK_OUTPUT_RES"), getenv("TOTK_WINDOW"), getenv("TOTK_BIND_pad_layout"));
#endif
}

static const char *default_exefs(void) {
    const char *exefs = "data/exefs";
    char path[4096];
    if (!exe_dir(path, sizeof path)) return exefs;
    char cfg[4096 + 32];
    snprintf(cfg, sizeof cfg, "%s%cultratears.cfg", path, SEP);
    FILE *f = fopen(cfg, "r");
    char *slash;
    // build\UltraTears.exe (an old layout) or linux/ultratears (the Linux game beside the data on the PC): the folder above
    if (!f && (slash = strrchr(path, SEP)) && (!name_eq(slash + 1, "build") || !name_eq(slash + 1, "linux"))) {
        *slash = 0;
        snprintf(cfg, sizeof cfg, "%s%cultratears.cfg", path, SEP);
        f = fopen(cfg, "r");
    }
    if (!enter_dir(path)) fprintf(stderr, "[totk] cannot enter %s\n", path);
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
                {"resolution", "TOTK_OUTPUT_RES"}, {"scale", "TOTK_RENDER_SCALE"}, {"shadow_map", "TOTK_SHADOW_RES"},
                {"sharpness", "TOTK_SHARPNESS"}, {"upscale", "TOTK_UPSCALER"}, {"antialias", "TOTK_AA"},
                {"gpu_marks", "TOTK_VK_CHECKPOINTS"},   // the launcher's GPU markers (Debug tab)
                {"fps_overlay", "TOTK_FPS_OVERLAY"},   // the frame counter (F10 in the game)
                // the launcher's Sticks and gyro page (window_sdl.c dead_zone, motion_fusion.c)
                {"stick_deadzone_left", "TOTK_STICK_DEADZONE_LEFT"}, {"stick_deadzone_right", "TOTK_STICK_DEADZONE_RIGHT"},
                {"gyro_deadzone", "TOTK_GYRO_DEADZONE"},
                // the launcher's Keyboard page (mouse_direct.c)
                {"mouse_sens", "TOTK_MOUSE_SENS"}, {"mouse_aim", "TOTK_MOUSE_AIM_SCALE"}, {"mouse_invert", "TOTK_MOUSE_INVERT_Y"},
                // the launcher's Mods page (mods.c)
                {"mods_dir", "TOTK_MODS_DIR"}, {"mods_order", "TOTK_MODS_ORDER"}, {"mods_off", "TOTK_MODS_OFF"},
                {"cheats_on", "TOTK_CHEATS_ON"}};
            if (!strcmp(line, "exefs")) snprintf(g_exefs, sizeof g_exefs, "%s", eq + 1), exefs = g_exefs;
            else if (!strcmp(line, "snapshots")) {   // the launcher's "Extended debugging with snapshots": F9 saves the frame into diag\f<frame>
                if (!strcmp(eq + 1, "1")) setenv("TOTK_DUMP_KEY", "diag", 0);
            }
            else if (!strncmp(line, "TOTK_", 5)) setenv(line, eq + 1, 0);
            else if (!strncmp(line, "pad_", 4) || !strncmp(line, "key_", 4)) {   // the launcher's Controls tab (window_sdl.c binds_load)
                char var[96];
                if (snprintf(var, sizeof var, "TOTK_BIND_%s", line) < (int)sizeof var) setenv(var, eq + 1, 0);
            }
            else
                for (unsigned i = 0; i < sizeof keys / sizeof *keys; i++)
                    if (!strcmp(line, keys[i][0])) setenv(keys[i][1], eq + 1, 0);
        }
        fclose(f);
        fprintf(stderr, "[totk] game folder %s (%s)\n", path, cfg);
    } else
        fprintf(stderr, "[totk] game folder %s (no ultratears.cfg: setup.py's layout)\n", path);
    deck_defaults();
    return exefs;
}

/* A setting the player changes in the game (F10, the frame counter) kept in ultratears.cfg of the game folder (the
   working directory), so the next start and the launcher's Play screen show it: the key's line goes, and with a value
   "key=value" is added at the end. The other lines stay as they are. */
void totk_cfg_set(const char *key, const char *value) {
    FILE *f = fopen("ultratears.cfg", "rb");
    if (!f) return;
    char *in = NULL;
    size_t n = 0, cap = 0, klen = strlen(key);
    for (int c; (c = fgetc(f)) != EOF;) {
        if (n + 2 > cap) { cap = cap ? cap * 2 : 4096; char *t = realloc(in, cap); if (!t) { free(in); fclose(f); return; } in = t; }
        in[n++] = (char)c;
    }
    fclose(f);
    FILE *w = fopen("ultratears.cfg.new", "wb");
    if (!w) { free(in); return; }
    int ok = 1;
    for (size_t i = 0; i < n;) {
        size_t e = i;
        while (e < n && in[e] != '\n') e++;
        int mine = e - i > klen && !strncmp(in + i, key, klen) && in[i + klen] == '=';
        if (!mine) {
            ok &= fwrite(in + i, 1, e - i, w) == e - i;
            ok &= fputc('\n', w) != EOF;
        }
        i = e + 1;
    }
    if (*value) ok &= fprintf(w, "%s=%s\n", key, value) > 0;
    ok &= fclose(w) == 0;
    free(in);
#ifdef _WIN32
    if (ok && MoveFileExA("ultratears.cfg.new", "ultratears.cfg", MOVEFILE_REPLACE_EXISTING)) return;
#else
    if (ok && rename("ultratears.cfg.new", "ultratears.cfg") == 0) return;
#endif
    remove("ultratears.cfg.new");
}

int main(int argc, char **argv) {
    setvbuf(stderr, NULL, _IONBF, 0);
    fprintf(stderr, "UltraTears %s (commit %s)\n", ULTRS_VERSION, ULTRS_COMMIT);   // first line of every log
    // --debug: the statistics lines in the log (TOTK_STATS=1; the launcher's Debug mode writes debug=1 instead)
    // --prepare-shaders: every shader compiled by this GPU's driver ahead of play, then exit (gfx/vkb_shader.c)
    const char *arg = NULL;
    int prepare = 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--debug")) setenv("TOTK_STATS", "1", 1);
        else if (!strcmp(argv[i], "--prepare-shaders")) prepare = 1;
        else if (!arg) arg = argv[i];
        else { fprintf(stderr, "usage: UltraTears [--debug] [--prepare-shaders] [exefs dir]\n"); return 2; }
    const char *exefs = arg ? arg : default_exefs();
    if (prepare) { int vkb_prepare_shaders(void); return vkb_prepare_shaders(); }
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
    { void mods_code_load(void); mods_code_load(); }   // player mods' code patches and code cheats (mods.c)
    fprintf(stderr, "[totk] modules loaded and linked in %.0f ms\n", (runtime_now_ns() - t0) / 1e6);

    runtime_profiler_start();
    { void pgo_start(void); pgo_start(); }   // the PGO training build only (src/host/pgo.c)

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
