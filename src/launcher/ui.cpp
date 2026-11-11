// The launcher's screens (Nintendo/launcher-design): the dump folder, the build's progress, the hardware question, the
// failure, and Play once the game is built. Layout in design pixels (800x480), multiplied by the window's scale.
// Every clickable thing has a hover and a pressed look and the hand cursor; Tab / the gamepad move between them.
#include "launcher.h"
#include "imgui_internal.h"   // NavMoveRequestSubmit: the left stick moves the focus as the d-pad does
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

// ---------------------------------------------------------------- look
static const ImU32 C_BG = IM_COL32(12, 12, 12, 255);
static const ImU32 C_TEXT = IM_COL32(242, 242, 242, 255);
static const ImU32 C_SUB = IM_COL32(150, 150, 150, 255);
static const ImU32 C_DIM = IM_COL32(110, 110, 110, 255);
static const ImU32 C_CARD = IM_COL32(28, 28, 28, 255);
static const ImU32 C_CARD_HOVER = IM_COL32(36, 36, 36, 255);
static const ImU32 C_DARK = IM_COL32(46, 46, 46, 255);
static const ImU32 C_DARK_HOVER = IM_COL32(60, 60, 60, 255);
static const ImU32 C_DARK_DOWN = IM_COL32(38, 38, 38, 255);
static const ImU32 C_LIGHT = IM_COL32(236, 236, 236, 255);
static const ImU32 C_LIGHT_HOVER = IM_COL32(255, 255, 255, 255);
static const ImU32 C_LIGHT_DOWN = IM_COL32(210, 210, 210, 255);
static const ImU32 C_GREEN = IM_COL32(32, 122, 96, 255);
static const ImU32 C_GREEN_HOVER = IM_COL32(38, 145, 113, 255);
static const ImU32 C_GREEN_DOWN = IM_COL32(27, 104, 82, 255);
static const ImU32 C_BAR = IM_COL32(52, 52, 52, 255);
static const ImU32 C_BAR_FILL = IM_COL32(61, 214, 140, 255);
static const ImU32 C_BAR_FAIL = IM_COL32(96, 96, 96, 255);
static const ImU32 C_OK = IM_COL32(64, 179, 150, 255);
static const ImU32 C_ERR = IM_COL32(205, 109, 97, 255);
static const ImU32 C_PENDING = IM_COL32(58, 58, 58, 255);

static float S = 1;          // the window's scale
static ImVec2 O;             // where design pixel (0, 0) of the current region is on the screen
static ImDrawList *DL;
static SDL_Window *g_win;

static ImVec2 P(float x, float y) { return ImVec2(O.x + x * S, O.y + y * S); }

enum Style { DARK, LIGHT, PRIMARY, CARD };

static bool hovered_rect(ImVec2 a, ImVec2 b) {
    return ImGui::IsMouseHoveringRect(a, b) && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
}

static bool g_drag_scrolled;   // the body was dragged with a finger: the touch that did it clicks nothing (ui_frame)
static bool g_input_held;      // after a key / button was bound: nothing clicks until every key and button is up
static bool g_scrolling;       // the right stick held or a drag going on: frames at the display's rate (main.cpp)
static bool g_focus_play;      // the Play screen was just shown: its Play button takes the focus once it is there
bool ui_busy() { return g_scrolling; }

// An invisible ImGui item over [x, y, w, h] (design pixels): focus, keyboard and gamepad come with it.
static bool hit(const char *id, float x, float y, float w, float h, bool *hover, bool *down, bool enabled = true) {
    ImGui::SetCursorScreenPos(P(x, y));
    ImGui::PushID(id);
    if (!enabled)
        ImGui::BeginDisabled();
    // EnableNav: ImGui 1.91 leaves an invisible button out of the keyboard and gamepad navigation unless asked (without
    // it nothing on the launcher could be reached with the Steam Deck's controls)
    bool clicked = ImGui::InvisibleButton("##hit", ImVec2(w * S, h * S), ImGuiButtonFlags_EnableNav);
    if (!enabled)
        ImGui::EndDisabled();
    ImGui::PopID();
    *hover = enabled && ImGui::IsItemHovered();
    *down = enabled && ImGui::IsItemActive();
    if (*hover)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (enabled && ImGui::IsItemFocused() && ImGui::GetIO().NavVisible)   // keyboard / gamepad focus (the design, 7.1 / 7.2)
        DL->AddRect(P(x - 4, y - 4), P(x + w + 4, y + h + 4), IM_COL32(118, 196, 170, 255), 10 * S, 0, 2 * S);
    return clicked && !g_drag_scrolled && !g_input_held;
}

static ImVec2 text_size(ImFont *f, const char *t) { return f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, t); }

static void text(ImFont *f, float x, float y, ImU32 col, const char *t) { DL->AddText(f, f->FontSize, P(x, y), col, t); }

static float text_w(ImFont *f, const char *t) { return text_size(f, t).x / S; }   // in design pixels

// A path within width (design pixels): its middle left out ("D:\Games\…\game"); the whole path on hover.
static void path_text(ImFont *f, float x, float y, float width, const std::string &path, float *drawn_w = nullptr) {
    std::string s = path;
    if (text_w(f, s.c_str()) > width) {
        std::vector<size_t> cuts;   // where UTF-8 characters start
        for (size_t i = 0; i < path.size(); i++)
            if ((path[i] & 0xC0) != 0x80)
                cuts.push_back(i);
        size_t head = cuts.size() / 2, tail = cuts.size() / 2;
        while (head > 1) {
            s = path.substr(0, cuts[head]) + "…" + path.substr(cuts[cuts.size() - tail]);
            if (text_w(f, s.c_str()) <= width)
                break;
            head--, tail = tail > 1 ? tail - 1 : 1;
        }
        if (hovered_rect(P(x, y), P(x + width, y + f->FontSize / S)))
            ImGui::SetTooltip("%s", path.c_str());
    }
    text(f, x, y, C_TEXT, s.c_str());
    if (drawn_w)
        *drawn_w = text_w(f, s.c_str());
}

static bool button(const char *id, const char *label, float x, float y, float w, float h, Style st, bool enabled = true) {
    bool hov, down;
    bool clicked = hit(id, x, y, w, h, &hov, &down, enabled);
    ImU32 bg, fg = C_TEXT;
    switch (st) {
    case LIGHT: bg = down ? C_LIGHT_DOWN : hov ? C_LIGHT_HOVER : C_LIGHT; fg = IM_COL32(17, 17, 17, 255); break;
    case PRIMARY: bg = down ? C_GREEN_DOWN : hov ? C_GREEN_HOVER : C_GREEN; break;
    default: bg = down ? C_DARK_DOWN : hov ? C_DARK_HOVER : C_DARK; break;
    }
    if (!enabled)
        bg = C_DARK, fg = C_DIM;
    DL->AddRectFilled(P(x, y), P(x + w, y + h), bg, 8 * S);
    ImFont *f = g_media.medium;
    ImVec2 ts = text_size(f, label);
    DL->AddText(f, f->FontSize, ImVec2(P(x, y).x + (w * S - ts.x) / 2, P(x, y).y + (h * S - ts.y) / 2), fg, label);
    return clicked;
}

static void icon(const Icon &i, float x, float y, float size = 0) {
    if (!i.tex)
        return;
    float w = size ? size : i.w, h = size ? size : i.h;
    DL->AddImage((ImTextureID)(intptr_t)i.tex, P(x, y), P(x + w, y + h));
}

// A paragraph of runs, wrapped at word breaks within width; runs with a url are links (underlined on hover, open the
// url). Returns its height in design pixels.
struct Run { const char *text; ImU32 col; const char *url = nullptr; };

static float rich(ImFont *f, float x, float y, float width, std::initializer_list<Run> runs, float line_h = 0) {
    float lh = line_h ? line_h : font_line(f);
    float lead = (lh - f->FontSize / S) / 2;   // as Figma: the glyphs centred in their line
    float cx = 0, cy = 0, space = text_w(f, " ");
    bool trailing = false;   // the run before ended with a space
    for (const Run &r : runs) {
        struct Word { float x, y, w; std::string s; };
        std::vector<Word> words;
        const char *p = r.text;
        bool lead_space = *p == ' ' || trailing;
        size_t len = SDL_strlen(p);
        trailing = len && p[len - 1] == ' ';
        while (*p) {
            while (*p == ' ')
                p++;
            if (!*p)
                break;
            const char *e = p;
            while (*e && *e != ' ')
                e++;
            std::string w(p, e);
            float ww = text_w(f, w.c_str());
            bool first = words.empty();
            float gap = (cx > 0 && (!first || lead_space)) ? space : 0;
            if (cx > 0 && cx + gap + ww > width) {
                cx = 0, cy += lh, gap = 0;
            }
            words.push_back({cx + gap, cy, ww, w});
            cx += gap + ww;
            p = e;
        }
        bool hov = false;
        if (r.url)
            for (const Word &w : words)
                if (hovered_rect(P(x + w.x, y + w.y), P(x + w.x + w.w, y + w.y + lh)))
                    hov = true;
        for (const Word &w : words) {
            text(f, x + w.x, y + w.y + lead, r.col, w.s.c_str());
            float base = y + w.y + lead + f->Ascent / S + 1.5f;   // under the baseline
            if (r.url && hov)
                DL->AddLine(P(x + w.x, base), P(x + w.x + w.w, base), r.col, S);
        }
        if (r.url && hov) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                SDL_OpenURL(r.url);
        }
    }
    return cy + lh;
}

static void ring(float cx, float cy, float r, ImU32 col) { DL->AddCircle(P(cx, cy), r * S, col, 32, 1.6f * S); }
static void dot(float cx, float cy, float r, ImU32 col) { DL->AddCircleFilled(P(cx, cy), r * S, col, 32); }

static void globe(float x, float y, ImU32 col) {   // 14x14: a circle, the equator, a meridian ellipse
    ImVec2 c = P(x + 7, y + 7);
    float r = 6.2f * S, t = 1.2f * S;
    DL->AddCircle(c, r, col, 32, t);
    DL->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), col, t);
    DL->AddEllipse(c, ImVec2(r * 0.45f, r), col, 0, 32, t);
}

// ---------------------------------------------------------------- state
enum Screen { SCR_DUMP, SCR_BUILD, SCR_HW, SCR_PLAY, SCR_UPDATE, SCR_BROKEN };
enum DumpState { D_NONE, D_CHECKING, D_BAD, D_OK, D_DATA };   // D_DATA: the game data already in data/
enum RowState { R_PENDING, R_RUN, R_DONE, R_WARN, R_ERROR };

enum { NSTEP = 10 };   // setup.py's steps (STEPS) and its tools
struct Row { const char *id, *name; RowState st = R_PENDING; double secs = -1; bool skipped = false; std::string err; };

static struct {
    Screen screen = SCR_DUMP;
    Settings set;
    std::string home;          // the package the launcher was started from (extracted from the zip)
    std::string install;       // where the game is built: home, or a folder the player chose (the package copied there)
    std::string root;          // the package that builds and updates: install once the copy is there, else home
    std::string dump;          // the folder with exefs and romfs (settings: dump)
    std::string version;       // of the package (VERSION.txt): x.y.z, for updates
    std::string version_line;  // as VERSION.txt and the game's log write it: "0.2.0 (commit abc1234)"
    // the dump folder screen
    DumpState dump_st = D_NONE;
    std::string dump_code;
    Json dump_args;
    SetupProc *check = nullptr;
    std::string checking;
    // the build
    SetupProc *build = nullptr;
    SetupProc *copy = nullptr;   // tools/install_to.py: the package into the install folder, before the build
    Row rows[7] = {{"check", "step_check"}, {"romfs", "step_romfs"}, {"lift", "step_lift"},
                   {"compile", "step_compile"}, {"shaders", "step_shaders"}, {"textures", "step_textures"},
                   {"deck", "step_deck"}};   // deck: shown in a build for the Steam Deck only
    // graphics: the shaders compiled by the driver (setup.py), in the shaders row ("Preparing the graphics") after them
    double weight[NSTEP] = {};
    const char *weight_id[NSTEP] = {"tools", "check", "romfs", "lift", "compile", "shaders", "graphics", "textures", "finish", "deck"};
    double frac[NSTEP] = {};  // each setup step's share done, 0..1
    double eta = -1;          // of the running step, from its progress events
    bool failed = false, done = false, cancel_q = false, close_after_cancel = false;
    std::string fail_text;
    Json hw;                  // the hardware cases
    bool hw_asked = false;
    // folder dialogs answer on another thread
    SDL_Mutex *lock = nullptr;
    std::string picked;
    int picked_for = 0;       // 1 the dump, 2 the install folder
    bool stopping = false;    // Stop pressed: the build is being killed
    bool full = false;        // the next build is a full rebuild (setup.py --full)
    std::string platform;     // what is built: windows, linux (the Steam Deck: a tar to carry there) or both
    bool can_build = true;    // a builder beside the launcher (not so in the Steam Deck's tar)
    // updates
    UpdateInfo upd;
    bool has_upd = false;
    int upd_st = 0;           // 0 asked to update? 1 downloading, 2 putting in place, 3 failed
    std::string upd_err;
    SetupProc *apply = nullptr;
    double apply_frac = 0;
} U;

static std::string log_path() { return path_join(U.install, "ultratears-setup.log"); }
static bool installed() { return !game_exe(U.install).empty() && path_is_file(path_join(U.install, "ultratears.cfg")); }
#ifdef _WIN32
static bool for_deck() { return U.platform == "linux" || U.platform == "both"; }
#else
static bool for_deck() { return false; }   // on Linux the game is built in place, for this machine: nothing to carry
#endif
static std::string deck_tar() { return path_join(U.install, "UltraTears-SteamDeck.tar"); }
static bool deck_built() { return path_is_file(deck_tar()); }   // a build for the Steam Deck alone has no exe here
static bool started_before() { return path_is_file(path_join(U.install, "data/setup.json")) && !installed() && !deck_built(); }
static int n_rows() { return for_deck() ? 7 : 6; }
// a builder package without the Linux kit it came with (tools/make_package.py): damaged, a build for the Deck cannot run
static bool linux_kit_gone() {
    return for_deck() && path_is_file(path_join(U.root, "package-files.txt")) && !path_is_dir(path_join(U.root, "deps/linux/sysroot"));
}
// The Steam Deck's tar carries the Linux builder and the AppImage of this release, downloaded once (tools/deps_install.py
// linux_package, named in the package's deps/linux-package.json): not downloaded yet, the screen says so.
static bool linux_package_missing() {
    size_t n = 0;
    char *s = (char *)SDL_LoadFile(path_join(U.root, "deps/linux-package.json").c_str(), &n);
    if (!s)
        return true;
    Json pins;
    json_parse(std::string(s, n), pins);
    SDL_free(s);
    for (const char *k : {"builder", "appimage"})
        if (!path_is_file(path_join(path_join(U.root, "deps/downloads"), pins[k]["name"].str())))
            return true;
    return false;
}

static Json setup_state() {   // data/setup.json of the install folder: setup.py's record of the build
    Json st;
    size_t n = 0;
    char *s = (char *)SDL_LoadFile(path_join(U.install, "data/setup.json").c_str(), &n);
    if (s) {
        json_parse(std::string(s, n), st);
        SDL_free(s);
    }
    return st;
}

// built by another package than the one in the install folder (an update came since): the next build redoes only what
// changed. setup.py records the package with the build (data/setup.json "package"), so any launcher, the one the
// player extracted or the copy in the install folder, sees the same.
// The builder is the install folder's package, else the launcher's own (U.root): on Linux the Steam Deck's tar carries
// no builder, the AppImage is it, so a newer AppImage put in the folder makes the build stale. A folder of an older
// layout (a builder copied in) is stale too once the AppImage that runs is a newer release than that copy.
static bool has_package(const std::string &folder);
// the package in `from` replaces the one in `to`: another build (the whole VERSION.txt line, so a rebuild with the same
// number, 0.2.0-41-g... then 0.2.0, counts too), only never an older release over a newer one
static bool package_replaces(const std::string &from, const std::string &to) {
    std::string a = package_version_line(from), b = package_version_line(to);
    return !a.empty() && a != b && !version_newer(package_version(to), package_version(from));
}
static bool stale() {
    std::string line = package_version_line(U.root);
#ifdef _WIN32
    bool newer = false;   // the PC's launcher is in its package: an update replaces it with the package (apply_update.py)
#else
    bool newer = has_package(U.home) && has_package(U.install) && package_replaces(U.home, U.install);
#endif
    return installed() && ((!line.empty() && setup_state()["package"].str() != "UltraTears " + line) || newer);
}

// The game's files are already in data\ of the install folder (setup.py packed the RomFS and took the ExeFS): the
// dump is no longer needed, and setup.py checks that data itself (step check) before it builds on it.
static bool data_ready() {
    bool ok = !setup_state()["romfs"].str().empty();
    for (const char *f : {"data/romfs/romfs.bin", "data/romfs/runtime-index.bin", "data/exefs/main"})
        ok = ok && path_is_file(path_join(U.install, f));
    return ok;
}

static bool same_path(std::string a, std::string b) {
    for (std::string *s : {&a, &b}) {
        for (char &c : *s)
            c = c == '/' ? '\\' : (char)SDL_tolower((unsigned char)c);
        while (s->size() > 3 && s->back() == '\\')
            s->pop_back();
    }
    return a == b;
}

static bool has_package(const std::string &folder) { return path_is_file(path_join(folder, "tools/setup.py")); }

// the install folder has no package yet, or an older one than the launcher's own: tools/install_to.py first
static bool needs_copy() {
    if (same_path(U.install, U.home))
        return false;
    return !has_package(U.install) || package_replaces(U.home, U.install);
}

// GB a build needs free: the game, the package copy when it goes elsewhere, and on Windows for the Steam Deck its tar,
// game and builder (for_deck: never on Linux, where the game is built in place). The numbers are tools/disk_space.json
// of the launcher's package, as tools/hw_check.py, which stops a build below them, reads them.
static double need_gb() {
    static double build = -1, copy = 1, deck = 36;
    if (build < 0) {
        build = 40;
        size_t n = 0;
        char *s = (char *)SDL_LoadFile(path_join(U.home, "tools/disk_space.json").c_str(), &n);
        if (s) {
            Json d;
            json_parse(std::string(s, n), d);
            SDL_free(s);
            build = d["build_gb"].num(40), copy = d["copy_gb"].num(1), deck = d["deck_gb"].num(36);
        }
    }
    return build + (needs_copy() ? copy : 0) + (for_deck() ? deck : 0);
}

static void check_dump(const std::string &path);

// Linux: a game folder the AppImage builds in carries no tools of its own: its deps/ is a link to the running AppImage's
// (tools/install_to.py makes it), whose mount point changes at every start, so the link is made again before anything
// looks at it. A deps/ folder of its own (a folder of the older layout) stays until install_to.py replaces it.
static void link_deps() {
#ifndef _WIN32
    const char *appdir = SDL_getenv("APPDIR");
    if (!appdir || !*appdir || U.home == U.install || !path_is_file(path_join(U.install, "tools/setup.py")))
        return;
    std::string link = path_join(U.install, "deps"), target = path_join(U.home, "deps"), tmp = link + ".new";
    struct stat st;
    if (lstat(link.c_str(), &st) == 0 && !S_ISLNK(st.st_mode))
        return;
    char now[4096];
    ssize_t n = readlink(link.c_str(), now, sizeof now - 1);
    if (n > 0 && std::string(now, (size_t)n) == target)
        return;
    unlink(tmp.c_str());
    if (symlink(target.c_str(), tmp.c_str()) == 0 && rename(tmp.c_str(), link.c_str()) == 0)
        SDL_Log("deps: %s -> %s", link.c_str(), target.c_str());
    else
        SDL_Log("deps: cannot link %s to %s", link.c_str(), target.c_str());
#endif
}

static void use_install(const std::string &folder) {
    U.install = folder;
    U.root = has_package(folder) ? folder : U.home;
    U.version = package_version(U.root);
    U.version_line = package_version_line(U.root);
}

// Where an update is downloaded (its update/ folder) and, on Linux, where the new UltraTears.AppImage goes. The package
// folder, unless that is the running AppImage's mount (a game folder without a builder of its own: U.root = U.home),
// which is read-only (GitHub #74: "Couldn't open /tmp/.mount_.../update/UltraTears.AppImage.part"): then the game
// folder, where the new AppImage, started, brings the folder up to date itself.
static std::string update_folder() {
#ifndef _WIN32
    const char *appdir = SDL_getenv("APPDIR");
    if (appdir && *appdir && same_path(U.root, U.home))
        return U.install;
#endif
    return U.root;
}

static void dump_state() {   // what the dump screen shows for this install folder
    if (data_ready())
        U.dump_st = D_DATA;
    else if (!U.dump.empty())
        check_dump(U.dump);
    else
        U.dump_st = D_NONE;
}

static SDL_EnumerationResult SDLCALL count_entry(void *n, const char *, const char *) {
    ++*(int *)n;
    return SDL_ENUM_SUCCESS;   // one is enough
}

static bool folder_empty(const std::string &folder) {
    int n = 0;
    SDL_EnumerateDirectory(folder.c_str(), count_entry, &n);
    return n == 0;
}

static void save() {
    U.set.set("lang", lang_code(g_lang));
    U.set.set("dump", U.dump);
    settings_save(U.set);
}

static void check_dump(const std::string &path) {
    if (U.check) {
        setup_free(U.check);
        U.check = nullptr;
    }
    std::string py = python_path(U.root);
    U.checking = path;
    U.dump_st = D_CHECKING;
    if (py.empty()) {
        U.dump_st = D_BAD, U.dump_code = "no_python", U.dump_args = Json();
        return;
    }
    U.check = setup_start({py, path_join(U.root, "tools/setup.py"), "--check-dump", path, "--json"}, U.root, "");
}

static void SDLCALL folder_picked(void *which, const char *const *files, int) {
    if (!files || !files[0])
        return;
    SDL_LockMutex(U.lock);
    U.picked = files[0];
    U.picked_for = (int)(intptr_t)which;
    SDL_UnlockMutex(U.lock);
}

// ---- folders chosen inside the launcher, with the pad: in the Steam Deck's Game Mode (gamescope) there is no desktop
// to show the system's folder dialog (SDL asks xdg-desktop-portal). ULTRS_OWN_PICKER=1: this one elsewhere too (to try).
static struct { bool open = false; int which = 0; std::string dir; std::vector<std::string> subs; } FP;
static bool own_picker() {
#ifdef _WIN32
    return false;
#else
    const char *d = SDL_getenv("XDG_CURRENT_DESKTOP"), *f = SDL_getenv("ULTRS_OWN_PICKER");
    return (f && *f == '1') || (d && SDL_strcasecmp(d, "gamescope") == 0);
#endif
}
static SDL_EnumerationResult SDLCALL fp_entry(void *, const char *, const char *name) {
    SDL_PathInfo i;
    if (name[0] != '.' && SDL_GetPathInfo(path_join(FP.dir, name).c_str(), &i) && i.type == SDL_PATHTYPE_DIRECTORY)
        FP.subs.push_back(name);
    return SDL_ENUM_CONTINUE;
}
static void fp_enter(std::string dir) {
    while (dir.size() > 1 && dir.back() == '/')
        dir.pop_back();
    FP.dir = dir.empty() ? "/" : dir;
    FP.subs.clear();
    SDL_EnumerateDirectory(FP.dir.c_str(), fp_entry, nullptr);
    std::sort(FP.subs.begin(), FP.subs.end());
}
static std::string home_dir() {
    const char *h = SDL_getenv("HOME");
    return h && *h ? h : "/";
}
static void folder_picker_frame() {
    if (!FP.open)
        return;
    if (!ImGui::IsPopupOpen("##folders"))
        ImGui::OpenPopup("##folders");
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(720 * S, 420 * S));
    if (!ImGui::BeginPopupModal("##folders", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove))
        return;
    ImGui::PushFont(g_media.regular);
    ImGui::TextUnformatted(T("pick_title"));
    ImGui::PushStyleColor(ImGuiCol_Text, C_SUB);
    ImGui::TextWrapped("%s", FP.dir.c_str());
    ImGui::PopStyleColor();
    std::string go;
    if (ImGui::Button(T("pick_up")))
        go = FP.dir == "/" ? "/" : FP.dir.substr(0, SDL_max(FP.dir.find_last_of('/'), (size_t)1));
    ImGui::SameLine();
    if (ImGui::Button(T("pick_home")))
        go = home_dir();
    ImGui::SameLine();
    if (ImGui::Button(T("pick_media")))   // the SD card and USB drives (SteamOS mounts them there)
        go = path_is_dir("/run/media/deck") ? "/run/media/deck" : "/run/media";
    float bottom = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginChild("subs", ImVec2(0, -bottom), ImGuiChildFlags_Borders);
    if (FP.subs.empty())
        ImGui::TextDisabled("%s", T("pick_empty"));
    for (const std::string &n : FP.subs)
        if (ImGui::Selectable(n.c_str()))   // A on the pad, a click: into it
            go = path_join(FP.dir, n);
    ImGui::EndChild();
    if (ImGui::Button(T("pick_this"))) {
        SDL_LockMutex(U.lock);
        U.picked = FP.dir;
        U.picked_for = FP.which;
        SDL_UnlockMutex(U.lock);
        FP.open = false;
    }
    ImGui::SameLine();
    if (ImGui::Button(T("cancel")) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        FP.open = false;
    ImGui::PopFont();
    if (!FP.open)
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    if (!go.empty())
        fp_enter(go);
}

static void pick_folder(int which, const std::string &start) {
    if (own_picker()) {
        FP.open = true;
        FP.which = which;
        fp_enter(!start.empty() && path_is_dir(start) ? start : home_dir());
        return;
    }
    SDL_ShowOpenFolderDialog(folder_picked, (void *)(intptr_t)which, g_win, start.empty() ? nullptr : start.c_str(), false);
}

// ---------------------------------------------------------------- the build
static int setup_index(const std::string &id) {
    for (int i = 0; i < NSTEP; i++)
        if (id == U.weight_id[i])
            return i;
    return -1;
}

static Row *row_of(const std::string &id) {   // tools and check share the first row; finish belongs to the last
    if (id == "tools" || id == "check")
        return &U.rows[0];
    if (id == "finish")
        return &U.rows[5];
    if (id == "graphics")
        return &U.rows[4];
    for (Row &r : U.rows)
        if (id == r.id)
            return &r;
    return nullptr;
}

// The time of a build from nothing: measured on this PC once it built the game from nothing (setup.py records it,
// data/setup.json "full_secs"), else the design's 30-40 minutes (a clean build took 34 on 8 CPUs, 2026-10-30): a guess
// from the CPU count alone promised 15-30 where a slow disk or an antivirus can make it 50. One rule for the install
// screen, the full rebuild and the steps' "may take".
// A build for the Steam Deck as well: the game compiled twice, the Deck's shaders, the tar (setup.py step_weights:
// 162 of 100 for both platforms, 106 for the Deck alone); measured on this PC for a Windows build
// for a platform choice; "" the one chosen (U.platform)
static double deck_factor(const std::string &p = "") {
#ifndef _WIN32
    (void)p;
    return 1;   // on Linux the game is built for this machine alone
#else
    const std::string &q = p.empty() ? U.platform : p;
    return q == "both" ? 1.62 : q == "linux" ? 1.06 : 1;
#endif
}
// The measured time as a build for Windows alone (a build with the Deck divided back by its factor)
static double windows_secs() {
    Json st = setup_state();
    double secs = st["full_secs"].num(0);
    bool win = false, lin = false;
    for (size_t i = 0; i < st["platforms"].size(); i++)
        win |= st["platforms"][i].str() == "windows", lin |= st["platforms"][i].str() == "linux";
    return secs / deck_factor(lin ? (win ? "both" : "linux") : "windows");
}
static double measured_secs(const std::string &p = "") { return windows_secs() * deck_factor(p); }
static double est_total_secs() { return measured_secs() > 0 ? measured_secs() : 35 * 60 * deck_factor(); }

// In minutes, rounded to 5, of a build from nothing for platform p ("" the one chosen; the Debug tab's card: "windows",
// the platform being picked after it in its list).
static void wait_range(long *lo, long *hi, const std::string &p = "") {
    double m = measured_secs(p) / 60;
    if (m <= 0)
        m = 35 * deck_factor(p);
    *lo = SDL_max((long)(m * 0.9 / 5) * 5, 5L);
    *hi = SDL_max(((long)(m * 1.15) + 4) / 5 * 5, *lo + 5);
}

static void start_build_state() {
    for (Row &r : U.rows)
        r.st = R_PENDING, r.secs = -1, r.skipped = false, r.err.clear();
    static const double defaults[NSTEP] = {2, 1, 7, 4, 47, 9, 4, 30, 0, 0};   // setup.py's STEP_WEIGHTS, until its plan arrives
    for (int i = 0; i < NSTEP; i++)
        U.weight[i] = defaults[i], U.frac[i] = 0;
    U.eta = -1, U.failed = U.done = U.cancel_q = U.hw_asked = U.stopping = false;
    U.fail_text.clear();
    U.hw = Json();
}

static void run_setup() {
    std::string py = python_path(U.root);
    if (py.empty()) {
        U.screen = SCR_BUILD;
        U.failed = true;
        U.fail_text = T("no_python");
        return;
    }
    const char *cpus = SDL_getenv("ULTRS_LAUNCHER_CPUS");   // development: test builds leave the PC half free
    std::vector<std::string> args = {py, path_join(U.root, "tools/setup.py"), "--json",
                                     "--cpus", cpus && *cpus ? cpus : std::to_string(logical_cpus())};
    if (U.dump_st != D_DATA)   // with the data already in place setup.py needs no dump (a deleted one is fine)
        args.insert(args.end(), {"--dump", U.dump});
    if (U.full)   // the Debug tab's full rebuild: once
        args.push_back("--full");
    args.insert(args.end(), {"--platform", U.platform});
    U.full = false;
    U.build = setup_start(args, U.root, log_path());
    U.screen = SCR_BUILD;
}

static void start_build() {
    start_build_state();
    SDL_CreateDirectory(U.install.c_str());
    U.screen = SCR_BUILD;
    if (!needs_copy()) {
        run_setup();
        return;
    }
    U.rows[0].st = R_RUN;   // the copy counts as the first step's "tools" part
    U.copy = setup_start({python_path(U.home), path_join(U.home, "tools/install_to.py"), U.install}, U.home, log_path());
}

static void on_event(const Json &e) {
    std::string ev = e["ev"].str();
    if (ev == "plan") {
        const Json &steps = e["steps"];
        for (size_t i = 0; i < steps.size(); i++) {
            int k = setup_index(steps[i]["id"].str());
            if (k >= 0)
                U.weight[k] = steps[i]["weight"].num();
        }
    } else if (ev == "step") {
        std::string id = e["id"].str(), st = e["state"].str();
        int k = setup_index(id);
        Row *r = row_of(id);
        if (st == "run") {
            U.eta = -1;
            if (r && r->st == R_PENDING)
                r->st = R_RUN;
        } else {
            if (k >= 0)
                U.frac[k] = 1;
            // tools, shaders and finish only add their time: the row's last step (check, graphics, textures) says how it
            // went (skipped: when the shaders were too)
            if (r) {
                r->secs = (r->secs > 0 ? r->secs : 0) + e["secs"].num();
                if (id == "shaders")
                    r->skipped = st == "skip";
                else if (id != "tools" && id != "finish") {
                    r->st = r->st == R_WARN ? R_WARN : R_DONE;
                    r->skipped = st == "skip" && (id != "graphics" || r->skipped);
                }
            }
        }
    } else if (ev == "progress") {
        for (int k = 0; k < NSTEP; k++) {
            Row *r = row_of(U.weight_id[k]);
            if (r && r->st == R_RUN && U.frac[k] < 1 && (k != 0 || U.rows[0].st != R_DONE)) {
                double total = e["total"].num();
                if (total > 0) {
                    // the running setup step: the first one not yet done whose row runs
                    U.frac[k] = SDL_min(0.999, e["done"].num() / total);
                    U.eta = e["eta"].kind == Json::Num ? e["eta"].num() : -1;
                }
                break;
            }
        }
    } else if (ev == "hw") {
        U.hw = e["cases"];
        for (size_t i = 0; i < U.hw.size(); i++)
            if (U.hw[i]["sev"].str() != "info")
                U.rows[0].st = R_WARN;
    } else if (ev == "ask" && e["id"].str() == "hw") {
        U.hw_asked = true;
        U.screen = SCR_HW;
    } else if (ev == "error") {
        U.failed = true;
        std::string code = e["code"].str();
        Row *r = row_of(e["id"].str());
        std::string t = has_text(code.c_str()) ? Tf(code.c_str(), e["args"]) : T("error_generic");
        if (r) {
            r->st = R_ERROR;
            r->err = t;
        }
        if (code == "hw_declined") {   // the player said no at the hardware question: back to the start
            U.failed = false;
            U.screen = SCR_DUMP;
        }
    } else if (ev == "done") {
        U.done = true;   // setup.py recorded the package with the build (data/setup.json): Play from now on
        for (Row &r : U.rows)
            if (r.st == R_PENDING || r.st == R_RUN)
                r.st = R_DONE;
        for (double &f : U.frac)
            f = 1;
    }
}

static void poll_update() {
    UpdateInfo found;
    if (update_check_poll(&found) == 1 && found.version != U.version) {
        U.upd = found;
        U.has_upd = true;
        U.set.set("update_version", found.version), U.set.set("update_name", found.name);
        U.set.set("update_url", found.url), U.set.set("update_sha256", found.sha256);
        U.set.set("update_size", std::to_string(found.size)), U.set.set("update_page", found.page);
        save();
        if (SDL_getenv("ULTRS_LAUNCHER_DEMO") && SDL_strcmp(SDL_getenv("ULTRS_LAUNCHER_DEMO"), "update") == 0) {
            U.screen = SCR_UPDATE, U.upd_st = 1;   // development: the whole update without a click
            update_download_start(U.upd, path_join(update_folder(), "update"));
        }
    }
    if (U.upd_st == 1) {
        std::string res;
        int st = update_download_poll(nullptr, nullptr, &res);
        if (st == 3) {
            U.upd_st = 3, U.upd_err = res;
        } else if (st == 2) {   // checked: put it in place with the package's Python, the launcher restarts after
            U.upd_st = 2, U.apply_frac = 0;
#ifdef _WIN32
            U.apply = setup_start({python_path(U.root), path_join(U.root, "tools/apply_update.py"), res, "--root", U.root},
                                  U.root, path_join(U.root, "update/update.log"));
#else
            std::string err;   // the new AppImage in the game folder, started: it updates the folder itself
            if (appimage_update(res, update_folder(), err)) {
                SDL_Event q = {};
                q.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&q);
            } else {
                U.upd_st = 3, U.upd_err = err;
            }
#endif
        }
    }
    if (U.apply) {
        std::string line;
        bool ok = false;
        while (setup_line(U.apply, line)) {
            Json e;
            if (line.empty() || line[0] != '{' || !json_parse(line, e))
                continue;
            std::string ev = e["ev"].str();
            if (ev == "progress" && e["total"].num() > 0)
                U.apply_frac = e["done"].num() / e["total"].num();
            else if (ev == "error")
                U.upd_err = e["message"].str();
            else if (ev == "done")
                ok = true;
        }
        static bool applied = false;
        applied = applied || ok;
        int code;
        if (setup_finished(U.apply, &code)) {
            setup_free(U.apply);
            U.apply = nullptr;
            if (applied && code == 0 && restart_self()) {
                SDL_Event q = {};
                q.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&q);
            } else {
                U.upd_st = 3;
                if (U.upd_err.empty())
                    U.upd_err = applied ? T("upd_restart") : "apply_update.py: exit code " + std::to_string(code);
            }
            applied = false;
        }
    }
}

static void poll() {
    poll_update();
    SDL_LockMutex(U.lock);
    std::string picked = U.picked;
    int which = U.picked_for;
    U.picked.clear();
    U.picked_for = 0;
    SDL_UnlockMutex(U.lock);
    if (which == 1)
        check_dump(picked);
    if (which == 2) {   // a folder with other things in it: the game goes into UltraTears inside it
        std::string f = picked;
        if (!same_path(f, U.home) && !has_package(f) && !folder_empty(f))
            f = path_join(f, "UltraTears");
        use_install(same_path(f, U.home) ? U.home : f);
        U.set.set("install", same_path(f, U.home) ? "" : f);
        save();
        if (U.dump_st == D_DATA || data_ready())
            dump_state();
    }

    if (U.copy) {
        std::string line;
        while (setup_line(U.copy, line)) {
            Json e;
            if (line.empty() || line[0] != '{' || !json_parse(line, e))
                continue;
            std::string ev = e["ev"].str();
            if (ev == "progress" && e["total"].num() > 0)
                U.frac[0] = SDL_min(0.999, e["done"].num() / e["total"].num());
            else if (ev == "error")
                on_event(e);
        }
        int code;
        if (setup_finished(U.copy, &code)) {
            setup_free(U.copy);
            U.copy = nullptr;
            if (code == 0 && !U.failed && has_package(U.install)) {
                use_install(U.install);   // from now on the copy builds, updates and is played
                run_setup();
            } else if (!U.stopping) {
                U.failed = true;
                U.rows[0].st = R_ERROR;
                U.rows[0].err = Tf("install_copy", {{"path", U.install}});
            }
        }
    }

    if (U.check) {
        std::string line;
        while (setup_line(U.check, line)) {
            Json e;
            if (json_parse(line, e) && e["ev"].str() == "dump") {
                if (e["ok"].truth()) {
                    U.dump_st = D_OK;
                    U.dump = e["path"].str();
                    save();
                } else {
                    U.dump_st = D_BAD;
                    U.dump_code = e["code"].str();
                    U.dump_args = e["args"];
                    U.checking = e["path"].str(U.checking.c_str());
                }
            }
        }
        int code;
        if (setup_finished(U.check, &code)) {
            if (U.dump_st == D_CHECKING) {   // no answer at all: Python missing or broken
                U.dump_st = D_BAD;
                U.dump_code = "no_python";
                U.dump_args = Json();
            }
            setup_free(U.check);
            U.check = nullptr;
        }
    }
    if (U.build) {
        std::string line;
        while (setup_line(U.build, line)) {
            Json e;
            if (!line.empty() && line[0] == '{' && json_parse(line, e))
                on_event(e);
        }
        int code;
        if (setup_finished(U.build, &code)) {
            if (!U.done && !U.failed && U.screen != SCR_DUMP) {
                U.failed = true;
                U.fail_text = Tf("setup_crash", {{"code", std::to_string(code)}});
                for (Row &r : U.rows)
                    if (r.st == R_RUN) {
                        r.st = R_ERROR;
                        r.err = T("error_generic");
                    }
            }
            setup_free(U.build);
            U.build = nullptr;
        }
    }
}

static double overall() {
    double sum = 0, done = 0;
    for (int k = 0; k < NSTEP; k++)
        sum += U.weight[k], done += U.weight[k] * U.frac[k];
    return sum > 0 ? done / sum : 0;
}

// ---------------------------------------------------------------- screens
static void update_banner(float right);

static void header(bool lang) {
    icon(g_media.emblem, 36, 20);
    ImFont *b = g_media.brand;
    text(b, 86, 40 - b->FontSize / S / 2, C_TEXT, "UltraTears");
    if (!lang)
        return;
    const char *name = lang_name(g_lang);
    float w = 26 + 14 + 6 + text_w(g_media.medium, name) + 4 + 14;
    float x = 764 - w;
    update_banner(x);
    bool hov, down;
    if (hit("lang", x, 24, w, 32, &hov, &down))
        ImGui::OpenPopup("langs");
    bool open = ImGui::IsPopupOpen("langs");
    DL->AddRectFilled(P(x, 24), P(764, 56), down ? C_DARK_DOWN : hov || open ? C_DARK_HOVER : C_DARK, 6 * S);
    globe(x + 13, 33, C_TEXT);
    text(g_media.medium, x + 13 + 14 + 6, 40 - g_media.medium->FontSize / S / 2, C_TEXT, name);
    icon(g_media.chevron, 764 - 13 - 14, 33, 14);
    ImGui::SetNextWindowPos(P(x, 60));
    ImGui::SetNextWindowSize(ImVec2((764 - x) * S, (8 + 30 * LANG_COUNT) * S));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (ImGui::BeginPopup("langs", ImGuiWindowFlags_NoScrollbar)) {
        ImDrawList *keep = DL;
        ImVec2 keep_o = O;
        DL = ImGui::GetWindowDrawList();
        O = ImVec2(0, 0);
        for (int l = 0; l < LANG_COUNT; l++) {
            float ly = 64 + 30 * l;
            bool lh, ld;
            if (hit(lang_code((Lang)l), x + 4, ly, 764 - x - 8, 30, &lh, &ld)) {
                g_lang = (Lang)l;
                save();
                ImGui::CloseCurrentPopup();
            }
            if (lh || l == g_lang)
                DL->AddRectFilled(P(x + 4, ly), P(760, ly + 30), lh ? C_DARK_HOVER : C_DARK_DOWN, 4 * S);
            text(g_media.medium, x + 13, ly + 15 - g_media.medium->FontSize / S / 2, C_TEXT, lang_name((Lang)l));
        }
        DL = keep, O = keep_o;
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

// The heading as the design has it: y the top of its 32 px line (82: centred at 98), the subtitle's 20 px lines from 40
// below (122), 650 wide. Returns where the subtitle (or the heading) ends.
static float title(float y, const char *t, const char *sub) {
    ImFont *f = g_media.title;
    text(f, 36, y + (font_line(f) - f->FontSize / S) / 2, C_TEXT, t);
    if (!sub)
        return y + font_line(f);
    return y + 40 + rich(g_media.regular, 36, y + 40, 650, {{sub, C_SUB}});
}

static std::string gb(double v) { return std::to_string((long)(v + 0.5)); }

static void screen_dump() {
    bool data = U.dump_st == D_DATA;
    float y = title(82, T(data ? "prep_title" : "dump_title"), T(data ? "data_sub" : "dump_sub")) + 24;
    bool hov, down;
    if (U.dump_st == D_NONE || U.dump_st == D_CHECKING) {
        bool busy = U.dump_st == D_CHECKING;
        if (hit("pick", 36, y, 728, 88, &hov, &down, !busy))
            pick_folder(1, U.dump);
        DL->AddRectFilled(P(36, y), P(764, y + 88), down ? C_CARD : hov ? C_CARD_HOVER : C_CARD, 10 * S);
        const char *label = busy ? T("checking") : T("choose_folder");
        if (!busy)
            icon(g_media.folder, 400 - 12, y + 18);
        float tw = text_w(g_media.medium, label);
        text(g_media.medium, 400 - tw / 2, busy ? y + 35 : y + 50, busy ? C_SUB : C_TEXT, label);
        float hw = text_w(g_media.small, T("drop_hint"));
        text(g_media.small, 400 - hw / 2, y + 102, C_DIM, T("drop_hint"));
        return;
    }
    if (U.dump_st == D_BAD) {
        std::string head = has_text(U.dump_code.c_str()) ? T(U.dump_code.c_str()) : T("error_generic");
        std::string detail = U.dump_code + ".d";
        float bw = text_w(g_media.medium, T("choose_other")) + 32;
        float top = y;
        y += 18;
        path_text(g_media.regular, 52, y + 10, 728 - bw - 48, U.checking);
        if (button("other", T("choose_other"), 748 - bw, y, bw, 40, LIGHT))
            pick_folder(1, U.checking);
        y += 56;
        icon(g_media.error, 52, y + 2);
        y += rich(g_media.regular, 76, y, 660, {{head.c_str(), C_TEXT}}) + 2;
        if (has_text(detail.c_str())) {
            std::string d = Tf(detail.c_str(), U.dump_args);
            y += rich(g_media.regular, 76, y, 660, {{d.c_str(), C_SUB}});
        }
        DL->ChannelsSetCurrent(0);
        DL->AddRectFilled(P(36, top), P(764, y + 14), C_CARD, 10 * S);
        DL->ChannelsSetCurrent(1);
        return;
    }
    // D_OK: the dump and the install folder; D_DATA: the game data already prepared instead of the dump
    float top = y;
    float cw = text_w(g_media.medium, T("change")) + 32, pw = 0;
    if (U.dump_st == D_DATA) {
        text(g_media.small, 52, y + 14, C_SUB, T("data_label"));
        path_text(g_media.regular, 52, y + 38, 640, path_join(U.install, "data"), &pw);
        icon(g_media.check, 52 + pw + 8, y + 39);
        y += 62;
        y += rich(g_media.small, 52, y, 700, {{T("data_note"), C_SUB}});
        DL->ChannelsSetCurrent(0);
        DL->AddRectFilled(P(36, top), P(764, y + 12), C_CARD, 10 * S);
        DL->ChannelsSetCurrent(1);
        top = y + 20;
    } else {
        text(g_media.small, 52, y + 14, C_SUB, T("dump_label"));
        path_text(g_media.regular, 52, y + 38, 748 - cw - 52 - 40, U.dump, &pw);
        icon(g_media.check, 52 + pw + 8, y + 39);
        if (button("dump_change", T("change"), 748 - cw, y + 14, cw, 40, DARK))
            pick_folder(1, U.dump);
        DL->ChannelsSetCurrent(0);
        DL->AddRectFilled(P(36, top), P(764, top + 68), C_CARD, 10 * S);
        DL->ChannelsSetCurrent(1);
        top += 76;
    }

    y = top;
    text(g_media.small, 52, y + 14, C_TEXT, T("install_label"));
    path_text(g_media.regular, 52, y + 38, 748 - cw - 52 - 16, U.install);
    if (button("inst_change", T("change"), 748 - cw, y + 12, cw, 40, DARK))
        pick_folder(2, U.install);
    double need = need_gb(), free = disk_free_gb(U.install);
    long lo, hi;
    wait_range(&lo, &hi);
    std::string need_s = Tf("need_free", {{"need", gb(need)}, {"free", free < 0 ? "?" : gb(free)}});
    std::string wait_s = Tf("wait_time", {{"lo", std::to_string(lo)}, {"hi", std::to_string(hi)}});
    bool space = free < 0 || free >= need || installed() || started_before();
    y += 64;
    if (data) {   // built here before: only what changed is built again
        std::string fs = Tf("free_only", {{"free", free < 0 ? "?" : gb(free)}});
        y += rich(g_media.small, 52, y, 700, {{fs.c_str(), C_SUB}});
    } else if (space)
        y += rich(g_media.small, 52, y, 700, {{need_s.c_str(), C_SUB}, {(" " + wait_s).c_str(), C_TEXT}});
    else {
        std::string ns = Tf("no_space", {{"drive", drive_of(U.install)}});
        y += rich(g_media.small, 52, y, 700, {{need_s.c_str(), C_ERR}, {(" " + ns).c_str(), C_ERR}});
    }
    if (U.dump_st != D_DATA) {
        std::string del = Tf("can_delete", {{"dump", U.dump}});
        y += rich(g_media.small, 52, y, 700, {{del.c_str(), C_SUB}});
    }
    DL->ChannelsSetCurrent(0);
    DL->AddRectFilled(P(36, top), P(764, y + 14), C_CARD, 10 * S);
    DL->ChannelsSetCurrent(1);
}

static void version_label() {   // the port's version (the launcher's is the same), as the design shows it
    if (U.version_line.empty())
        return;
    std::string v = Tf("version", {{"v", U.version_line}});
    text(g_media.small, 400 - text_w(g_media.small, v.c_str()) / 2, 440, C_DIM, v.c_str());
}

static float platform_choice(float x_right, bool note);

static void screen_dump_footer() {
    if (U.dump_st != D_OK && U.dump_st != D_DATA) {
        version_label();
        return;
    }
    bool resume = started_before(), rebuild = stale();
    const char *label = rebuild ? T("rebuild") : resume ? T("resume") : T("prepare");
    float w = SDL_max(196.0f, text_w(g_media.medium, label) + 48);
    double free = disk_free_gb(U.install);
    bool space = free < 0 || free >= need_gb() || installed() || resume;
    float left = platform_choice(764 - w - 8, !(resume || rebuild));   // the design: for which platform, beside the button
    if (resume || rebuild)
        rich(g_media.small, 36, 418, left - 36 - 24, {{T(rebuild ? "rebuild_hint" : "resume_hint"), C_SUB}}, 17);
    if (button("prepare", label, 764 - w, 408, w, 48, PRIMARY, space && !linux_kit_gone()))
        start_build();
}

static void step_rows(float y, float sp) {   // sp: from one row to the next (26, less when they would not fit)
    double est = est_total_secs();
    for (int i = 0; i < n_rows(); i++) {
        Row &r = U.rows[i];
        float ry = y + sp * i, cx = 44, cy = ry + 8;
        ImU32 name_col = C_TEXT;
        switch (r.st) {
        case R_DONE: icon(g_media.check, 36, ry); break;
        case R_WARN: icon(g_media.warn, 36, ry); break;
        case R_RUN: ring(cx, cy, 6.5f, C_OK); break;
        case R_ERROR: ring(cx, cy, 6.5f, C_ERR); name_col = C_ERR; break;
        default: dot(cx, cy, 7, C_PENDING); name_col = C_DIM;
        }
        const char *name = T(r.name);
        text(g_media.regular, 58, ry - 1, name_col, name);
        float nx = 58 + text_w(g_media.regular, name) + 8;
        std::string note;
        ImU32 note_col = C_DIM;
        ImFont *nf = g_media.small;
        if (r.st == R_ERROR) {
            note = r.err, note_col = C_ERR, nf = g_media.regular;
        } else if (r.st == R_WARN && i == 0) {
            note = T("hw_note");
        } else if (r.st == R_DONE && r.skipped) {
            note = T("was_done");
        } else if (r.st == R_DONE && r.secs >= 1) {
            note = Tf("took", {{"t", fmt_duration(r.secs)}});
        } else if (r.st == R_RUN && U.eta > 0) {
            note = Tf("left", {{"t", fmt_minutes(U.eta)}});
        } else if (r.st == R_PENDING && (i == 3 || i == 5)) {   // the long ones, as the design shows
            note = Tf("may_take", {{"t", fmt_minutes(est * U.weight[setup_index(r.id)] / 100)}});
        }
        if (!note.empty())
            text(nf, nx, ry + (nf == g_media.small ? 1 : -1), note_col, note.c_str());
        if (i == 0 && U.hw.size() && hovered_rect(P(36, ry - 2), P(760, ry + 20))) {   // the cases, on hover
            ImGui::BeginTooltip();
            for (size_t k = 0; k < U.hw.size(); k++) {
                std::string code = U.hw[k]["code"].str();
                ImGui::TextUnformatted(Tf(code.c_str(), U.hw[k]["args"]).c_str());
                std::string d = code + ".d";
                if (has_text(d.c_str())) {
                    ImGui::PushStyleColor(ImGuiCol_Text, C_SUB);
                    ImGui::TextUnformatted(Tf(d.c_str(), U.hw[k]["args"]).c_str());
                    ImGui::PopStyleColor();
                }
            }
            ImGui::EndTooltip();
        }
    }
}

static void screen_build() {
    float y;
    if (U.done)
        y = title(84, T("done_title"), nullptr) + 21;   // the design: no subtitle
    else
        y = title(82, T("prep_title"), T("prep_sub")) + 26;
    double f = U.done ? 1 : overall();
    char pct[16];
    SDL_snprintf(pct, sizeof pct, "%d%%", (int)(f * 100));
    if (!U.done) {
        const char *label = U.failed ? T("failed_title") : U.copy ? T("copying") : T("processing");
        for (const Row &r : U.rows)   // the design: the step running names the bar
            if (!U.failed && !U.copy && r.st == R_RUN)
                label = T(r.name);
        text(g_media.regular, 36, y + 1, C_TEXT, label);
    }
    text(g_media.regular, 764 - text_w(g_media.regular, pct), y + 1, C_TEXT, pct);
    y += 31;
    DL->AddRectFilled(P(36, y), P(764, y + 8), C_BAR, 4 * S);
    if (f > 0)
        DL->AddRectFilled(P(36, y), P(36 + 728 * (float)f, y + 8), U.failed ? C_BAR_FAIL : C_BAR_FILL, 4 * S);
    y += 34;
    // every step above the footer (y 408): the rows close up when there are more of them than room (a build for the
    // Steam Deck has one more), the failure's words below them included
    float room = 398 - y - (U.failed && !U.fail_text.empty() ? 22 : 0);
    float sp = SDL_max(20.0f, SDL_min(26.0f, room / n_rows()));
    step_rows(y, sp);
    if (U.failed && !U.fail_text.empty())
        rich(g_media.small, 36, y + sp * n_rows() + 4, 728, {{U.fail_text.c_str(), C_ERR}});
}

static void screen_build_footer() {
    if (U.done) {
        if (for_deck() && deck_built()) {   // what to carry to the Deck, and where it is
            const char *show = T("show_file");
            float sw = text_w(g_media.medium, show) + 40;
            SDL_PathInfo info;
            double size = SDL_GetPathInfo(deck_tar().c_str(), &info) ? info.size / 1073741824.0 : 0;
            char gbs[16];
            SDL_snprintf(gbs, sizeof gbs, "%.1f", size);
            std::string t = Tf("deck_carry", {{"file", deck_tar()}, {"gb", gbs}});
            rich(g_media.small, 36, 410, 649 - 12 - sw - 12 - 36, {{t.c_str(), C_TEXT}}, 17);
            if (button("show_tar", show, 649 - 12 - sw, 408, sw, 48, DARK))
                reveal_in_folder(deck_tar());
        }
        if (button("done", T("done_btn"), 649, 408, 115, 48, PRIMARY)) {
            // built for the Steam Deck alone: nothing to play on this PC, back to the folders and the platform choice
            if (installed())
                U.screen = SCR_PLAY;
            else {
                dump_state();
                U.screen = SCR_DUMP;
            }
        }
        return;
    }
    if (U.failed) {
        std::string issues = std::string(ULTRS_REPO) + "/issues/new";
        rich(g_media.small, 36, 416, 422, {{T("report1"), C_SUB}, {T("report_link"), C_TEXT, issues.c_str()}, {T("report2"), C_SUB}}, 17);
        float w = text_w(g_media.medium, T("open_log")) + 70;
        if (button("open_log", T("open_log"), 764 - w, 408, w, 48, DARK))
            reveal_in_folder(log_path());
        return;
    }
    if (U.cancel_q) {
        rich(g_media.small, 36, 412, 420, {{T("cancel_q"), C_TEXT}}, 17);
        float w1 = text_w(g_media.medium, T("stop")) + 40, w2 = text_w(g_media.medium, T("go_on")) + 40;
        if (button("stop", T("stop"), 764 - w2 - 12 - w1, 408, w1, 48, DARK)) {
            if (U.build)
                setup_kill(U.build);
            if (U.copy)
                setup_kill(U.copy);
            U.stopping = true;
            U.cancel_q = false;
            U.screen = SCR_DUMP;
            if (U.close_after_cancel) {
                SDL_Event q = {};
                q.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&q);
            }
        }
        if (button("go_on", T("go_on"), 764 - w2, 408, w2, 48, PRIMARY))
            U.cancel_q = U.close_after_cancel = false;
        return;
    }
    float w = text_w(g_media.medium, T("cancel")) + 40;
    if (button("cancel", T("cancel"), 36, 416, w, 40, DARK))
        U.cancel_q = true;
    float hw = text_w(g_media.small, T("bg_hint"));
    text(g_media.small, 764 - hw, 430, C_SUB, T("bg_hint"));
}

static void screen_hw() {
    float y = title(82, T("hw_title"), nullptr) + 8;
    std::string req = std::string(ULTRS_REPO) + (g_lang == LANG_RU ? "/blob/HEAD/README.ru.md" : "#requirements-for-either-method");
    y += rich(g_media.regular, 36, y, 700, {{T("hw_text1"), C_SUB}, {T("hw_link"), C_TEXT, req.c_str()}, {T("hw_text2"), C_SUB}});
    y += 25;
    bool hov, down;
    float w1 = text_w(g_media.medium, T("hw_go")) + 36, w2 = text_w(g_media.medium, T("cancel")) + 36;
    if (button("hw_go", T("hw_go"), 36, y, w1, 40, PRIMARY)) {
        setup_answer(U.build, "yes");
        U.screen = SCR_BUILD;
    }
    if (button("hw_no", T("cancel"), 36 + w1 + 8, y, w2, 40, DARK)) {
        setup_answer(U.build, "no");
        U.screen = SCR_DUMP;
    }
    (void)hov, (void)down;
    y += 64;
    for (size_t k = 0; k < U.hw.size(); k++) {   // what exactly: the graphics card first (stop), then the rest
        const Json &c = U.hw[k];
        std::string code = c["code"].str(), sev = c["sev"].str();
        icon(sev == "stop" ? g_media.error : g_media.warn, 36, y + 2);
        std::string head = Tf(code.c_str(), c["args"]);
        y += rich(g_media.medium, 60, y, 700, {{head.c_str(), C_TEXT}});
        std::string d = code + (code == "no_shader_object" && c["args"]["layer"].truth() ? ".layer" : ".d");
        if (has_text(d.c_str())) {
            std::string dt = Tf(d.c_str(), c["args"]);
            y += rich(g_media.regular, 60, y, 700, {{dt.c_str(), C_SUB}});
        }
        y += 10;
    }
    ImGui::SetCursorScreenPos(P(0, y));
    ImGui::Dummy(ImVec2(1, 1));
}

static void screen_broken() {   // the exe without the rest of the zip: nothing to choose, only what to do
    float y = title(82, T("broken_title"), nullptr) + 4;
    rich(g_media.regular, 36, y, 700, {{T("broken_text"), C_SUB}});
}

// ---------------------------------------------------------------- Play: the game's settings
// The player's keys in the install folder's ultratears.cfg, read by the game at its start (src/host/boot.c) and kept by
// setup.py on an update: fps=, window=, vsync=, lang=. A key left out is the game's default.
static std::string cfg_path() { return path_join(U.install, "ultratears.cfg"); }

static std::vector<std::string> cfg_read() {
    std::vector<std::string> out;
    size_t n = 0;
    char *t = (char *)SDL_LoadFile(cfg_path().c_str(), &n);
    if (!t)
        return out;
    std::string s(t, n);
    SDL_free(t);
    for (size_t pos = 0; pos < s.size();) {
        size_t e = s.find('\n', pos);
        if (e == std::string::npos)
            e = s.size();
        std::string l = s.substr(pos, e - pos);
        pos = e + 1;
        if (!l.empty() && l.back() == '\r')
            l.pop_back();
        out.push_back(l);
    }
    return out;
}

static std::vector<std::string> g_cfg;   // read again a second after the last read: the screen asks every frame
static Uint64 g_cfg_at;

static const std::vector<std::string> &cfg_lines() {
    if (!g_cfg_at || SDL_GetTicks() - g_cfg_at > 1000) {
        g_cfg = cfg_read();
        g_cfg_at = SDL_GetTicks();
    }
    return g_cfg;
}

static std::string cfg_get(const char *key) {
    std::string want = std::string(key) + "=";
    for (const std::string &l : cfg_lines())
        if (l.rfind(want, 0) == 0)
            return l.substr(want.size());
    return "";
}

static void cfg_set(const char *key, const std::string &value) {   // "" removes the key: the game's default
    std::string want = std::string(key) + "=", text;
    bool done = false;
    for (const std::string &l : cfg_lines()) {
        if (l.rfind(want, 0) == 0) {
            if (!done && !value.empty())
                text += want + value + "\n";
            done = true;
            continue;
        }
        text += l + "\n";
    }
    if (!done && !value.empty())
        text += want + value + "\n";
    SDL_SaveFile(cfg_path().c_str(), text.data(), text.size());
    g_cfg_at = 0;
}

struct Opt { const char *value, *label; };   // label: a text key, else shown as it is
static const char *opt_label(const Opt &o) { return has_text(o.label) ? T(o.label) : o.label; }

static const Opt WINDOW_OPTS[] = {{"fullscreen", "win_full"}, {"borderless", "win_borderless"}, {"window", "win_window"}};
static const Opt FPS_OPTS[] = {{"30", "30"}, {"40", "40"}, {"45", "45"}, {"60", "60"}, {"120", "fps_120"},
                               {"144", "fps_144"}, {"display", "fps_display"}, {"unlimited", "fps_unlimited"}};
static const Opt LANG_OPTS[] = {{"", "lang_system"}, {"en-US", "English"}, {"de", "Deutsch"}, {"es", "Español"},
                                {"fr", "Français"}, {"it", "Italiano"}, {"nl", "Nederlands"}, {"pt", "Português"},
                                {"pt-BR", "Português (Brasil)"}, {"ru", "Русский"}, {"ja", "lang_ja"}, {"ko", "lang_ko"},
                                {"zh-Hans", "lang_zh"}};
// The game's resolution (src/host/render_res.c): resolution= the size the game composes its frame in (its output),
// scale= its 3D scene in % of that (unset: the game's own 5/6); the game's FSR 1 scales the scene to the output, and the
// present's FSR 1 the output to a larger window.
// Another shape than 16:9 (src/host/render_res.c totk_aspect): the camera takes the output's ratio and the UI is laid out
// for it (src/host/ui_aspect.c); 16:10 with the Steam
// Deck's 1280x800. "display": the screen's own size, whatever its shape.
static const Opt RES_16_9[] = {{"1280x720", "1280 × 720"}, {"1920x1080", "1920 × 1080"}, {"2560x1440", "2560 × 1440"},
                               {"3840x2160", "3840 × 2160"}, {"display", "res_display"}};
static const Opt RES_16_10[] = {{"1280x800", "1280 × 800"}, {"1680x1050", "1680 × 1050"}, {"1920x1200", "1920 × 1200"},
                                {"2560x1600", "2560 × 1600"}, {"display", "res_display"}};
static const Opt RES_21_9[] = {{"2560x1080", "2560 × 1080"}, {"3440x1440", "3440 × 1440"}, {"3840x1600", "3840 × 1600"},
                               {"5120x2160", "5120 × 2160"}, {"display", "res_display"}};
static const Opt RES_32_9[] = {{"3840x1080", "3840 × 1080"}, {"5120x1440", "5120 × 1440"}, {"display", "res_display"}};
static const Opt ASPECT_OPTS[] = {{"16:9", "16:9"}, {"16:10", "16:10"}, {"21:9", "21:9"}, {"32:9", "32:9"}};
#define RES_LIST(a) RES_##a, sizeof RES_##a / sizeof *RES_##a
static const struct { const Opt *opts; size_t n; double ratio; } RES_BY_ASPECT[] = {
    {RES_LIST(16_9), 16.0 / 9}, {RES_LIST(16_10), 16.0 / 10}, {RES_LIST(21_9), 64.0 / 27}, {RES_LIST(32_9), 32.0 / 9}};

static int res_index(int a, const std::string &v) {
    for (size_t i = 0; i < RES_BY_ASPECT[a].n; i++)
        if (v == RES_BY_ASPECT[a].opts[i].value)
            return (int)i;
    return -1;
}

// The shape of a resolution= value: the nearest of the four ("" is the game's 1920x1080; display: the screen's).
static int res_aspect(const std::string &res) {
    int w = 1920, h = 1080;
    if (res == "display") {
        const SDL_DisplayMode *m = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
        if (m && m->h > 0)
            w = m->w, h = m->h;
    } else if (!res.empty() && SDL_sscanf(res.c_str(), "%dx%d", &w, &h) != 2) {
        w = 1920, h = 1080;
    }
    double r = (double)w / SDL_max(h, 1);
    int best = 0;
    for (int i = 1; i < 4; i++)
        if (SDL_fabs(r - RES_BY_ASPECT[i].ratio) < SDL_fabs(r - RES_BY_ASPECT[best].ratio))
            best = i;
    return best;
}

// Another shape chosen: the size of that shape nearest in height to the present one (display stays display).
static std::string res_for_aspect(int a, const std::string &res) {
    if (res == "display")
        return res;
    int w = 1920, h = 1080;
    if (!res.empty())
        SDL_sscanf(res.c_str(), "%dx%d", &w, &h);
    const Opt *opts = RES_BY_ASPECT[a].opts;
    int best = 0, best_d = SDL_MAX_SINT32;
    for (int i = 0; i < (int)RES_BY_ASPECT[a].n; i++) {
        int ow = 1920, oh = 1080;
        if (!SDL_strcmp(opts[i].value, "display") || (*opts[i].value && SDL_sscanf(opts[i].value, "%dx%d", &ow, &oh) != 2))
            continue;
        int d = SDL_abs(oh - h);
        if (d < best_d)
            best = i, best_d = d;
    }
    return opts[best].value;
}
// The scales: FSR 1's own modes (AMD: performance 2x, balanced 1.7x, quality 1.5x, ultra quality 1.3x per axis), the
// game's 5/6, and supersampling; 100% is the upscaler "none".
// The upscaler, beside the scale: none (the scene at the output's size, scale=100; the scale list greyed), the game's
// FSR 1 (every scale but 100%, supersampling too) or FSR 3.1 (antialias=fsr3, src/gfx/vkb_tsr.c: it also anti-aliases
// over frames, and does not scale down: below 100% only)
static const Opt UPSCALER_OPTS[] = {{"none", "up_none"}, {"fsr1", "aa_game"}, {"fsr3", "FSR 3.1"}};
static const Opt SCALE_FSR1[] = {{"50", "scale_perf"}, {"59", "scale_bal"}, {"67", "scale_qual"}, {"77", "scale_ultra"},
                                 {"", "scale_game"}, {"150", "scale_150"}, {"200", "scale_200"}};
static const Opt SCALE_FSR3[] = {{"50", "scale_perf"}, {"59", "scale_bal"}, {"67", "scale_qual"}, {"77", "scale_ultra"},
                                 {"", "scale_game"}};
// antialias= without FSR 3.1 (src/gfx/vkb_smaa.c; its own row, gone with FSR 3.1): the game's FXAA, SMAA 1x or none
static const Opt AA_OPTS[] = {{"", "aa_fxaa"}, {"smaa", "SMAA"}, {"off", "aa_off"}};
// sharpness= (src/vk/fsr.c, src/gfx/vkb_tsr.c): the RCAS pass of the present's FSR 1 (when the window is larger than the
// game's output), and of FSR 3.1
static const Opt SHARP_OPTS[] = {{"0", "sharp_off"}, {"25", "25%"}, {"", "sharp_default"}, {"75", "75%"}, {"100", "100%"}};

// The window, resolution and frame limit the game takes when ultratears.cfg sets none: its own on a PC, the Steam Deck's
// window and resolution on the Deck (src/host/boot.c deck_defaults: fullscreen, 1280x800); the frame limit 60 on both
// (a new game folder on the Deck starts with fps=40 in its ultratears.cfg: tools/setup.py first_settings). Each option writes its value: an option that wrote "" (the key removed) meant the game's
// default, so on the Deck the PC's 60 fps, 1920x1080 and window could not be chosen at all - the Deck's defaults came
// back (the user's Deck ran at 40 with 60 chosen, 2026-11-06) - and an unset key showed the PC's value while the
// Deck's ran.
static bool on_deck();
static std::string cfg_or_platform(const char *key) {
    std::string v = cfg_get(key);
    if (!v.empty())
        return v;
    bool deck = on_deck();
    if (!SDL_strcmp(key, "window")) return deck ? "fullscreen" : "window";
    if (!SDL_strcmp(key, "resolution")) return deck ? "1280x800" : "1920x1080";
    if (!SDL_strcmp(key, "fps")) return "60";
    return v;
}

template <size_t N> static int opt_index(const Opt (&opts)[N], const std::string &v, const char *same_as_default = "") {
    std::string value = v == same_as_default ? "" : v;
    for (size_t i = 0; i < N; i++)
        if (value == opts[i].value)
            return (int)i;
    return -1;
}

static const ImU32 C_FIELD = IM_COL32(43, 43, 43, 255);
static const ImU32 C_FIELD_HOVER = IM_COL32(54, 54, 54, 255);
static const ImU32 C_SEG = IM_COL32(36, 36, 36, 255);
static const ImU32 C_SEG_ON = IM_COL32(58, 58, 58, 255);

// The design's segmented control, right-aligned at x_right; returns the index chosen this frame or -1.
template <size_t N> static int segmented(const char *id, float x_right, float cy, const Opt (&opts)[N], int sel) {
    float w[N], total = 4;
    for (size_t i = 0; i < N; i++)
        total += (w[i] = text_w(g_media.control, opt_label(opts[i])) + 24);
    float x = x_right - total, y = cy - 16;
    DL->AddRectFilled(P(x, y), P(x_right, y + 32), C_SEG, 8 * S);
    int chosen = -1;
    float ix = x + 2;
    for (size_t i = 0; i < N; i++) {
        bool hov, down;
        char hid[64];
        SDL_snprintf(hid, sizeof hid, "%s%d", id, (int)i);
        if (hit(hid, ix, y + 2, w[i], 28, &hov, &down) && (int)i != sel)
            chosen = (int)i;
        if ((int)i == sel || hov)
            DL->AddRectFilled(P(ix, y + 2), P(ix + w[i], y + 30), (int)i == sel ? C_SEG_ON : C_FIELD, 6 * S);
        const char *l = opt_label(opts[i]);
        text(g_media.control, ix + 12, cy - g_media.control->FontSize / S / 2, C_TEXT, l);
        ix += w[i];
    }
    return chosen;
}

// A drop-down list: the box right-aligned at x_right, its list in a popup (scrolls when long). shown: the box says
// this instead of the choice (a button that opens a list, as the Debug tab's full rebuild); h: the box's height.
static int dropdown_n(const char *id, float x_right, float cy, float w, const Opt *opts, size_t N, int sel,
                      const char *shown, float h);
// A drop-down box that cannot be opened (its choice set by another): greyed, without its chevron.
static void dropdown_off(float x_right, float cy, float w, const char *shown) {
    float h = 32, x = x_right - w, y = cy - h / 2;
    DL->AddRectFilled(P(x, y), P(x_right, y + h), C_SEG, 6 * S);
    text(g_media.control, x + 12, cy - g_media.control->FontSize / S / 2, C_DIM, shown);
}
template <size_t N> static int dropdown(const char *id, float x_right, float cy, float w, const Opt (&opts)[N], int sel,
                                        const char *shown = nullptr, float h = 32) {
    return dropdown_n(id, x_right, cy, w, opts, N, sel, shown, h);
}

static int dropdown_n(const char *id, float x_right, float cy, float w, const Opt *opts, size_t N, int sel,
                      const char *shown, float h) {
    if (w <= 0) {   // as wide as its longest choice, at least the design's 126
        w = SDL_max(126.0f, shown ? text_w(g_media.medium, shown) + 52 : 0);
        for (size_t i = 0; i < N; i++)
            w = SDL_max(w, text_w(g_media.control, opt_label(opts[i])) + 52);
    }
    float x = x_right - w, y = cy - h / 2;
    bool hov, down;
    char pid[64];
    SDL_snprintf(pid, sizeof pid, "dd_%s", id);
    extern const char *g_demo_open;   // development: a list opened for a screenshot (ui_demo)
    if (hit(id, x, y, w, h, &hov, &down) || (g_demo_open && !SDL_strcmp(g_demo_open, id))) {
        ImGui::OpenPopup(pid);
        g_demo_open = nullptr;
    }
    bool open = ImGui::IsPopupOpen(pid);
    DL->AddRectFilled(P(x, y), P(x_right, y + h), hov || open ? C_FIELD_HOVER : C_FIELD, (h > 32 ? 8 : 6) * S);
    ImFont *bf = shown || h > 32 ? g_media.medium : g_media.control;   // a button's words, as the design's buttons
    const char *bl = shown ? shown : sel >= 0 ? opt_label(opts[sel]) : "";
    text(bf, shown || h > 32 ? x + (w - 26 - text_w(bf, bl)) / 2 - 2 : x + 12, cy - bf->FontSize / S / 2, C_TEXT, bl);
    icon(g_media.chevron, x_right - 12 - 14, cy - 7, 14);
    int chosen = -1;
    // The list stays inside the window (SDL_Renderer draws no windows of its own outside it): it opens on the side with
    // more room, as tall as that room allows, and scrolls the rest.
    float lh = 30, want = N * lh + 8, wh = ImGui::GetIO().DisplaySize.y;
    float below = (wh - P(x, y + h + 4).y) / S - 8, above = P(x, y - 4).y / S - 8;
    bool open_down = below >= want || below >= above;
    float ph = SDL_min(want, SDL_max(lh + 8, open_down ? below : above)), py = open_down ? y + h + 4 : y - 4 - ph;
    ImGui::SetNextWindowPos(P(x, py));
    ImGui::SetNextWindowSize(ImVec2(w * S, ph * S));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (ImGui::BeginPopup(pid)) {
        if (ImGui::IsWindowAppearing() && sel >= 0 && want > ph)   // the chosen item in view
            ImGui::SetScrollY(SDL_max(0.0f, (4 + lh * sel - (ph - lh) / 2) * S));
        ImDrawList *keep = DL;
        ImVec2 keep_o = O;
        DL = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetWindowPos();
        O = ImVec2(wp.x - x * S, wp.y - ImGui::GetScrollY() - py * S);
        DL->AddRectFilled(P(x, py), P(x_right, py + 8 + N * lh), C_FIELD, 6 * S);
        for (size_t i = 0; i < N; i++) {
            float iy = py + 4 + lh * i;
            bool ih, idn;
            char hid[64];
            SDL_snprintf(hid, sizeof hid, "%s_%d", id, (int)i);
            if (hit(hid, x + 4, iy, w - 8, lh, &ih, &idn)) {
                chosen = (int)i;
                ImGui::CloseCurrentPopup();
            }
            if (ih || (int)i == sel)
                DL->AddRectFilled(P(x + 4, iy), P(x_right - 4, iy + lh), ih ? C_FIELD_HOVER : C_SEG_ON, 4 * S);
            text(g_media.control, x + 12, iy + lh / 2 - g_media.control->FontSize / S / 2, C_TEXT, opt_label(opts[i]));
        }
        ImGui::SetCursorScreenPos(P(x, py + 8 + N * lh));
        ImGui::Dummy(ImVec2(1, 1));
        DL = keep, O = keep_o;
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    return chosen;
}

// For which platform the game is built (setup.py --platform): this PC, the Steam Deck (a tar to carry there; the Linux
// kit is downloaded for it), or both. The launcher's own choice, remembered in its settings.
static const Opt PLATFORM_OPTS[] = {{"windows", "plat_windows"}, {"linux", "plat_linux"}, {"both", "plat_both"}};

// The design's platform list right-aligned at x_right on the footer's line, and with note the words that the Linux kit is
// downloaded when it is not there yet; returns where the list (or the note) begins.
static float platform_choice(float x_right, bool note) {
#ifndef _WIN32
    (void)note;
    return x_right;   // on Linux the game is built for this machine: no choice (the design)
#endif
    float w = 0;
    for (const Opt &o : PLATFORM_OPTS)
        w = SDL_max(w, text_w(g_media.medium, opt_label(o)) + 56);
    int k = dropdown("platform", x_right, 432, w, PLATFORM_OPTS, opt_index(PLATFORM_OPTS, U.platform), nullptr, 48);
    if (k >= 0) {
        U.platform = PLATFORM_OPTS[k].value;
        U.set.set("platform", U.platform);
        settings_save(U.set);
    }
    float left = x_right - w;
    if (note && for_deck() && (!path_is_dir(path_join(U.root, "deps/linux/sysroot")) || linux_package_missing())) {
        // a builder package carries the kit (tools/make_package.py): gone from it, the package is damaged; a build from
        // the source makes it from Valve's sysroot (tools/deps_install.py). The Linux builder for the tar: downloaded.
        bool package = linux_kit_gone();
        std::string t = T(package ? "linux_kit_gone" : "linux_kit_note");
        ImU32 col = package ? C_ERR : C_SUB;
        size_t nl = t.find('\n');
        std::string l1 = t.substr(0, nl), l2 = nl == std::string::npos ? "" : t.substr(nl + 1);
        float r = left - 16;
        text(g_media.small, r - text_w(g_media.small, l1.c_str()), 413, col, l1.c_str());
        text(g_media.small, r - text_w(g_media.small, l2.c_str()), 431, col, l2.c_str());
        left = r - SDL_max(text_w(g_media.small, l1.c_str()), text_w(g_media.small, l2.c_str()));
    }
    return left;
}

static bool toggle(const char *id, float x_right, float cy, bool on) {   // the design's switch, 38 x 22
    float x = x_right - 38, y = cy - 11;
    bool hov, down;
    bool clicked = hit(id, x, y, 38, 22, &hov, &down);
    ImU32 bg = on ? (hov ? C_GREEN_HOVER : C_GREEN) : (hov ? C_FIELD_HOVER : C_FIELD);
    DL->AddRectFilled(P(x, y), P(x_right, y + 22), bg, 11 * S);
    DL->AddCircleFilled(P(on ? x_right - 11 : x + 11, cy), 8 * S, C_TEXT, 24);
    return clicked;
}

static int g_tab = 0;   // 0 general, 1 the game's language, 2 controls, 3 mods, 4 debug
enum { TAB_COUNT = 5, TAB_CONTROLS = 2, TAB_MODS = 3, TAB_DEBUG = 4 };
static bool g_full_q = false;   // "Rebuild completely" pressed: asking whether to
static std::string g_full_platform = "windows";   // for which platform (its list)

// The shoulder buttons' names on the controller in use, for the tab badges (the design 7.1: L1 and R1 on the Steam Deck)
static void shoulder_names(const char **l, const char **r) {
    *l = "L1", *r = "R1";
    const char *deck = SDL_getenv("SteamDeck");
    if (deck && !SDL_strcmp(deck, "1"))
        return;
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    SDL_GamepadType t = ids && n ? SDL_GetGamepadTypeForID(ids[0]) : SDL_GAMEPAD_TYPE_UNKNOWN;
    SDL_free(ids);
    if (t == SDL_GAMEPAD_TYPE_XBOX360 || t == SDL_GAMEPAD_TYPE_XBOXONE)
        *l = "LB", *r = "RB";
    else if (t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO || t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR)
        *l = "L", *r = "R";
}

static void tab_badge(float x, const char *name) {   // the design: a light rounded square with the button's name
    float w = SDL_max(22.0f, text_w(g_media.small, name) + 10);
    DL->AddRectFilled(P(x, 25), P(x + w, 47), C_LIGHT, 5 * S);
    text(g_media.small, x + (w - text_w(g_media.small, name)) / 2, 36 - g_media.small->FontSize / S / 2, IM_COL32(17, 17, 17, 255), name);
}
static float badge_w(const char *name) { return SDL_max(22.0f, text_w(g_media.small, name) + 10); }

static bool capturing();   // the Controls tab waits for a key or button (below)

static void play_tabs() {   // instead of the header on the Play screen (the design)
    static const char *const TABS[] = {"tab_main", "tab_lang", "tab_controls", "tab_mods", "tab_debug"};
    // with a controller: L1 / R1 (LB / RB) go through the tabs, their badges at both ends (the design 7.1)
    bool pad = SDL_HasGamepad();
    if (pad && !capturing() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false))
            g_tab = (g_tab + TAB_COUNT - 1) % TAB_COUNT;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false))
            g_tab = (g_tab + 1) % TAB_COUNT;
    }
    const char *ln, *rn;
    shoulder_names(&ln, &rn);
    float x = 36;
    if (pad) {
        tab_badge(x, ln);
        x += badge_w(ln) + 10;
    }
    for (int i = 0; i < TAB_COUNT; i++) {
        const char *l = T(TABS[i]);
        float w = text_w(g_media.control, l) + 24;
        bool hov, down;
        if (hit(TABS[i], x, 20, w, 32, &hov, &down))
            g_tab = i;
        if (i == g_tab || hov)
            DL->AddRectFilled(P(x, 20), P(x + w, 52), i == g_tab ? C_DARK : C_CARD_HOVER, 6 * S);
        text(g_media.control, x + 12, 36 - g_media.control->FontSize / S / 2, C_TEXT, l);
        x += w + 2;
    }
    if (pad)
        tab_badge(x + 8, rn);
    DL->AddLine(P(0, 60), P(800, 60), IM_COL32(40, 40, 40, 255), S);
}

static void row_label(float cy, const char *key) {
    text(g_media.label, 52, cy - g_media.label->FontSize / S / 2, C_TEXT, T(key));
}

// Debug: the game's Debug mode (debug=1: the statistics lines in the log and a console window with it) and the full
// rebuild (setup.py --full), asked first as Cancel during a build is.

static void screen_debug() {
    float top = 80, cy = top + 28;
    row_label(cy, "row_debug");
    bool on = !cfg_get("debug").empty() && cfg_get("debug") != "0";
    if (toggle("debug", 748, cy, on)) {
        cfg_set("debug", on ? "" : "1");
        if (on) {   // Debug mode off: its own settings go with it
            cfg_set("snapshots", "");
            cfg_set("gpu_marks", "");
        }
    }
    float bottom = top + 56;
    if (on) {   // the settings that only mean something in Debug mode
        cy += 44;
        row_label(cy, "row_snapshots");
        bool shots = cfg_get("snapshots") == "1";
        if (toggle("snapshots", 748, cy, shots))
            cfg_set("snapshots", shots ? "" : "1");
        text(g_media.small, 52, cy + 29 - g_media.small->FontSize / S / 2, C_SUB, T("snapshots_note"));
        cy += 64;
        row_label(cy, "row_gpu_marks");
        bool marks = cfg_get("gpu_marks") == "1";
        if (toggle("gpu_marks", 748, cy, marks))
            cfg_set("gpu_marks", marks ? "" : "1");
        text(g_media.small, 52, cy + 29 - g_media.small->FontSize / S / 2, C_SUB, T("gpu_marks_note"));
        bottom = cy + 50;
    }
    DL->ChannelsSetCurrent(0);
    DL->AddRectFilled(P(36, top), P(764, bottom), C_CARD, 10 * S);
    DL->ChannelsSetCurrent(1);

    if (!U.can_build)
        return;   // the Steam Deck's tar: the PC rebuilds it
    top = 320;   // the full rebuild: its own card, just above Play (the design) whichever way Debug mode is
    long lo, hi;   // the card: for this PC; the question: for the platform picked from the list
    wait_range(&lo, &hi, g_full_q ? g_full_platform : "windows");
    std::string range = Tf("full_time", {{"lo", std::to_string(lo)}, {"hi", std::to_string(hi)}});
    text(g_media.label, 52, top + 22 - g_media.label->FontSize / S / 2, C_TEXT, T("full_title"));
    if (!g_full_q) {
        text(g_media.small, 52, top + 47 - g_media.small->FontSize / S / 2, C_SUB, range.c_str());
#ifdef _WIN32
        // the design: the button opens the list of platforms to rebuild for
        int k = dropdown("full", 748, top + 34, 0, PLATFORM_OPTS, -1, T("full_btn"), 38);
        if (k >= 0) {
            g_full_platform = PLATFORM_OPTS[k].value;
            g_full_q = true;
        }
#else
        float w = text_w(g_media.medium, T("full_btn")) + 34;   // on Linux: this machine only, a plain button
        if (button("full", T("full_btn"), 748 - w, top + 15, w, 38, DARK))
            g_full_platform = "linux", g_full_q = true;
#endif
    } else {   // the question, with the time it takes, in place of the button
        std::string q = Tf("full_q", {{"lo", std::to_string(lo)}, {"hi", std::to_string(hi)}});
        float w1 = text_w(g_media.medium, T("full_go")) + 34, w2 = text_w(g_media.medium, T("cancel")) + 34;
        rich(g_media.small, 52, top + 38, 748 - w1 - w2 - 8 - 52 - 16, {{q.c_str(), C_TEXT}}, 16);
        if (button("full_no", T("cancel"), 748 - w1 - 8 - w2, top + 15, w2, 38, DARK))
            g_full_q = false;
        if (button("full_go", T("full_go"), 748 - w1, top + 15, w1, 38, PRIMARY)) {
            g_full_q = false;
            U.full = true;
            U.platform = g_full_platform;
            U.set.set("platform", U.platform);
            settings_save(U.set);
            start_build();
        }
    }
    DL->ChannelsSetCurrent(0);
    DL->AddRectFilled(P(36, top), P(764, top + 68), C_CARD, 10 * S);
    DL->ChannelsSetCurrent(1);
}

static void screen_controls();
static void screen_mods();

static void screen_play() {
    if (g_tab == TAB_DEBUG) {
        screen_debug();
        return;
    }
    if (g_tab == TAB_MODS) {
        screen_mods();
        return;
    }
    if (g_tab == TAB_CONTROLS) {
        screen_controls();
        return;
    }
    float top = 80, y = top + 20;
    text(g_media.small, 52, y - g_media.small->FontSize / S / 2, C_SUB, T(g_tab == 0 ? "sec_screen" : "sec_ui_lang"));
    float cy = top + 52;
    if (g_tab == 0) {
        // seven rows above Play (the design has five at 40); eight with the anti-aliasing row (not with FSR 3.1): the
        // body scrolls then, as the Controls tab
        std::string aa = cfg_get("antialias"), sc = cfg_get("scale");
        int up = aa == "fsr3" ? 2 : sc == "100" ? 0 : 1;   // the upscaler: none, FSR 1, FSR 3.1
        const float ROW = 38;
        row_label(cy, "row_window");
        int k = segmented("window", 748, cy, WINDOW_OPTS, opt_index(WINDOW_OPTS, cfg_or_platform("window")));
        if (k >= 0)
            cfg_set("window", WINDOW_OPTS[k].value);
        cy += ROW;
        row_label(cy, "row_res");   // the design: the shape, then the size of that shape
        std::string res = cfg_or_platform("resolution");
        int a = res_aspect(res);
        if ((k = dropdown_n("res", 748, cy, 0, RES_BY_ASPECT[a].opts, RES_BY_ASPECT[a].n, res_index(a, res), nullptr, 32)) >= 0)
            cfg_set("resolution", RES_BY_ASPECT[a].opts[k].value);
        float rw = 126;
        for (size_t i = 0; i < RES_BY_ASPECT[a].n; i++)
            rw = SDL_max(rw, text_w(g_media.control, opt_label(RES_BY_ASPECT[a].opts[i])) + 52);
        float aw = 0;   // the design: as narrow as its longest shape
        for (const Opt &o : ASPECT_OPTS)
            aw = SDL_max(aw, text_w(g_media.control, o.label) + 50);
        if ((k = dropdown("aspect", 748 - rw - 8, cy, aw, ASPECT_OPTS, a)) >= 0 && k != a)
            cfg_set("resolution", res_for_aspect(k, res));
        cy += ROW;
        row_label(cy, "row_scale");   // the method, then its scale (as the shape, then the size, of the resolution)
        {
            float sw = 126, mw = 0;   // the scale box as wide as any of its lists, so that it stays in place
            for (const Opt &o : SCALE_FSR1)
                sw = SDL_max(sw, text_w(g_media.control, opt_label(o)) + 52);
            for (const Opt &o : UPSCALER_OPTS)
                mw = SDL_max(mw, text_w(g_media.control, opt_label(o)) + 50);
            if (up == 0)
                dropdown_off(748, cy, sw, "100%");
            else if (up == 1 && (k = dropdown("scale", 748, cy, sw, SCALE_FSR1, opt_index(SCALE_FSR1, sc))) >= 0)
                cfg_set("scale", SCALE_FSR1[k].value);
            else if (up == 2 && (k = dropdown("scale", 748, cy, sw, SCALE_FSR3, opt_index(SCALE_FSR3, sc))) >= 0)
                cfg_set("scale", SCALE_FSR3[k].value);
            if ((k = dropdown("upscaler", 748 - sw - 8, cy, mw, UPSCALER_OPTS, up)) >= 0 && k != up) {
                if (k == 2)
                    cfg_set("antialias", "fsr3");
                else if (aa == "fsr3")   // FSR 3.1 left: the game's FXAA again
                    cfg_set("antialias", "");
                if (k == 0)
                    cfg_set("scale", "100");
                else if (sc == "100" || (k == 2 && (sc == "150" || sc == "200")))   // a scale the method takes: the game's
                    cfg_set("scale", "");
            }
        }
        cy += ROW;
        if (up != 2) {
            row_label(cy, "row_aa");
            if ((k = dropdown("aa", 748, cy, 0, AA_OPTS, opt_index(AA_OPTS, aa))) >= 0)
                cfg_set("antialias", AA_OPTS[k].value);
            cy += ROW;
        }
        row_label(cy, "row_sharp");
        if ((k = dropdown("sharp", 748, cy, 0, SHARP_OPTS, opt_index(SHARP_OPTS, cfg_get("sharpness"), "50"))) >= 0)
            cfg_set("sharpness", SHARP_OPTS[k].value);
        cy += ROW;
        row_label(cy, "row_fps");
        if ((k = dropdown("fps", 748, cy, 0, FPS_OPTS, opt_index(FPS_OPTS, cfg_or_platform("fps")))) >= 0)
            cfg_set("fps", FPS_OPTS[k].value);
        cy += ROW;
        row_label(cy, "row_vsync");
        std::string vs = cfg_get("vsync");
        bool on = !(vs == "0" || vs == "off");
        if (toggle("vsync", 748, cy, on))
            cfg_set("vsync", on ? "off" : "");
        cy += ROW;
        row_label(cy, "row_counter");   // the frame counter, with its key in the game
        text(g_media.small, 52 + text_w(g_media.label, T("row_counter")) + 10, cy - g_media.small->FontSize / S / 2, C_SUB,
             T("counter_key"));
        bool ctr = cfg_get("fps_overlay") == "1";
        if (toggle("fps_overlay", 748, cy, ctr))
            cfg_set("fps_overlay", ctr ? "" : "1");
    } else {   // the launcher's language (it was the header's button on the other screens), then the game's
        static const Opt UI_LANGS[] = {{"ru", "Русский"}, {"en", "English"}};
        row_label(cy, "row_ui_text");
        int k = dropdown("ui_lang", 748, cy, 0, UI_LANGS, g_lang == LANG_RU ? 0 : 1);
        if (k >= 0) {
            g_lang = lang_from_code(UI_LANGS[k].value);
            save();
        }
        DL->ChannelsSetCurrent(0);
        DL->AddRectFilled(P(36, top), P(764, cy + 28), C_CARD, 10 * S);
        DL->ChannelsSetCurrent(1);
        top = cy + 36;
        text(g_media.small, 52, top + 20 - g_media.small->FontSize / S / 2, C_SUB, T("sec_game_lang"));
        cy = top + 52;
        row_label(cy, "row_text");
        k = dropdown("lang", 748, cy, 0, LANG_OPTS, opt_index(LANG_OPTS, cfg_get("lang")));
        if (k >= 0)
            cfg_set("lang", LANG_OPTS[k].value);
    }
    DL->ChannelsSetCurrent(0);
    DL->AddRectFilled(P(36, top), P(764, cy + 28), C_CARD, 10 * S);
    DL->ChannelsSetCurrent(1);
    ImGui::SetCursorScreenPos(P(0, cy + 36));   // the body scrolls down to here
    ImGui::Dummy(ImVec2(1, 1));
}

// ---------------------------------------------------------------- Controls (the game reads them: src/host/window_sdl.c
// binds_load). pad_layout= nintendo (unset: A the right button) or xbox (A the bottom one); pad_<action>= SDL gamepad
// names (a = the bottom button, as SDL names them by position), key_<action>= SDL scancode names, comma separated, none:
// unbound; unset: the default below, which is the game's (window_sdl.c ACTIONS). A key or button bound to one action
// leaves the others. Keys may also be the mouse's buttons: Mouse Left, Mouse Right, Mouse Middle, Mouse 4, Mouse 5.
struct Ctl { const char *id, *label, *pad_nin, *pad_xbox, *keys; };
static const Ctl CTL_BUTTONS[] = {
    {"a", "act_a", "b", "a", "E"}, {"b", "act_b", "a", "b", "Left Shift"}, {"x", "act_x", "y", "x", "Space"},
    {"y", "act_y", "x", "y", "Mouse Left,T"}, {"l", "act_l", "leftshoulder", "leftshoulder", "Q"},
    {"r", "act_r", "rightshoulder", "rightshoulder", "R"}, {"zl", "act_zl", "lefttrigger", "lefttrigger", "Mouse Right,Z"},
    {"zr", "act_zr", "righttrigger", "righttrigger", "F,X"}, {"plus", "act_plus", "start", "start", "Escape"},
    {"minus", "act_minus", "back", "back", "Tab,M"}, {"lstick", "act_lstick", "leftstick", "leftstick", "Left Ctrl"},
    {"rstick", "act_rstick", "rightstick", "rightstick", "C"}, {"up", "act_up", "dpup", "dpup", "2"},
    {"down", "act_down", "dpdown", "dpdown", "4"}, {"left", "act_left", "dpleft", "dpleft", "1,Mouse 4"},
    {"right", "act_right", "dpright", "dpright", "3,Mouse 5"}};
static const Ctl CTL_MOVE[] = {
    {"move_up", "act_move_up", nullptr, nullptr, "W"}, {"move_down", "act_move_down", nullptr, nullptr, "S"},
    {"move_left", "act_move_left", nullptr, nullptr, "A"}, {"move_right", "act_move_right", nullptr, nullptr, "D"},
    {"cam_up", "act_cam_up", nullptr, nullptr, "Up"}, {"cam_down", "act_cam_down", nullptr, nullptr, "Down"},
    {"cam_left", "act_cam_left", nullptr, nullptr, "Left"}, {"cam_right", "act_cam_right", nullptr, nullptr, "Right"}};
static const Opt LAYOUT_OPTS[] = {{"nintendo", "layout_nintendo"}, {"xbox", "layout_xbox"}};

static int g_ctl_tab = 0;   // 0 the gamepad's buttons, 1 its sticks and gyro, 2 the keyboard
enum { CTL_PAD = 0, CTL_STICKS = 1, CTL_KEYS = 2 };
static struct { bool on = false, pad = false; std::string id; Uint64 t0 = 0; } g_cap;   // waiting for a key or button
static bool capturing() { return g_cap.on; }

// The Steam Deck: SteamDeck=1 (Steam sets it) or Valve's board, product Jupiter (LCD) or Galileo (OLED), as the game
// tells it (src/host/boot.c deck_defaults)
static bool on_deck() {
    const char *d = SDL_getenv("SteamDeck");
    if (d && !SDL_strcmp(d, "1"))
        return true;
#ifndef _WIN32
    static int dmi = -1;
    if (dmi < 0) {
        auto first_line = [](const char *path) {
            size_t n = 0;
            char *t = (char *)SDL_LoadFile(path, &n);
            std::string s = t ? std::string(t, n) : "";
            SDL_free(t);
            return s.substr(0, s.find_first_of("\r\n"));
        };
        std::string vendor = first_line("/sys/class/dmi/id/board_vendor"), product = first_line("/sys/class/dmi/id/product_name");
        dmi = vendor == "Valve" && (product == "Jupiter" || product == "Galileo");
    }
    return dmi == 1;
#else
    return false;
#endif
}
// The Xbox layout (A the bottom button): chosen, or unset on the Steam Deck, whose buttons are labelled so (the game's
// default there too, src/host/boot.c); pad_layout=nintendo keeps the Switch's on the Deck
static bool layout_xbox() {
    std::string v = cfg_get("pad_layout");
    return v == "xbox" || (v.empty() && on_deck());
}
static std::string ctl_cfg(bool pad, const char *id) { return std::string(pad ? "pad_" : "key_") + id; }
static std::vector<std::string> split_list(const std::string &v) {
    std::vector<std::string> out;
    size_t p = 0;
    while (p <= v.size()) {
        size_t e = v.find(',', p);
        if (e == std::string::npos)
            e = v.size();
        std::string t = v.substr(p, e - p);
        while (!t.empty() && t.front() == ' ')
            t.erase(t.begin());
        while (!t.empty() && t.back() == ' ')
            t.pop_back();
        if (!t.empty())
            out.push_back(t);
        p = e + 1;
    }
    return out;
}
static std::vector<std::string> ctl_current(bool pad, const Ctl &c) {
    std::string v = cfg_get(ctl_cfg(pad, c.id).c_str());
    if (v.empty())
        v = pad ? (layout_xbox() ? c.pad_xbox : c.pad_nin) : c.keys;
    if (v == "none")
        return {};
    return split_list(v);
}
// A gamepad input as the chosen layout names it: the Xbox (and Steam Deck) letters with A at the bottom, the Switch's
// with A on the right; the shoulders as the controller in use says (L1 on the Deck, LB on an Xbox pad, L on a Switch's)
static std::string pad_label(const std::string &n) {
    bool x = layout_xbox(), deck = on_deck();
    if (n == "a") return x ? "A" : "B";
    if (n == "b") return x ? "B" : "A";
    if (n == "x") return x ? "X" : "Y";
    if (n == "y") return x ? "Y" : "X";
    if (n == "leftshoulder") return deck ? "L1" : x ? "LB" : "L";
    if (n == "rightshoulder") return deck ? "R1" : x ? "RB" : "R";
    if (n == "lefttrigger") return deck ? "L2" : x ? "LT" : "ZL";
    if (n == "righttrigger") return deck ? "R2" : x ? "RT" : "ZR";
    if (n == "back") return x || deck ? "View" : "−";
    if (n == "start") return x || deck ? "Menu" : "+";
    if (n == "leftstick") return "LS";
    if (n == "rightstick") return "RS";
    if (n == "dpup") return "↑";
    if (n == "dpdown") return "↓";
    if (n == "dpleft") return "←";
    if (n == "dpright") return "→";
    if (n.rfind("paddle", 0) == 0) return "P" + n.substr(6);
    if (n == "misc1") return "Share";
    return n;
}
// A key as shown: its SDL name, the mouse's buttons in words
static std::string key_label(const std::string &n) {
    if (n == "Mouse Left") return T("mouse_left");
    if (n == "Mouse Right") return T("mouse_right");
    if (n == "Mouse Middle") return T("mouse_middle");
    return n;
}
static std::string ctl_default(bool pad, const Ctl &c) { return pad ? (layout_xbox() ? c.pad_xbox : c.pad_nin) : c.keys; }
static bool same_input(const std::string &a, const std::string &b) { return !SDL_strcasecmp(a.c_str(), b.c_str()); }
static bool has_input(const std::vector<std::string> &l, const std::string &s) {
    return std::any_of(l.begin(), l.end(), [&](const std::string &t) { return same_input(t, s); });
}
// Stored as the default (empty) when it is the default, "none" when nothing
static void ctl_store(bool pad, const Ctl &c, const std::vector<std::string> &l) {
    std::string v;
    for (const std::string &s : l)
        v += (v.empty() ? "" : ",") + s;
    if (v.empty())
        v = "none";
    std::vector<std::string> def = split_list(ctl_default(pad, c));
    bool is_def = def.size() == l.size();
    for (size_t i = 0; is_def && i < l.size(); i++)
        is_def = same_input(def[i], l[i]);
    cfg_set(ctl_cfg(pad, c.id).c_str(), is_def ? "" : v);
}
// The inputs go to this action; an action that had one of them gets this action's old input in its place (a swap:
// A to B's button gives B A's), so no action is left without one and no input does two
static void ctl_set(bool pad, const Ctl &c, const std::vector<std::string> &inputs) {
    std::vector<std::string> freed;
    for (const std::string &s : ctl_current(pad, c))
        if (!has_input(inputs, s))
            freed.push_back(s);
    size_t next = 0;
    auto swap_in = [&](const Ctl *list, size_t n) {
        for (size_t i = 0; i < n; i++) {
            if (!SDL_strcmp(c.id, list[i].id) || (pad && !list[i].pad_nin))
                continue;
            std::vector<std::string> cur = ctl_current(pad, list[i]), out;
            bool changed = false;
            for (const std::string &s : cur) {
                if (!has_input(inputs, s)) {
                    out.push_back(s);
                    continue;
                }
                changed = true;
                while (next < freed.size() && (has_input(out, freed[next]) || has_input(cur, freed[next])))
                    next++;   // already this action's: given to the next one instead
                if (next < freed.size())
                    out.push_back(freed[next++]);
            }
            if (changed)
                ctl_store(pad, list[i], out);
        }
    };
    swap_in(CTL_BUTTONS, sizeof CTL_BUTTONS / sizeof *CTL_BUTTONS);
    if (!pad)
        swap_in(CTL_MOVE, sizeof CTL_MOVE / sizeof *CTL_MOVE);
    ctl_store(pad, c, inputs);
}
static const Ctl *ctl_find(const std::string &id) {
    for (const Ctl &c : CTL_BUTTONS)
        if (id == c.id)
            return &c;
    for (const Ctl &c : CTL_MOVE)
        if (id == c.id)
            return &c;
    return nullptr;
}
static void ctl_assign(bool pad, const std::string &id, const std::string &input) {
    if (const Ctl *c = ctl_find(id))
        ctl_set(pad, *c, {input});
}
static void cap_done() {
    g_cap.on = false;
    g_input_held = true;   // the key or button just pressed clicks nothing until it is up
}

// Every SDL event first (main.cpp): while a key or button is awaited it is taken here, not by ImGui.
bool ui_event(const SDL_Event *e) {
    if (!g_cap.on)
        return false;
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: {
        SDL_Scancode sc = e->key.scancode;
        if (sc == SDL_SCANCODE_ESCAPE) {
            cap_done();
            return true;
        }
        // the game's own keys (window_sdl.c: F9 snapshots, F10 the frame counter, F11 full screen; Esc, which lets the
        // mouse go, cancels the wait above)
        bool reserved = sc == SDL_SCANCODE_F9 || sc == SDL_SCANCODE_F10 || sc == SDL_SCANCODE_F11;
        const char *name = SDL_GetScancodeName(sc);
        if (!g_cap.pad && !reserved && name && *name && !e->key.repeat) {
            ctl_assign(false, g_cap.id, name);
            cap_done();
        }
        return true;
    }
    case SDL_EVENT_KEY_UP: case SDL_EVENT_TEXT_INPUT: return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (g_cap.pad) {
            const char *name = SDL_GetGamepadStringForButton((SDL_GamepadButton)e->gbutton.button);
            if (name && *name) {
                ctl_assign(true, g_cap.id, name);
                cap_done();
            }
        }
        return true;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (g_cap.pad && e->gaxis.value > 20000 &&
            (e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) {
            ctl_assign(true, g_cap.id, SDL_GetGamepadStringForAxis((SDL_GamepadAxis)e->gaxis.axis));
            cap_done();
        }
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (!g_cap.pad && e->button.button >= 1 && e->button.button <= 5) {   // a key awaited: a mouse button does too
            static const char *const names[] = {"", "Mouse Left", "Mouse Middle", "Mouse Right", "Mouse 4", "Mouse 5"};
            ctl_assign(false, g_cap.id, names[e->button.button]);
            cap_done();   // the click itself clicks nothing
            return true;
        }
        g_cap.on = false;   // a button awaited: a click elsewhere, no longer waiting (the click itself goes on)
        return false;
    default: return false;
    }
}

// The back-to-default button: a circular arrow, 28 x 28 at (x, cy - 14)
static bool reset_button(const char *id, float x, float cy) {
    bool hov, down;
    bool clicked = hit(id, x, cy - 14, 28, 28, &hov, &down);
    if (hov || down)
        DL->AddRectFilled(P(x, cy - 14), P(x + 28, cy + 14), down ? C_DARK_DOWN : C_FIELD_HOVER, 6 * S);
    ImVec2 c = P(x + 14, cy);
    float r = 6.5f * S;
    ImU32 col = hov ? C_TEXT : C_SUB;
    // a circle open at the top right, its arrowhead at the top pointing back (anticlockwise): "undo"
    float a0 = -SDL_PI_F * 0.5f + 0.55f;
    DL->PathArcTo(c, r, a0, a0 + SDL_PI_F * 1.6f, 24);
    DL->PathStroke(col, 0, 1.5f * S);
    ImVec2 p(c.x + r * cosf(a0), c.y + r * sinf(a0)), t(sinf(a0), -cosf(a0)), nrm(cosf(a0), sinf(a0));
    float hs = 4.2f * S;
    DL->AddTriangleFilled(ImVec2(p.x + t.x * hs, p.y + t.y * hs), ImVec2(p.x + nrm.x * hs * 0.9f, p.y + nrm.y * hs * 0.9f),
                          ImVec2(p.x - nrm.x * hs * 0.9f, p.y - nrm.y * hs * 0.9f), col);
    if (hov)
        ImGui::SetTooltip("%s", T("ctl_reset"));
    return clicked;
}

static void ctl_row(float cy, bool pad, const Ctl &c) {
    row_label(cy, c.label);
    std::vector<std::string> cur = ctl_current(pad, c);
    std::string shown;
    for (const std::string &s : cur)
        shown += (shown.empty() ? "" : ", ") + (pad ? pad_label(s) : key_label(s));
    bool waiting = g_cap.on && g_cap.pad == pad && g_cap.id == c.id;
    if (waiting)
        shown = T(pad ? "press_button" : "press_key");
    else if (shown.empty())
        shown = T("ctl_unbound");
    float w = SDL_max(132.0f, text_w(g_media.control, shown.c_str()) + 28), x = 748 - w;
    bool hov, down;
    std::string hid = std::string(pad ? "bp_" : "bk_") + c.id;
    if (hit(hid.c_str(), x, cy - 16, w, 32, &hov, &down) && !waiting)
        g_cap.on = true, g_cap.pad = pad, g_cap.id = c.id, g_cap.t0 = SDL_GetTicks();
    DL->AddRectFilled(P(x, cy - 16), P(748, cy + 16), waiting ? C_DARK : hov ? C_FIELD_HOVER : C_FIELD, 6 * S);
    if (waiting)
        DL->AddRect(P(x, cy - 16), P(748, cy + 16), C_OK, 6 * S, 0, 1.5f * S);
    text(g_media.control, x + (w - text_w(g_media.control, shown.c_str())) / 2, cy - g_media.control->FontSize / S / 2,
         waiting ? C_OK : cur.empty() ? C_DIM : C_TEXT, shown.c_str());
    std::string key = ctl_cfg(pad, c.id);
    if (!cfg_get(key.c_str()).empty() && reset_button(("rs_" + hid).c_str(), x - 8 - 28, cy))
        ctl_set(pad, c, split_list(ctl_default(pad, c)));   // the default again, swapped as an assignment is
}

static void card(float top, float bottom) {   // drawn under what is on it (the body's channel 0)
    DL->ChannelsSetCurrent(0);
    DL->AddRectFilled(P(36, top), P(764, bottom), C_CARD, 10 * S);
    DL->ChannelsSetCurrent(1);
}

// ---------------------------------------------------------------- Sticks and gyro (the game reads them:
// src/host/window_sdl.c dead_zone, src/host/motion_fusion.c). stick_deadzone_left= / stick_deadzone_right= percent of the
// stick's travel that reads as centred (unset: 10), gyro_deadzone= degrees a second that read as no rotation (unset: the
// game's own, 1.2 with its HID reader of Switch controllers, 0 otherwise). GitHub #39: a stick that does not rest at its
// centre walks Link on his own; the page shows each stick live in its circle, the dead zone as a grey disc in it.
static const float STICK_DZ_DEFAULT = 10, GYRO_DZ_AUTO = 1.2f, GYRO_BAR_MAX = 5;   // percent; deg/s: Auto's knob, the bar's end

struct PadNow { bool pad = false, gyro = false; float lx = 0, ly = 0, rx = 0, ry = 0, g[3] = {}; };   // sticks -1..1, gyro deg/s
static bool g_demo_sticks;   // development: sample positions for the screenshots (ULTRS_LAUNCHER_DEMO=play_sticks)

static PadNow pad_now() {   // the first controller, as the game takes it
    PadNow p;
    if (g_demo_sticks) {
        p.pad = p.gyro = true;
        p.lx = 0.13f, p.ly = -0.07f, p.rx = 0.55f, p.ry = 0.32f;
        p.g[0] = 0.5f, p.g[1] = -0.6f, p.g[2] = 0.2f;
        return p;
    }
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    SDL_Gamepad *g = ids && n ? SDL_GetGamepadFromID(ids[0]) : nullptr;
    SDL_free(ids);
    if (!g)
        return p;
    p.pad = true;
    auto ax = [&](SDL_GamepadAxis a) { return SDL_GetGamepadAxis(g, a) / 32767.0f; };
    p.lx = ax(SDL_GAMEPAD_AXIS_LEFTX), p.ly = ax(SDL_GAMEPAD_AXIS_LEFTY);
    p.rx = ax(SDL_GAMEPAD_AXIS_RIGHTX), p.ry = ax(SDL_GAMEPAD_AXIS_RIGHTY);
    if (SDL_GamepadHasSensor(g, SDL_SENSOR_GYRO)) {
        if (!SDL_GamepadSensorEnabled(g, SDL_SENSOR_GYRO))
            SDL_SetGamepadSensorEnabled(g, SDL_SENSOR_GYRO, true);
        float r[3] = {};
        if (SDL_GetGamepadSensorData(g, SDL_SENSOR_GYRO, r, 3)) {   // rad/s
            p.gyro = true;
            for (int i = 0; i < 3; i++)
                p.g[i] = r[i] * 180 / SDL_PI_F;
        }
    }
    return p;
}

// The gyro's rate as the game sees it: its zero-rate bias (a still controller reads a little rotation) taken off, the
// bias learnt as a slow mean while the controller is about still (as motion_fusion.c learns it, more simply)
static float gyro_rate(const PadNow &p) {
    static float bias[3];
    static bool have;
    if (!p.gyro) {
        have = false;
        return 0;
    }
    if (!have) {
        for (int i = 0; i < 3; i++)
            bias[i] = p.g[i];
        have = true;
    }
    float d[3], r2 = 0;
    for (int i = 0; i < 3; i++)
        d[i] = p.g[i] - bias[i], r2 += d[i] * d[i];
    float rate = SDL_sqrtf(r2), dt = SDL_min(ImGui::GetIO().DeltaTime, 0.1f);
    if (rate < 10 && !g_demo_sticks)   // about still: the mean follows over ~2 s
        for (int i = 0; i < 3; i++)
            bias[i] += d[i] * dt / 2;
    return g_demo_sticks ? 0.8f : rate;
}

static std::string num_text(float v, int decimals) {   // 1.5 in English, 1,5 in Russian
    char b[32];
    SDL_snprintf(b, sizeof b, "%.*f", decimals, v);
    if (g_lang == LANG_RU)
        for (char *c = b; *c; c++)
            if (*c == '.')
                *c = ',';
    return b;
}

// A slider over [x, x + w] at cy: the mouse drags it, the d-pad and the arrow keys move it a step while it has the focus
// (they move the focus elsewhere only by up and down there). Returns true when the value changed this frame.
static bool slider(const char *id, float x, float cy, float w, float *v, float lo, float hi, float step) {
    bool hov, down;
    hit(id, x - 8, cy - 14, w + 16, 28, &hov, &down);
    ImGuiID iid = ImGui::GetItemID();
    float old = *v;
    if (down && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        float f = SDL_clamp(((ImGui::GetIO().MousePos.x - O.x) / S - x) / w, 0.0f, 1.0f);
        *v = lo + SDL_roundf(f * (hi - lo) / step) * step;
    }
    if (ImGui::IsItemFocused()) {
        static const ImGuiKey LESS[] = {ImGuiKey_GamepadDpadLeft, ImGuiKey_LeftArrow}, MORE[] = {ImGuiKey_GamepadDpadRight, ImGuiKey_RightArrow};
        for (ImGuiKey k : LESS) {
            ImGui::SetKeyOwner(k, iid);   // not the navigation's while here
            if (ImGui::IsKeyPressed(k, ImGuiInputFlags_Repeat, iid))
                *v -= step;
        }
        for (ImGuiKey k : MORE) {
            ImGui::SetKeyOwner(k, iid);
            if (ImGui::IsKeyPressed(k, ImGuiInputFlags_Repeat, iid))
                *v += step;
        }
        *v = SDL_clamp(*v, lo, hi);
    }
    float f = (*v - lo) / (hi - lo), kx = x + w * f;
    DL->AddRectFilled(P(x, cy - 2), P(x + w, cy + 2), C_BAR, 2 * S);
    DL->AddRectFilled(P(x, cy - 2), P(kx, cy + 2), C_DIM, 2 * S);
    dot(kx, cy, hov || down ? 9 : 8, hov || down ? C_LIGHT_HOVER : C_TEXT);
    return *v != old;
}

static struct {   // "Set up automatically": 3 s of the controller lying still, its sticks let go
    bool on = false;
    Uint64 t0 = 0;
    float maxr[2] = {};
    std::vector<std::array<float, 3>> gyro;
    std::string msg;
} g_calib;

static void calib_frame(const PadNow &p) {
    if (!g_calib.on)
        return;
    if (!p.pad) {
        g_calib.on = false;
        return;
    }
    float r[2] = {SDL_sqrtf(p.lx * p.lx + p.ly * p.ly), SDL_sqrtf(p.rx * p.rx + p.ry * p.ry)};
    for (int i = 0; i < 2; i++)
        g_calib.maxr[i] = SDL_max(g_calib.maxr[i], r[i]);
    if (p.gyro)
        g_calib.gyro.push_back({p.g[0], p.g[1], p.g[2]});
    float dev = 0;   // the gyro's largest distance from its mean: its noise at rest
    if (!g_calib.gyro.empty()) {
        float m[3] = {};
        for (auto &s : g_calib.gyro)
            for (int i = 0; i < 3; i++)
                m[i] += s[i] / g_calib.gyro.size();
        for (auto &s : g_calib.gyro) {
            float d2 = 0;
            for (int i = 0; i < 3; i++)
                d2 += (s[i] - m[i]) * (s[i] - m[i]);
            dev = SDL_max(dev, SDL_sqrtf(d2));
        }
    }
    if (g_calib.maxr[0] > 0.5f || g_calib.maxr[1] > 0.5f || dev > 20) {   // held or moved: not a resting reading
        g_calib.on = false;
        g_calib.msg = T("calib_moved");
        return;
    }
    if (SDL_GetTicks() - g_calib.t0 < 3000)
        return;
    g_calib.on = false;
    std::string shown[2];
    const char *keys[2] = {"stick_deadzone_left", "stick_deadzone_right"};
    for (int i = 0; i < 2; i++) {   // the rest's reach and a margin; the default when that is enough
        float dz = SDL_min(50.0f, SDL_ceilf(g_calib.maxr[i] * 100 * 1.2f + 3));
        cfg_set(keys[i], dz <= STICK_DZ_DEFAULT ? "" : std::to_string((int)dz));
        shown[i] = num_text(SDL_max(dz, STICK_DZ_DEFAULT), 0) + "%";
    }
    std::string g = T("dz_auto");
    if (p.gyro) {
        float dz = SDL_min(GYRO_BAR_MAX, SDL_roundf((dev * 1.5f + 0.2f) * 10) / 10);
        char b[16];   // the game reads it with a dot, whatever the language
        SDL_snprintf(b, sizeof b, "%.1f", dz);
        cfg_set("gyro_deadzone", dz <= GYRO_DZ_AUTO ? "" : b);
        if (dz > GYRO_DZ_AUTO)
            g = num_text(dz, 1) + T("deg_s");
    }
    g_calib.msg = Tf("calib_done", {{"l", shown[0]}, {"r", shown[1]}, {"g", g}});
}

static void stick_card(float x0, float top, const char *title, const char *key, float sx, float sy, bool pad) {
    const float W = 360;
    text(g_media.small, x0 + 16, top + 20 - g_media.small->FontSize / S / 2, C_SUB, T(title));
    std::string set = cfg_get(key);
    float dz = set.empty() ? STICK_DZ_DEFAULT : SDL_clamp((float)SDL_atof(set.c_str()), 0.0f, 50.0f);
    if (!set.empty() && reset_button((std::string("rs_") + key).c_str(), x0 + W - 16 - 28, top + 20))
        cfg_set(key, ""), dz = STICK_DZ_DEFAULT;
    const float R = 38, cx = x0 + W / 2, cy = top + 78;
    DL->AddCircleFilled(P(cx, cy), R * S, C_FIELD, 48);
    DL->AddLine(P(cx - R, cy), P(cx + R, cy), C_SEG_ON, S);
    DL->AddLine(P(cx, cy - R), P(cx, cy + R), C_SEG_ON, S);
    DL->AddCircleFilled(P(cx, cy), R * dz / 100 * S, IM_COL32(88, 88, 88, 255), 48);
    ring(cx, cy, R, C_DIM);
    if (pad) {   // where the stick is; green once the game sees it pushed
        float r = SDL_sqrtf(sx * sx + sy * sy), k = r > 1 ? 1 / r : 1;
        bool out = r * 100 > dz;
        dot(cx + sx * k * R, cy + sy * k * R, 5, out ? C_BAR_FILL : C_TEXT);
    }
    float ry = top + 132;
    text(g_media.label, x0 + 16, ry - g_media.label->FontSize / S / 2, C_TEXT, T("row_deadzone"));
    std::string val = num_text(dz, 0) + "%";
    text(g_media.control, x0 + W - 16 - text_w(g_media.control, val.c_str()), ry - g_media.control->FontSize / S / 2, C_TEXT, val.c_str());
    float lx = x0 + 32 + text_w(g_media.label, T("row_deadzone"));
    if (slider((std::string("sl_") + key).c_str(), lx, ry, x0 + W - 16 - 52 - lx, &dz, 0, 50, 1))
        cfg_set(key, dz == STICK_DZ_DEFAULT ? "" : std::to_string((int)dz));
}

static void screen_sticks() {
    PadNow p = pad_now();
    calib_frame(p);
    g_scrolling = true;   // live: frames at the display's rate while the page is shown
    float bw = text_w(g_media.medium, T("calib")) + 34;
    std::string calib_label = g_calib.on ? Tf("calib_wait", {{"n", std::to_string(3 - (int)((SDL_GetTicks() - g_calib.t0) / 1000))}})
                                         : std::string(T("calib"));
    bw = SDL_max(bw, text_w(g_media.medium, calib_label.c_str()) + 34);
    if (button("calib", calib_label.c_str(), 764 - bw, 98 - 16, bw, 32, DARK, p.pad && !g_calib.on)) {
        g_calib = {};
        g_calib.on = true, g_calib.t0 = SDL_GetTicks();
    }
    float top = 124;
    stick_card(36, top, "sec_lstick", "stick_deadzone_left", p.lx, p.ly, p.pad);
    stick_card(404, top, "sec_rstick", "stick_deadzone_right", p.rx, p.ry, p.pad);
    DL->ChannelsSetCurrent(0);
    DL->AddRectFilled(P(36, top), P(396, top + 152), C_CARD, 10 * S);
    DL->AddRectFilled(P(404, top), P(764, top + 152), C_CARD, 10 * S);
    DL->ChannelsSetCurrent(1);
    // the gyro: its rate on a bar, the dead zone the bar's grey start
    top += 160;
    text(g_media.small, 52, top + 20 - g_media.small->FontSize / S / 2, C_SUB, T("sec_gyro"));
    std::string set = cfg_get("gyro_deadzone");
    bool aut = set.empty();
    float dz = aut ? GYRO_DZ_AUTO : SDL_clamp((float)SDL_atof(set.c_str()), 0.0f, GYRO_BAR_MAX);
    if (!aut && reset_button("rs_gyro", 748 - 28, top + 20))
        cfg_set("gyro_deadzone", ""), aut = true, dz = GYRO_DZ_AUTO;
    float cy = top + 48, bx = 52, bwid = 300;
    float rate = gyro_rate(p);
    if (!p.pad || !p.gyro) {
        text(g_media.small, bx, cy - g_media.small->FontSize / S / 2, C_DIM, T(p.pad ? "no_gyro" : "no_pad"));
    } else {
        DL->AddRectFilled(P(bx, cy - 4), P(bx + bwid, cy + 4), C_BAR, 4 * S);
        DL->AddRectFilled(P(bx, cy - 4), P(bx + bwid * dz / GYRO_BAR_MAX, cy + 4), IM_COL32(88, 88, 88, 255), 4 * S);
        float mx = bx + bwid * SDL_min(rate, GYRO_BAR_MAX) / GYRO_BAR_MAX;
        DL->AddRectFilled(P(mx - 1.5f, cy - 9), P(mx + 1.5f, cy + 9), rate > dz ? C_BAR_FILL : C_TEXT, 1.5f * S);
        std::string rt = num_text(rate, 1) + T("deg_s");
        text(g_media.control, bx + bwid + 12, cy - g_media.control->FontSize / S / 2, C_SUB, rt.c_str());
    }
    float lx = 440;
    text(g_media.label, lx, cy - g_media.label->FontSize / S / 2, C_TEXT, T("row_deadzone"));
    std::string val = aut ? std::string(T("dz_auto")) : num_text(dz, 1) + T("deg_s");
    float vw = 64;
    text(g_media.control, 748 - text_w(g_media.control, val.c_str()), cy - g_media.control->FontSize / S / 2, C_TEXT, val.c_str());
    float sx = lx + 16 + text_w(g_media.label, T("row_deadzone"));
    if (slider("sl_gyro", sx, cy, 748 - vw - 12 - sx, &dz, 0, GYRO_BAR_MAX, 0.1f)) {
        char b[16];
        SDL_snprintf(b, sizeof b, "%.1f", dz);
        cfg_set("gyro_deadzone", b);
    }
    float bottom = top + 72;
    DL->ChannelsSetCurrent(0);
    DL->AddRectFilled(P(36, top), P(764, bottom), C_CARD, 10 * S);
    DL->ChannelsSetCurrent(1);
    const char *note = g_calib.msg.empty() ? T("sticks_note") : g_calib.msg.c_str();
    rich(g_media.small, 52, bottom + 5, 712, {{note, C_SUB}}, 17);
    ImGui::SetCursorScreenPos(P(0, bottom + 24));
    ImGui::Dummy(ImVec2(1, 1));
}

// The mouse (src/host/mouse_direct.c): mouse_sens= the camera's turn per count (unset: 1, 0.08 degrees a count),
// mouse_aim= its factor while the bow is drawn (unset: 1), mouse_invert=1 up and down swapped. Each row writes nothing
// at its default.
static void mouse_row_slider(float cy, const char *label, const char *key, float lo, float hi, float step) {
    row_label(cy, label);
    std::string set = cfg_get(key);
    float v = set.empty() ? 1.0f : SDL_clamp((float)SDL_atof(set.c_str()), lo, hi);
    std::string val = num_text(v, 1);
    text(g_media.control, 748 - text_w(g_media.control, val.c_str()), cy - g_media.control->FontSize / S / 2, C_TEXT, val.c_str());
    float x = 340;
    if (slider((std::string("sl_") + key).c_str(), x, cy, 748 - 52 - x, &v, lo, hi, step)) {
        char b[16];   // the game reads it with a dot, whatever the language
        SDL_snprintf(b, sizeof b, "%.1f", v);
        cfg_set(key, SDL_fabsf(v - 1.0f) < 0.01f ? "" : b);
    }
}

static void mouse_section(float &top) {
    const float ROW = 38;
    text(g_media.small, 52, top + 20 - g_media.small->FontSize / S / 2, C_SUB, T("sec_mouse"));
    float cy = top + 52;
    mouse_row_slider(cy, "row_mouse_sens", "mouse_sens", 0.1f, 5.0f, 0.1f);
    cy += ROW;
    mouse_row_slider(cy, "row_mouse_aim", "mouse_aim", 0.1f, 3.0f, 0.1f);
    cy += ROW;
    row_label(cy, "row_mouse_invert");
    bool inv = cfg_get("mouse_invert") == "1";
    if (toggle("mouse_invert", 748, cy, inv))
        cfg_set("mouse_invert", inv ? "" : "1");
    float h = rich(g_media.small, 52, cy + 24, 696, {{T("mouse_note"), C_SUB}}, 17);
    float bottom = cy + 24 + h + 12;
    card(top, bottom);
    top = bottom + 8;
}

static void screen_controls() {
    if (g_cap.on && SDL_GetTicks() - g_cap.t0 > 8000)   // nothing pressed for a while: no longer waiting
        g_cap.on = false;
    static const Opt DEV_OPTS[] = {{"pad", "ctl_gamepad"}, {"sticks", "ctl_sticks"}, {"key", "ctl_keyboard"}};
    float sw = 4;
    for (const Opt &o : DEV_OPTS)
        sw += text_w(g_media.control, opt_label(o)) + 24;
    int k = segmented("ctl_dev", 36 + sw, 98, DEV_OPTS, g_ctl_tab);   // buttons, sticks or the keyboard, left under the tabs
    if (k >= 0)
        g_ctl_tab = k, g_cap.on = false;
    if (g_ctl_tab == CTL_STICKS) {
        screen_sticks();
        return;
    }
    bool pad = g_ctl_tab == CTL_PAD;
    const float ROW = 38;
    float top = 124, cy;
    if (pad) {   // the layout: which face button is A
        text(g_media.small, 52, top + 20 - g_media.small->FontSize / S / 2, C_SUB, T("sec_layout"));
        cy = top + 52;
        row_label(cy, "row_layout");
        // stored only when it differs from the platform's default (the Xbox one on the Steam Deck)
        if ((k = segmented("pad_layout", 748, cy, LAYOUT_OPTS, layout_xbox() ? 1 : 0)) >= 0)
            cfg_set("pad_layout", !SDL_strcmp(LAYOUT_OPTS[k].value, on_deck() ? "xbox" : "nintendo") ? "" : LAYOUT_OPTS[k].value);
        float h = rich(g_media.small, 52, cy + 24, 696, {{T("layout_note"), C_SUB}}, 17);
        float bottom = cy + 24 + h + 12;
        card(top, bottom);
        top = bottom + 8;
    }
    auto section = [&](const char *title, const Ctl *list, size_t n) {
        text(g_media.small, 52, top + 20 - g_media.small->FontSize / S / 2, C_SUB, T(title));
        cy = top + 52;
        for (size_t i = 0; i < n; i++, cy += ROW)
            ctl_row(cy, pad, list[i]);
        float bottom = cy - ROW + 28;
        card(top, bottom);
        top = bottom + 8;
    };
    section("sec_buttons", CTL_BUTTONS, sizeof CTL_BUTTONS / sizeof *CTL_BUTTONS);
    if (!pad) {
        section("sec_move", CTL_MOVE, sizeof CTL_MOVE / sizeof *CTL_MOVE);
        mouse_section(top);
    }
    // when they apply, and everything of this device back to the defaults (the layout stays)
    float bw = text_w(g_media.medium, T("ctl_reset_all")) + 34;
    rich(g_media.small, 52, top + 12, 748 - bw - 16 - 52, {{T("ctl_applies"), C_SUB}}, 17);
    if (button("ctl_reset_all", T("ctl_reset_all"), 748 - bw, top + 6, bw, 38, DARK)) {
        for (const Ctl &c : CTL_BUTTONS)
            cfg_set(ctl_cfg(pad, c.id).c_str(), "");
        if (!pad)
            for (const Ctl &c : CTL_MOVE)
                cfg_set(ctl_cfg(pad, c.id).c_str(), "");
        g_cap.on = false;
    }
    ImGui::SetCursorScreenPos(P(0, top + 56));   // the body scrolls down to here
    ImGui::Dummy(ImVec2(1, 1));
}

// ---------------------------------------------------------------- Mods
// The mods folder (src/host/mods.c): what tools/mods_code.py list finds there, each mod switched on or off and moved up
// or down, its cheats switched one by one. Written to ultratears.cfg (mods_order, mods_off, cheats_on): the game reads
// them when it starts. The list is read again after every change (the counts depend on the order).
static struct {
    SetupProc *p = nullptr;
    bool loaded = false, failed = false, again = false;
    Json data;
    std::vector<int> order;            // indices into data["mods"], top first
    std::vector<std::string> open;     // mods whose cheats are shown
} M;

static std::string mods_dir() {
    std::string d = cfg_get("mods_dir");
    return d.empty() ? path_join(U.install, "mods") : (d.size() > 1 && (d[0] == '/' || d[1] == ':') ? d : path_join(U.install, d));
}
static void mods_list() {
    if (M.p) {
        M.again = true;   // a change while the list is read: read it once more after
        return;
    }
    std::string py = python_path(U.install);
    if (py.empty()) {
        M.failed = true;
        return;
    }
    M.p = setup_start({py, path_join(U.install, "tools/mods_code.py"), "list"}, U.install, path_join(U.install, "ultratears-mods.log"));
}
static void mods_poll() {
    if (!M.p)
        return;
    std::string line, last;
    while (setup_line(M.p, line))
        if (!line.empty() && line[0] == '{')
            last = line;
    if (!last.empty()) {
        Json j;
        if (json_parse(last, j)) {
            M.data = j, M.loaded = true, M.failed = false;
            M.order.clear();
            for (size_t i = 0; i < M.data["mods"].size(); i++)
                M.order.push_back((int)i);
        }
    }
    int code = 0;
    if (!setup_finished(M.p, &code))
        return;
    setup_free(M.p);
    M.p = nullptr;
    if (code != 0 && !M.loaded)
        M.failed = true;
    if (M.again) {
        M.again = false;
        mods_list();
    }
}
static void mods_save() {   // the order, the switched-off mods and the cheats that are on, from what the page shows
    std::string order, off;
    for (int i : M.order) {
        const Json &m = M.data["mods"][(size_t)i];
        order += (order.empty() ? "" : "|") + m["name"].str();
        if (!m["enabled"].truth())
            off += (off.empty() ? "" : "|") + m["name"].str();
    }
    std::string on;
    for (size_t i = 0; i < M.data["cheats_on"].size(); i++)
        on += (on.empty() ? "" : ",") + M.data["cheats_on"][i].str();
    cfg_set("mods_order", order);
    cfg_set("mods_off", off);
    cfg_set("cheats_on", on);
    mods_list();
}
static bool cheat_on(const std::string &id) {
    const Json &l = M.data["cheats_on"];
    for (size_t i = 0; i < l.size(); i++)
        if (l[i].str() == id)
            return true;
    return false;
}
static void cheat_set(const std::string &id, bool on) {
    Json &l = const_cast<Json &>(M.data["cheats_on"]);
    if (l.kind != Json::Arr) {   // none listed yet: a list of its own in the object
        M.data.keys.push_back("cheats_on");
        M.data.items.push_back(Json());
        M.data.items.back().kind = Json::Arr;
        return cheat_set(id, on);
    }
    for (size_t i = 0; i < l.items.size(); i++)
        if (l.items[i].str() == id) {
            if (!on)
                l.items.erase(l.items.begin() + (long)i);
            return;
        }
    if (on) {
        Json v;
        v.kind = Json::Str, v.s = id;
        l.items.push_back(v);
    }
}
static std::string fit(ImFont *f, const std::string &s, float w) {   // cut with an ellipsis to w design pixels
    if (text_w(f, s.c_str()) <= w)
        return s;
    std::string t = s;
    while (!t.empty() && text_w(f, (t + "...").c_str()) > w) {
        t.pop_back();
        while (!t.empty() && ((unsigned char)t.back() & 0xC0) == 0x80)   // a whole UTF-8 character
            t.pop_back();
    }
    return t + "...";
}
static std::string mod_summary(const Json &m) {
    std::vector<std::string> parts;
    if (m["romfs"].truth()) {
        parts.push_back(Tf("mod_files", {{"r", m["replaced"].str()}, {"a", m["added"].str()}}));
        if (m["other_version"].num() > 0)
            parts.push_back(Tf("mod_other_version", {{"n", m["other_version"].str()}}));
        if (m["overridden"].num() > 0)
            parts.push_back(Tf("mod_overridden", {{"n", m["overridden"].str()}, {"m", m["overridden_by"].str()}}));
    }
    if (m["patch_files"].num() > 0)
        parts.push_back(T("mod_code"));
    else if (m["other_build"].num() > 0)
        parts.push_back(T("mod_code_other"));
    if (m["cheats"].size())
        parts.push_back(Tf("mod_cheats", {{"n", std::to_string(m["cheats"].size())}}));
    std::string p = m["problem"].str();
    if (p == "empty")
        parts = {T("mod_empty")};
    else if (p == "bad_name")
        parts = {T("mod_bad_name")};
    std::string out;
    for (const std::string &s : parts)
        out += (out.empty() ? "" : " · ") + s;
    return out;
}

static void screen_mods() {
    if (!M.loaded && !M.p && !M.failed)
        mods_list();
    mods_poll();
    float top = 80;
    // the folder: open it (made when missing), read it again
    float bw = text_w(g_media.medium, T("mods_folder")) + 34, rw = text_w(g_media.medium, T("mods_refresh")) + 34;
    rich(g_media.small, 52, top + 4, 748 - bw - rw - 24 - 52, {{T("mods_note"), C_SUB}}, 17);
    if (button("mods_refresh", T("mods_refresh"), 748 - bw - 8 - rw, top, rw, 38, DARK, !M.p))
        mods_list();
    if (button("mods_folder", T("mods_folder"), 748 - bw, top, bw, 38, DARK)) {
        std::string d = mods_dir();
        SDL_CreateDirectory(d.c_str());
        SDL_OpenURL(("file:///" + d).c_str());
    }
    top += 56;
    if (!M.loaded) {
        text(g_media.label, 52, top + 10, M.failed ? C_ERR : C_SUB, T(M.failed ? "mods_failed" : "mods_loading"));
        return;
    }
    const Json &mods = M.data["mods"];
    if (!mods.size()) {
        float h = rich(g_media.small, 52, top + 16, 696, {{T("mods_none"), C_SUB}}, 17);
        card(top, top + 32 + h);
        return;
    }
    if (M.data["code"].str() == "stale") {
        float h = rich(g_media.small, 52, top, 696, {{T("mods_code_stale"), C_SUB}}, 17);
        top += h + 12;
    }
    for (size_t k = 0; k < M.order.size(); k++) {
        int i = M.order[k];
        const Json &m = mods[(size_t)i];
        std::string name = m["name"].str(), id = "mod" + std::to_string(i);
        bool on = m["enabled"].truth();
        float cy = top + 26;
        // the switch at the right, Up and Down left of it, the cheats' button left of those
        float uw = text_w(g_media.medium, T("mod_up")) + 24, dw = text_w(g_media.medium, T("mod_down")) + 24;
        float x = 748 - 38 - 16 - dw;
        if (button((id + "_dn").c_str(), T("mod_down"), x, cy - 15, dw, 30, DARK, k + 1 < M.order.size())) {
            std::swap(M.order[k], M.order[k + 1]);
            mods_save();
        }
        x -= uw + 6;
        if (button((id + "_up").c_str(), T("mod_up"), x, cy - 15, uw, 30, DARK, k > 0)) {
            std::swap(M.order[k], M.order[k - 1]);
            mods_save();
        }
        size_t nch = m["cheats"].size();
        bool shown = std::find(M.open.begin(), M.open.end(), name) != M.open.end();
        if (nch) {
            std::string l = shown ? T("mod_cheats_hide") : Tf("mod_cheats_show", {{"n", std::to_string(nch)}});
            float cw = text_w(g_media.medium, l.c_str()) + 24;
            x -= cw + 6;
            if (button((id + "_ch").c_str(), l.c_str(), x, cy - 15, cw, 30, DARK)) {
                if (shown)
                    M.open.erase(std::find(M.open.begin(), M.open.end(), name));
                else
                    M.open.push_back(name);
            }
        }
        if (toggle((id + "_on").c_str(), 748, cy, on)) {
            const_cast<Json &>(m["enabled"]).kind = Json::Bool;
            const_cast<Json &>(m["enabled"]).b = !on;
            mods_save();
        }
        text(g_media.label, 52, cy - g_media.label->FontSize / S / 2, on ? C_TEXT : C_DIM, fit(g_media.label, name, x - 16 - 52).c_str());
        std::string sum = mod_summary(m);
        float h = rich(g_media.small, 52, cy + 20, 696, {{sum.c_str(), m["problem"].str().empty() ? C_SUB : C_ERR}}, 17);
        float bottom = cy + 20 + h + 10;
        if (shown) {   // its cheats: a switch each; headings, the always-on and the ones the port cannot run without one
            for (size_t c = 0; c < nch; c++) {
                const Json &ch = m["cheats"][c];
                std::string kind = ch["kind"].str(), cid = ch["id"].str();
                float ccy = bottom + 16;
                ImU32 col = kind == "unsupported" || kind == "restore" || !on ? C_DIM : C_TEXT;
                if (kind == "heading") {
                    text(g_media.small, 68, ccy - g_media.small->FontSize / S / 2, C_SUB, fit(g_media.small, ch["name"].str(), 680).c_str());
                    bottom = ccy + 12;
                    continue;
                }
                std::string why;
                if (ch["master"].truth())
                    why = T("cheat_always");
                else if (kind == "restore")
                    why = T("cheat_restore");
                else if (kind == "unsupported")
                    why = T(("cheat_why_" + ch["why"].str()).c_str());
                else if (ch["keys"].num() > 0)
                    why = T("cheat_keys");
                float nw = why.empty() ? 600 : 360;
                text(g_media.regular, 68, ccy - g_media.regular->FontSize / S / 2, col, fit(g_media.regular, ch["name"].str(), nw).c_str());
                if (!why.empty())
                    text(g_media.small, 68 + nw + 12, ccy - g_media.small->FontSize / S / 2, C_SUB, fit(g_media.small, why, 748 - 50 - 68 - nw - 12).c_str());
                if ((kind == "data" || kind == "code") && !ch["master"].truth()) {
                    bool con = cheat_on(cid);
                    if (toggle((id + "_c" + cid).c_str(), 748, ccy, con)) {
                        cheat_set(cid, !con);
                        mods_save();
                    }
                }
                bottom = ccy + 16;
            }
            bottom += 8;
        }
        card(top, bottom);
        top = bottom + 8;
    }
    ImGui::SetCursorScreenPos(P(0, top + 56));   // the body scrolls down to here
    ImGui::Dummy(ImVec2(1, 1));
}

static void screen_update() {
    float y = title(82, Tf("upd_title", {{"v", U.upd.version}}).c_str(), nullptr) + 6;
    std::string from = Tf("upd_from", {{"old", U.version}, {"v", U.upd.version}});
    y += rich(g_media.regular, 36, y, 700, {{from.c_str(), C_SUB}, {" ", C_SUB}, {T("upd_notes"), C_TEXT, U.upd.page.c_str()}});
    y += 22;
    if (U.upd_st == 1 || U.upd_st == 2) {
        uint64_t got = 0, total = 0;
        update_download_poll(&got, &total, nullptr);
        double f = U.upd_st == 1 ? (total ? (double)got / total : 0) : U.apply_frac;
        char amount[64];
        if (U.upd_st == 1)
            SDL_snprintf(amount, sizeof amount, "%.0f / %.0f MB", got / 1048576.0, total / 1048576.0);
        else
            SDL_snprintf(amount, sizeof amount, "%d%%", (int)(f * 100));
        text(g_media.regular, 36, y, C_TEXT, T(U.upd_st == 1 ? "upd_download" : "upd_apply"));
        text(g_media.regular, 764 - text_w(g_media.regular, amount), y, C_TEXT, amount);
        y += 26;
        DL->AddRectFilled(P(36, y), P(764, y + 8), C_BAR, 4 * S);
        if (f > 0)
            DL->AddRectFilled(P(36, y), P(36 + 728 * (float)f, y + 8), C_BAR_FILL, 4 * S);
        y += 30;
        rich(g_media.small, 36, y, 728, {{T("upd_keep"), C_SUB}});
    } else if (U.upd_st == 3) {
        icon(g_media.error, 36, y + 2);
        text(g_media.regular, 60, y, C_TEXT, T("upd_failed"));
        y += 24;
        rich(g_media.regular, 60, y, 700, {{U.upd_err.c_str(), C_SUB}});
    } else {
        rich(g_media.regular, 36, y, 700, {{T("upd_what"), C_SUB}});
    }
}

static void screen_update_footer() {
    if (U.upd_st == 2)
        return;   // files are being replaced: no way back halfway (a second try would finish it)
    if (U.upd_st == 1) {
        float w = text_w(g_media.medium, T("cancel")) + 40;
        if (button("upd_cancel", T("cancel"), 36, 416, w, 40, DARK)) {
            update_download_cancel();
            U.upd_st = 0;
            U.screen = installed() && !stale() ? SCR_PLAY : SCR_DUMP;
        }
        return;
    }
    float w1 = SDL_max(160.0f, text_w(g_media.medium, T("upd_go")) + 48), w2 = text_w(g_media.medium, T("upd_later")) + 40;
    if (button("upd_go", T(U.upd_st == 3 ? "upd_retry" : "upd_go"), 764 - w1, 408, w1, 48, PRIMARY)) {
        U.upd_err.clear();
        U.upd_st = 1;
        update_download_start(U.upd, path_join(update_folder(), "update"));
    }
    if (button("upd_later", T("upd_later"), 764 - w1 - 12 - w2, 408, w2, 48, DARK)) {
        U.upd_st = 0;
        U.screen = installed() && !stale() ? SCR_PLAY : SCR_DUMP;
    }
}

// "Version X is out — Update" next to the language button, while nothing is being built
static void update_banner(float right) {
    if (!U.has_upd || U.screen != SCR_DUMP)   // the Play screen offers it at the bottom
        return;
    std::string t = Tf("upd_banner", {{"v", U.upd.version}});
    const char *go = T("upd_open");
    float bw = text_w(g_media.medium, go) + 28, x = right - 12 - bw;
    text(g_media.small, x - 10 - text_w(g_media.small, t.c_str()), 40 - g_media.small->FontSize / S / 2, C_SUB, t.c_str());
    if (button("upd_banner", go, x, 24, bw, 32, PRIMARY))
        U.screen = SCR_UPDATE;
}

static bool g_play_failed = false;

// ---- Play: first the shaders this graphics driver has not compiled yet (UltraTears --prepare-shaders, gfx/vkb_shader.c):
// the Steam Deck's folder built on a PC, a driver update, a game update. With nothing to do the game says "ready" in
// about a second. A driver crash kills it; it runs again (it skips what the driver crashed on), up to 3 times, and the
// game starts whatever happened: what is not compiled now is compiled while playing. --play (Steam's launch option,
// AppRun): the window stays hidden unless there is something to compile, and the game then takes the launcher's place.
static struct {
    SetupProc *p = nullptr;
    SetupProc *mods = nullptr;    // tools/mods_code.py build, before the graphics are prepared
    int tries = 0;
    double done = 0, total = 0;   // total 0: checking
    bool play_mode = false, show = false, quit = false;
} G;

static void gfx_start() {
    std::string exe = game_exe(U.install);
    G.done = G.total = 0;
    G.p = setup_start({exe, "--prepare-shaders"}, U.install, path_join(U.install, "ultratears-prepare.log"));
}

// Play: first the code the mods' patches and code cheats change, built when their files changed since the last time (a few
// seconds; nothing when they did not), then the graphics. A failed build starts the game without them (it says so in its log).
static void play_start() {
    std::string py = python_path(U.install);
    if (path_is_dir(mods_dir()) && !py.empty() && path_is_file(path_join(U.install, "tools/mods_code.py"))) {
        G.mods = setup_start({py, path_join(U.install, "tools/mods_code.py"), "build"}, U.install,
                             path_join(U.install, "ultratears-mods.log"));
        return;
    }
    gfx_start();
}

static void play_now() {
    if (G.play_mode)   // main.cpp closes the window, then ui_play_finish starts the game
        G.quit = true;
    else
        g_play_failed = !start_game(U.install);
}

static void gfx_poll() {
    if (G.mods) {
        std::string line;
        while (setup_line(G.mods, line))
            if (!line.empty())
                SDL_Log("mods: %s", line.c_str());
        int code = 0;
        if (!setup_finished(G.mods, &code))
            return;
        setup_free(G.mods);
        G.mods = nullptr;
        if (code != 0)
            SDL_Log("mods: the code of the mods' patches and cheats was not built (exit code %d): the game starts without it", code);
        M.loaded = false;   // the Mods page reads the folder again (the code is built now)
        gfx_start();
        return;
    }
    if (!G.p)
        return;
    std::string line;
    while (setup_line(G.p, line)) {
        unsigned long long d, t;
        if (SDL_sscanf(line.c_str(), "prepare-shaders: %llu of %llu", &d, &t) == 2)
            G.done = (double)d, G.total = (double)t, G.show = true;
        else if (!SDL_strncmp(line.c_str(), "prepare-shaders:", 16))
            SDL_Log("%s", line.c_str());
    }
    int code = 0;
    if (!setup_finished(G.p, &code))
        return;
    setup_free(G.p);
    G.p = nullptr;
    if (code != 0 && ++G.tries < 3) {
        SDL_Log("prepare-shaders: exit code %d, again without the shaders it was on", code);
        gfx_start();
        return;
    }
    play_now();
}

void ui_play_mode() {   // main.cpp: --play
    std::string here = default_install();   // the game the AppImage lies in (as AppRun took it before), whatever the settings say
    if (path_is_file(path_join(here, "ultratears.cfg")))
        use_install(here), link_deps();
    if (!installed() || stale())
        return;   // nothing to play yet, or an update to finish: the launcher as usual
    G.play_mode = true;
    U.screen = SCR_PLAY;
    play_start();
}
bool ui_window_wanted() { return !G.play_mode || G.show; }
bool ui_quit_wanted() { return G.quit; }
// --play, the window closed: on Linux the launcher's process becomes the game (Steam follows it; with the window open its
// display connection would pass to the game and keep the window up), elsewhere the game is started
bool ui_play_finish() {
    if (!G.quit)
        return false;
    return exec_game(U.install) || start_game(U.install);
}

static void screen_play_footer() {
    if (G.mods) {
        text(g_media.regular, 36, 404, C_TEXT, T("mods_building"));
        text(g_media.small, 36, 448, C_SUB, T("mods_building_note"));
        return;
    }
    if (G.p) {   // the graphics prepared, before the game starts
        std::string t = G.total > 0 ? Tf("gfx_prep", {{"done", std::to_string((long long)G.done)}, {"total", std::to_string((long long)G.total)}})
                                    : std::string(T("gfx_check"));
        text(g_media.regular, 36, 404, C_TEXT, t.c_str());
        DL->AddRectFilled(P(36, 432), P(764, 440), C_BAR, 4 * S);
        if (G.total > 0)
            DL->AddRectFilled(P(36, 432), P(36 + 728 * (float)SDL_min(1.0, G.done / G.total), 440), C_BAR_FILL, 4 * S);
        text(g_media.small, 36, 448, C_SUB, T("gfx_note"));
        return;
    }
    // Play; with a newer release found also Update, and what is new (the design: Play grey beside a green Update)
    if (g_focus_play) {   // the Play screen shown: Play has the focus (A starts the game), its frame with a controller
        ImGui::SetKeyboardFocusHere();
        ImGui::SetNavCursorVisible(SDL_HasGamepad());
        g_focus_play = false;
    }
    if (button("play", T("play"), 36, 408, 116, 48, U.has_upd ? DARK : PRIMARY)) {
        g_play_failed = false;
        G.tries = 0;
        play_start();
    }
    float x = 168;
    if (U.has_upd) {
        if (button("upd_open", T("upd_open"), 160, 408, 136, 48, PRIMARY))
            U.screen = SCR_UPDATE;
        x = 312;
        std::string a = Tf("upd_avail", {{"v", U.upd.version}});
        rich(g_media.label, x, 408, 450, {{(a + ".").c_str(), C_TEXT}, {" ", C_TEXT}, {T("upd_notes"), C_TEXT, U.upd.page.c_str()}});
    }
    if (g_play_failed) {
        text(g_media.small, x, U.has_upd ? 434 : 425, C_ERR, T("play_failed"));
        return;
    }
    if (!U.version_line.empty()) {
        std::string v = Tf(U.has_upd ? "cur_ver" : "port_ver", {{"v", U.version_line}});
        text(g_media.small, x, (U.has_upd ? 443 : 432) - g_media.small->FontSize / S / 2, C_SUB, v.c_str());
    }
}

// ---------------------------------------------------------------- frame
static void background(ImDrawList *bg, ImVec2 size) {
    bg->AddRectFilled(ImVec2(0, 0), size, C_BG);
    const Icon &p = g_media.pattern;
    if (!p.tex)
        return;
    float t = p.w * S;
    for (float y = 0; y < size.y; y += t)
        for (float x = 0; x < size.x; x += t)
            bg->AddImage((ImTextureID)(intptr_t)p.tex, ImVec2(x, y), ImVec2(x + t, y + t));
}

void ui_init(SDL_Window *w) {
    g_win = w;
    U.lock = SDL_CreateMutex();
    U.set = settings_load();
    std::string lang = U.set.get("lang");
    g_lang = lang.empty() ? system_lang() : lang_from_code(lang.c_str());
    U.dump = U.set.get("dump");
    // The builder package the launcher lies in: the game is built there (setup.py in place). ULTRS_ROOT: development,
    // a package or a folder made by setup.py --output elsewhere. A source checkout has no launcher: setup.py there.
    U.home = exe_dir();
    const char *root = SDL_getenv("ULTRS_ROOT");
    if (root && *root)
        U.home = root;
#ifdef _WIN32
    for (char &c : U.home)   // shown as Windows writes paths
        if (c == '/')
            c = '\\';
#endif
    std::string inst = U.set.get("install");
    use_install(inst.empty() ? (root && *root ? U.home : default_install()) : inst);
    link_deps();
    // the platform chosen last; else what the folder was built for (data/setup.json "platforms"), else this PC
    U.platform = U.set.get("platform");
#ifndef _WIN32
    U.platform = "linux";   // the Linux launcher builds for its own machine
#endif
    if (opt_index(PLATFORM_OPTS, U.platform) < 0) {
        Json st = setup_state();
        const Json &p = st["platforms"];
        bool win = false, lin = false;
        for (size_t i = 0; i < p.size(); i++)
            win |= p[i].str() == "windows", lin |= p[i].str() == "linux";
        U.platform = win && lin ? "both" : lin ? "linux" : "windows";
    }
    update_cleanup(U.root);
    // updates: one question to GitHub at every start, in the background (no answer: nothing shown); a release found
    // stays offered until it is installed
    U.upd.version = U.set.get("update_version");
    if (!U.upd.version.empty() && U.upd.version != U.version && !U.version.empty()) {
        U.upd.name = U.set.get("update_name"), U.upd.url = U.set.get("update_url");
        U.upd.sha256 = U.set.get("update_sha256"), U.upd.page = U.set.get("update_page");
        U.upd.size = SDL_strtoull(U.set.get("update_size").c_str(), nullptr, 10);
        U.has_upd = !U.upd.url.empty() && !U.upd.sha256.empty();
    }
    if (U.set.get("updates") != "off" && !U.version.empty() && !python_path(U.root).empty())
        update_check_start(U.version);
    ImGuiStyle &st = ImGui::GetStyle();
    st.WindowPadding = ImVec2(0, 0);
    st.WindowBorderSize = 0;
    st.ScrollbarSize = 6;
    st.ScrollbarRounding = 3;
    st.Colors[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
    st.Colors[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    st.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    st.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(1, 1, 1, 0.18f);
    st.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1, 1, 1, 0.3f);
    st.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(1, 1, 1, 0.4f);
    st.Colors[ImGuiCol_PopupBg] = ImVec4(0.14f, 0.14f, 0.14f, 1);
    st.Colors[ImGuiCol_NavCursor] = ImVec4(0, 0, 0, 0);   // the focus frame is drawn by hit()
    st.PopupRounding = 6;
    st.WindowRounding = 6;
    // the game carried to a Steam Deck in the tar from the PC (tools/make_deck_tar.py): no builder beside it, only Play
    // and the settings; the PC's launcher builds and updates it
    U.can_build = !python_path(U.root).empty();
    if (!U.can_build && !installed())
        U.screen = SCR_BROKEN;
    else if (!U.can_build || (installed() && (!U.dump.empty() || data_ready()) && !stale()))
        U.screen = SCR_PLAY;
    else
        dump_state();
}

void ui_frame(float scale) {
    S = scale;
    poll();
    gfx_poll();
    {   // entering the Play screen (at start, after a build, back from an update): Play takes the focus
        static int last_screen = -1;
        if (U.screen == SCR_PLAY && last_screen != SCR_PLAY)
            g_focus_play = true;
        last_screen = U.screen;
    }
    ImGuiIO &io = ImGui::GetIO();
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        g_drag_scrolled = false;   // the drag is over (its release frame clicked nothing)
    if (g_input_held) {   // a key or button was just bound: clicks again once nothing is held
        // from SDL, not ImGui: with its gamepad navigation off ImGui's gamepad keys stay as they were (the button that
        // started the wait would read held for ever, and nothing would click again)
        bool held = SDL_GetMouseState(nullptr, nullptr) != 0;
        int n = 0;
        const bool *keys = SDL_GetKeyboardState(&n);
        for (int i = 0; i < n && !held; i++)
            held = keys[i];
        // the controllers themselves (ImGui's gamepad keys may not follow while its gamepad navigation is off)
        SDL_JoystickID *ids = SDL_GetGamepads(&n);
        for (int i = 0; ids && i < n && !held; i++) {
            SDL_Gamepad *g = SDL_GetGamepadFromID(ids[i]);
            for (int b = 0; g && b < SDL_GAMEPAD_BUTTON_COUNT && !held; b++)
                held = SDL_GetGamepadButton(g, (SDL_GamepadButton)b);
            if (g && !held)
                held = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 8000 || SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8000;
        }
        SDL_free(ids);
        if (!held)
            g_input_held = false;
    }
    // The left stick moves the focus as the d-pad does (ImGui itself only scrolls with it): a push one step, held it
    // repeats after 0.35 s every 0.12 s. Not while a key is awaited or a list is open (its own items take the d-pad).
    g_scrolling = false;   // (set again below while the sticks are held or a drag goes on)
    // the Sticks and gyro page shows the sticks themselves: there they neither move the focus nor scroll (the d-pad does)
    bool sticks_page = U.screen == SCR_PLAY && g_tab == TAB_CONTROLS && g_ctl_tab == CTL_STICKS;
    if (!sticks_page) {
        static ImGuiDir held = ImGuiDir_None;
        static double next_at;
        auto mag = [&](ImGuiKey k) { return io.KeysData[k - ImGuiKey_NamedKey_BEGIN].AnalogValue; };
        float lx = mag(ImGuiKey_GamepadLStickRight) - mag(ImGuiKey_GamepadLStickLeft);
        float ly = mag(ImGuiKey_GamepadLStickDown) - mag(ImGuiKey_GamepadLStickUp);
        ImGuiDir dir = ImGuiDir_None;
        if (SDL_fabsf(lx) > SDL_fabsf(ly))
            dir = lx > 0.5f ? ImGuiDir_Right : lx < -0.5f ? ImGuiDir_Left : ImGuiDir_None;
        else
            dir = ly > 0.5f ? ImGuiDir_Down : ly < -0.5f ? ImGuiDir_Up : ImGuiDir_None;
        double now = ImGui::GetTime();
        bool go = dir != ImGuiDir_None && !g_cap.on && !g_input_held && ImGui::GetCurrentContext()->NavWindow &&
                  (dir != held || now >= next_at);
        if (go) {
            ImGui::NavMoveRequestSubmit(dir, dir, ImGuiNavMoveFlags_None, ImGuiScrollFlags_KeepVisibleEdgeX | ImGuiScrollFlags_KeepVisibleEdgeY);
            ImGui::SetNavCursorVisible(true);
            next_at = now + (dir != held ? 0.35 : 0.12);
        }
        held = dir;
        if (dir != ImGuiDir_None)
            g_scrolling = true;   // frames while it is held, for the repeat
    }
    // while a key or button is awaited (and until it is let go) the keyboard and the gamepad do not move the focus
    const ImGuiConfigFlags nav = ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    if (g_cap.on || g_input_held)
        io.ConfigFlags &= ~nav;
    else
        io.ConfigFlags |= nav;
    background(ImGui::GetBackgroundDrawList(), io.DisplaySize);
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    static bool focused_once;   // the window focused from the start: without it the gamepad and the keys move nothing
    if (!focused_once) {
        ImGui::SetNextWindowFocus();
        focused_once = true;
    }
    ImGui::Begin("launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    DL = ImGui::GetWindowDrawList();
    O = ImVec2(0, 0);
    bool lang = U.screen == SCR_DUMP || U.screen == SCR_BROKEN;

    // the body scrolls when it does not fit (long texts, a small screen); the header and the buttons stay
    ImGui::SetCursorScreenPos(P(0, 72));
    // NavFlattened: the gamepad's and the keys' focus moves between the body's items and the tabs and buttons around it
    // (without it the body was one item to the navigation, and nothing in it could be reached)
    ImGui::BeginChild("body", ImVec2(800 * S, 328 * S), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoBackground);
    DL = ImGui::GetWindowDrawList();
    DL->ChannelsSplit(2);   // 0: the cards, drawn after what is on them once their height is known
    DL->ChannelsSetCurrent(1);
    O = ImVec2(ImGui::GetWindowPos().x, ImGui::GetWindowPos().y - ImGui::GetScrollY() - 72 * S);
    switch (U.screen) {
    case SCR_DUMP: screen_dump(); break;
    case SCR_BUILD: screen_build(); break;
    case SCR_HW: screen_hw(); break;
    case SCR_PLAY: screen_play(); break;
    case SCR_UPDATE: screen_update(); break;
    case SCR_BROKEN: screen_broken(); break;
    }
    // A finger (or the mouse) dragged over the body scrolls it: the Steam Deck's screen in Gaming Mode reaches SDL as a
    // mouse, not as touches, so any drag that began on the body counts (not on its scrollbar, and not with a list open);
    // the drag's release clicks nothing (hit). The right stick scrolls it too; the wheel and the focus by themselves.
    {
        ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize(), c0 = io.MouseClickedPos[ImGuiMouseButton_Left];
        bool from_body = c0.x >= wp.x && c0.x < wp.x + ws.x - 14 * S && c0.y >= wp.y && c0.y < wp.y + ws.y;
        if (from_body && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 10 * S) &&
            !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
            ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
            g_drag_scrolled = true;
        }
        float rs = sticks_page ? 0 : io.KeysData[ImGuiKey_GamepadRStickDown - ImGuiKey_NamedKey_BEGIN].AnalogValue - io.KeysData[ImGuiKey_GamepadRStickUp - ImGuiKey_NamedKey_BEGIN].AnalogValue;
        g_scrolling = g_scrolling || SDL_fabsf(rs) > 0.2f || ImGui::IsMouseDragging(ImGuiMouseButton_Left);
        if (SDL_fabsf(rs) > 0.2f && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
            ImGui::SetScrollY(ImGui::GetScrollY() + rs * 700 * S * io.DeltaTime);
    }
    DL->ChannelsMerge();
    ImGui::EndChild();

    DL = ImGui::GetWindowDrawList();
    O = ImVec2(0, 0);
    switch (U.screen) {
    case SCR_DUMP: screen_dump_footer(); break;
    case SCR_BUILD: screen_build_footer(); break;
    case SCR_PLAY: screen_play_footer(); break;
    case SCR_BROKEN: version_label(); break;
    case SCR_UPDATE: screen_update_footer(); break;
    default: break;
    }
    if (U.screen == SCR_PLAY)
        play_tabs();
    else
        header(lang);   // last: the language list lies over the body
    ImGui::End();
    folder_picker_frame();
}

// Development: a screen with sample data, for screenshots (ULTRS_LAUNCHER_DEMO=<name>, main.cpp): dump, dump_bad,
// dump_ok, build, hw, failed, done, play.
const char *g_demo_open;

void ui_demo(const char *name) {
    std::string n = name;
    auto events = [](std::initializer_list<const char *> lines) {
        for (const char *l : lines) {
            Json e;
            if (json_parse(l, e))
                on_event(e);
        }
    };
    if (U.check) {
        setup_free(U.check);
        U.check = nullptr;
    }
    U.screen = SCR_DUMP;
    U.dump_st = D_NONE;
    if (U.dump.empty() && n != "update" && n != "run")   // sample screens only: never saved over a real choice
        U.dump = "D:\\Games\\Original";
    if (n == "update")
        return;   // poll_update starts it once GitHub answers
    if (n == "update_ask") {
        U.upd.version = "0.3.0", U.upd.page = ULTRS_REPO "/releases";
        U.version = U.version.empty() ? "0.2.0" : U.version;
        U.has_upd = true, U.upd_st = 0, U.screen = SCR_UPDATE;
        return;
    }
    if (n == "dump_bad") {
        U.dump_st = D_BAD, U.dump_code = "dump_version", U.checking = "D:\\Games\\Original";
    } else if (n == "dump_ok") {
        U.dump_st = D_OK;
    } else if (n == "build" || n == "hw" || n == "failed" || n == "done") {
        if (const char *dp = SDL_getenv("ULTRS_LAUNCHER_DEMO_PLATFORM"))   // windows, linux or both (the Deck's row)
            U.platform = dp;
        start_build_state();
        U.screen = SCR_BUILD;
        events({R"({"ev":"step","id":"tools","state":"done","secs":3})",
                R"({"ev":"hw","cases":[{"code":"no_shader_object","sev":"stop","args":{"gpu":"AMD Radeon RX 570","layer":false}},{"code":"ram_low","sev":"warn","args":{"gb":8}}]})",
                R"({"ev":"step","id":"check","state":"done","secs":2})",
                R"({"ev":"step","id":"romfs","state":"done","secs":220})",
                R"({"ev":"step","id":"lift","state":"run"})",
                R"({"ev":"progress","label":"lifting","done":40,"total":100,"eta":250})"});
        if (n == "hw")
            U.screen = SCR_HW;
        if (n == "failed")
            events({R"({"ev":"error","id":"lift","code":"failed","args":{},"message":"x"})"});
        if (n == "done")
            events({R"({"ev":"step","id":"lift","state":"done","secs":80})", R"({"ev":"step","id":"compile","state":"done","secs":940})",
                    R"({"ev":"step","id":"shaders","state":"skip","secs":1})", R"({"ev":"step","id":"graphics","state":"skip","secs":1})",
                    R"({"ev":"step","id":"textures","state":"done","secs":600})",
                    R"({"ev":"done","secs":1900})"});
    } else if (n == "run") {   // a real build at once, as if Prepare was pressed
        U.dump_st = D_OK;
        start_build();
    } else if (n == "play" || n == "play_upd" || n == "play_lang" || n == "play_debug" || n == "play_full" ||
               n == "play_pad" || n == "play_keys" || n == "play_sticks" || n == "play_mods") {
        U.screen = SCR_PLAY;
        g_tab = n == "play_lang" ? 1 : n == "play_pad" || n == "play_keys" || n == "play_sticks" ? TAB_CONTROLS
              : n == "play_debug" || n == "play_full" ? TAB_DEBUG : n == "play_mods" ? TAB_MODS : 0;
        if (n == "play_mods" && SDL_getenv("ULTRS_LAUNCHER_DEMO_OPEN"))   // the cheats of this mod shown
            M.open.push_back(SDL_getenv("ULTRS_LAUNCHER_DEMO_OPEN"));
        g_ctl_tab = n == "play_keys" ? CTL_KEYS : n == "play_sticks" ? CTL_STICKS : CTL_PAD;
        g_demo_sticks = n == "play_sticks";
        if (const char *w = SDL_getenv("ULTRS_LAUNCHER_DEMO_WAIT"))   // an action waiting for its key or button
            g_cap.on = true, g_cap.pad = g_ctl_tab == CTL_PAD, g_cap.id = w, g_cap.t0 = SDL_GetTicks();
        g_full_q = n == "play_full";
        if (n == "play_lang") g_demo_open = SDL_getenv("ULTRS_LAUNCHER_DEMO_OPEN");
        if (n == "play_upd" || n == "play_lang") {
            U.upd.version = "0.3.0", U.upd.page = ULTRS_REPO "/releases";
            U.has_upd = true;
        }
    }
}

void ui_drop(const char *path) {
    if (U.screen == SCR_DUMP || U.screen == SCR_PLAY) {
        U.screen = SCR_DUMP;
        check_dump(path);
    }
}

bool ui_close_request() {
    if (U.copy && !U.stopping) {   // the copy resumes too: the same question as for the build
        U.screen = SCR_BUILD;
        U.cancel_q = U.close_after_cancel = true;
        return false;
    }
    if (U.apply)
        return false;   // files of the package are being replaced: a few seconds
    if (!U.build || U.done || U.failed || U.stopping)
        return true;
    U.screen = SCR_BUILD;
    U.cancel_q = true;
    U.close_after_cancel = true;
    return false;
}

void ui_quit() {
    if (U.check)
        setup_free(U.check);
    if (U.build)
        setup_free(U.build);   // a build stopped here resumes next time
    if (U.copy)
        setup_free(U.copy);
}
