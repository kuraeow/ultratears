// The settings overlay in the game (Nintendo/launcher-design/ingame.png): F1 (unless bound) or the controller's Back +
// Start (src/host/window_sdl.c) open it over the paused game, whether the game was started by the launcher or not. Its
// pages are the launcher's (src/ui/settings.inc: General, Controls, Mods, Debug's switches) with the same look
// (src/ui/look.inc); what can apply at once does (the variable it stands for set again, the part that reads it told), the
// rest asks for a restart when the overlay closes, and the game starts itself again (src/host/boot.c totk_restart).
//
// Threads: the overlay's ImGui frames are made on the main thread (SDL's events are there), copied as a snapshot, and
// drawn by the present thread (src/vk/present.c) over the last game frame with ImGui's Vulkan backend (dynamic
// rendering into the swapchain image). The game thread waits in its present while the overlay is open (ovl_game_gate:
// a paused game), its sound quieter. Closed, the overlay costs one atomic load per present and per window loop.
//
// Input: driven by the game's own bindings (window_nav_buttons): A selects, B goes back, L / R change the tab, the
// d-pad (and the movement keys) moves; the hints at the bottom name the button or key bound to each, as the controller in
// use (or the keyboard, if it was used last) labels it. Enter, Esc and the arrows work besides; the mouse too.
#include "../launcher/launcher.h"
#include "imgui_internal.h"   // NavMoveRequestSubmit: the left stick moves the focus as the d-pad does
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"
#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#endif
extern "C" {
#include "../vk/vk.h"
}

#include "../ui/look.inc"
#include "../ui/settings.inc"

extern "C" {
// src/host/window_sdl.c
uint64_t window_nav_buttons(float sticks[4]);
int window_last_keyboard(void);
void window_overlay_shown(int open);
void window_apply_mode(void);
void window_reload_input(void);
float window_refresh_hz(void);
SDL_Window *window_handle(void);
// src/host/boot.c, frame_rate.c, the parts that read a setting again
void totk_cfg_env(const char *key, const char *value);
void totk_restart(void);
void totk_fr_reload(void);
void vk_fsr_sharp_reload(void);
void vkb_tsr_sharp_reload(void);
void vkb_aa_reload(void);
int vkb_tsr_configured(void);
void vk_overlay_reload(void);
void totk_mouse_reload(void);
void runtime_motion_reload(void);
void runtime_audio_duck(float v);
uint64_t runtime_game_language(void);
uint64_t runtime_now_ns(void);
// src/vk/present.c
VkFormat vkp_format(void);
uint32_t vkp_image_count(void);
void vkp_resized(void);
void vkp_wake(void);
void vkp_shot_request(const char *path);
}

const char *g_demo_open;   // (settings.inc: the launcher's screenshots)

// ---------------------------------------------------------------- state
enum { TAB_GENERAL, TAB_CTL, TAB_MODS, TAB_DBG, OVL_TABS };
enum { MODAL_NONE, MODAL_RESTART, MODAL_QUIT };
static std::atomic<int> g_open{0};           // shown: the present thread draws it, the game waits
static std::atomic<uint64_t> g_seq{0};       // the snapshot's number: a new one is drawn over the last game frame
static SDL_Mutex *g_draw_mu;                 // the snapshot, and the backend's font texture while it is made again
static SDL_Mutex *g_gate_mu;
static SDL_Condition *g_gate_cv;
static ImDrawData g_snap;
static ImVector<ImDrawList *> g_snap_lists;
static uint64_t g_snap_seq;
static bool g_inited, g_init_failed;
static float g_scale_built;                  // the fonts' scale
static int g_otab = TAB_GENERAL, g_modal = MODAL_NONE;
static bool g_wide;                          // the description beside the pages (else in the bottom bar)
static float footer_h() { return g_wide ? 60 : 96; }   // narrow: the description's two lines above the hints
static float g_H;                             // the screen's height in design pixels
static float g_ox;                           // where the pages' column starts (screen pixels)
static uint64_t g_last_frame_ns;
static std::vector<std::pair<std::string, std::string>> g_cfg_start;   // the settings this run started with
static const char *g_hot_shown;              // the row described (kept while a list is open)
static bool g_focus_next;                    // just opened: the overlay's window takes the focus

static std::string ui_game_dir() {   // the game folder: the working directory (src/host/boot.c enters it)
    char *d = SDL_GetCurrentDirectory();
    std::string s = d ? d : ".";
    SDL_free(d);
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\'))
        s.pop_back();
    return s;
}

// What needs a new start of the game to apply: the keys, and the rows that show the mark (settings.inc row_label)
static const char *const RESTART_KEYS[] = {"resolution", "scale", "upscale", "shadow_map", "lang", "debug", "snapshots",
                                           "gpu_marks", "mods_dir", "mods_order", "mods_off", "cheats_on"};
static bool ui_row_restart(const char *row) {
    static const char *const ROWS[] = {"row_res", "row_scale", "row_debug", "row_snapshots", "row_gpu_marks"};
    for (const char *r : ROWS)
        if (!SDL_strcmp(r, row))
            return true;
    return false;
}
static std::string start_value(const char *key) {
    for (auto &kv : g_cfg_start)
        if (kv.first == key)
            return kv.second;
    return "";
}
static bool is30(const std::string &fps) { return fps == "30"; }
static bool is_fsr3(const std::string &aa) { return aa == "fsr3" || aa == "fsr"; }
static bool restart_needed() {
    for (const char *k : RESTART_KEYS)
        if (cfg_get(k) != start_value(k))
            return true;
    return is30(cfg_get("fps")) != is30(start_value("fps")) ||   // the 30 fps code and the others: chosen at the start
           is_fsr3(cfg_get("antialias")) != (vkb_tsr_configured() != 0);   // FSR 3.1 on or off as running (FXAA, SMAA: at once)
}

// A key of ultratears.cfg was written (settings.inc cfg_set): the game follows it where it can at once.
static void apply_live(const std::string &k, const std::string &v);
static void ui_cfg_changed(const char *key) { apply_live(key, cfg_get(key)); }
static void apply_live(const std::string &k, const std::string &v) {
    totk_cfg_env(k.c_str(), v.c_str());
    if (k == "window")
        window_apply_mode();
    else if (k == "vsync")
        vkp_resized();   // the swapchain made again reads it
    else if (k == "fps") {
        if (is30(v) == is30(start_value("fps")))
            totk_fr_reload();
    } else if (k == "fps_overlay")
        vk_overlay_reload();
    else if (k == "sharpness")
        vk_fsr_sharp_reload(), vkb_tsr_sharp_reload();
    else if (k == "antialias") {
        if (is_fsr3(v) == (vkb_tsr_configured() != 0))
            vkb_aa_reload();
    }
    else if (!k.compare(0, 4, "pad_") || !k.compare(0, 4, "key_") || !k.compare(0, 15, "stick_deadzone_"))
        window_reload_input();
    else if (k == "gyro_deadzone")
        runtime_motion_reload();
    else if (!k.compare(0, 6, "mouse_"))
        totk_mouse_reload();
}

// ---------------------------------------------------------------- the pause
extern "C" void ovl_game_gate(void) {   // the game thread, at its present: waits while the overlay is open
    if (!g_open.load(std::memory_order_acquire) || !g_gate_mu)
        return;
    SDL_LockMutex(g_gate_mu);
    while (g_open.load(std::memory_order_acquire))
        SDL_WaitCondition(g_gate_cv, g_gate_mu);
    SDL_UnlockMutex(g_gate_mu);
}

// ---------------------------------------------------------------- Vulkan
static PFN_vkGetInstanceProcAddr g_gipa;
static PFN_vkVoidFunction load_fn(const char *name, void *) {
    PFN_vkVoidFunction f = vk.vkGetDeviceProcAddr(vkc.device, name);
    return f ? f : g_gipa(vkc.instance, name);
}
static void vk_result(VkResult r) {
    if (r != VK_SUCCESS)
        SDL_Log("[overlay] Vulkan error %d", (int)r);
}

// ---- the size of the overlay
// The screen's diagonal in inches from its EDID (bytes 21 and 22: the picture's width and height in cm), 0 when not
// known. Windows: the monitor the window is on (its device's EDID in the registry, read only). Linux: the largest
// connected display (/sys/class/drm).
#ifdef _WIN32
static float screen_inches(SDL_Window *w) {
    HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof mi;
    if (!hwnd || !GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), (MONITORINFO *)&mi))
        return 0;
    DISPLAY_DEVICEW dd = {};
    dd.cb = sizeof dd;
    for (DWORD i = 0; EnumDisplayDevicesW(mi.szDevice, i, &dd, EDD_GET_DEVICE_INTERFACE_NAME); i++, dd.cb = sizeof dd) {
        if (!(dd.StateFlags & DISPLAY_DEVICE_ACTIVE))
            continue;
        // \\?\DISPLAY#GSM5B09#5&2b4ba9a2&0&UID4352#{guid} -> SYSTEM\CurrentControlSet\Enum\DISPLAY\GSM5B09\5&...\Device Parameters
        std::wstring id = dd.DeviceID;
        if (id.compare(0, 4, L"\\\\?\\") == 0)
            id.erase(0, 4);
        size_t g = id.find(L"#{");
        if (g != std::wstring::npos)
            id.erase(g);
        for (wchar_t &c : id)
            if (c == L'#')
                c = L'\\';
        std::wstring key = L"SYSTEM\\CurrentControlSet\\Enum\\" + id + L"\\Device Parameters";
        BYTE edid[256];
        DWORD n = sizeof edid;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"EDID", RRF_RT_REG_BINARY, nullptr, edid, &n) == ERROR_SUCCESS &&
            n >= 23 && edid[21] && edid[22])
            return SDL_sqrtf((float)(edid[21] * edid[21] + edid[22] * edid[22])) / 2.54f;
    }
    return 0;
}
#else
static float screen_inches(SDL_Window *) {
    float best = 0;
    DIR *d = opendir("/sys/class/drm");
    for (struct dirent *e; d && (e = readdir(d));) {
        if (SDL_strncmp(e->d_name, "card", 4) || !SDL_strchr(e->d_name, '-'))
            continue;   // card0-eDP-1, card1-HDMI-A-1: the connectors
        std::string base = std::string("/sys/class/drm/") + e->d_name;
        size_t n = 0;
        char *st = (char *)SDL_LoadFile((base + "/status").c_str(), &n);
        bool on = st && !SDL_strncmp(st, "connected", 9);
        SDL_free(st);
        unsigned char *edid = on ? (unsigned char *)SDL_LoadFile((base + "/edid").c_str(), &n) : nullptr;
        if (edid && n >= 23 && edid[21] && edid[22])
            best = SDL_max(best, SDL_sqrtf((float)(edid[21] * edid[21] + edid[22] * edid[22])) / 2.54f);
        SDL_free(edid);
    }
    if (d)
        closedir(d);
    return best;
}
#endif

/* A small screen (the Steam Deck, and any under 14 inches: handhelds, small laptops): the overlay as large as the window
   allows, as the launcher on the Deck. Otherwise (a PC's monitor) the same share of the monitor's height on any monitor:
   1.76 design pixels a point on a 1440-point-high one (the user liked that size, 2026-11-15), less in a window too small
   for it. Read once a window: the monitor the window is on may change. */
static bool small_screen(SDL_Window *w) {
    static float inches = -1;
    static SDL_DisplayID shown_for;
    SDL_DisplayID now = SDL_GetDisplayForWindow(w);
    if (inches < 0 || now != shown_for) {
        inches = screen_inches(w);
        shown_for = now;
        bool deck = on_deck();
        SDL_Log("[overlay] screen %.1f in%s: %s", inches, deck ? " (Steam Deck)" : "",
                deck || (inches > 0 && inches < 14) ? "the large overlay" : "the monitor's share");
    }
    return on_deck() || (inches > 0 && inches < 14);
}
// The design's scale and layout for the window (points); the description beside the pages when 1090 design pixels fit
// across, else in the bottom bar.
static float layout_scale(ImVec2 d, bool *wide) {
    SDL_Window *w = window_handle();
    float s;
    if (small_screen(w)) {   // as large as fits (fonts of 18 px and more with the description beside)
        float narrow = SDL_min(d.y / 480, d.x / 800), wd = SDL_min(d.y / 480, d.x / 1090);
        *wide = wd >= 1.2f;
        return SDL_max(0.5f, *wide ? wd : narrow);
    }
    const SDL_DisplayMode *m = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(w));
    float mon_h = m && m->h > 0 ? (float)m->h : d.y;
    s = SDL_min(mon_h / 820, SDL_min(d.y / 480, d.x / 800));
    s = SDL_max(0.5f, s);
    *wide = d.x / s >= 1090;
    return s;
}

// The fonts and images at the scale, again when it changes (a resize, another mode). The present thread holds g_draw_mu
// from recording the overlay until its submit (ovl_record .. ovl_record_done): with it taken here no command buffer with
// the old texture is waiting to be submitted, and the queue's wait lets those submitted finish.
static void fonts_build(float scale) {
    SDL_LockMutex(g_draw_mu);
    g_snap.Valid = false;
    vk_queue_lock();
    vk.vkQueueWaitIdle(vkc.queue);
    if (g_scale_built > 0)
        ImGui_ImplVulkan_DestroyFontsTexture();
    media_load(nullptr, scale);
    ImGui_ImplVulkan_CreateFontsTexture();   // its upload waits for the queue (held: the replayer's submits wait)
    vk_queue_unlock();
    g_scale_built = scale;
    SDL_UnlockMutex(g_draw_mu);
}

static bool init() {
    if (g_inited || g_init_failed)
        return g_inited;
    SDL_Window *w = window_handle();
    VkFormat fmt = vkp_format();
    g_gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (!w || fmt == VK_FORMAT_UNDEFINED || !g_gipa || !vkc.device) {
        SDL_Log("[overlay] not yet: no window picture");
        return false;   // tried again at the next open
    }
    g_draw_mu = SDL_CreateMutex();
    g_gate_mu = SDL_CreateMutex();
    g_gate_cv = SDL_CreateCondition();
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;   // the keys come as the gamepad's (the game's bindings)
    ImGui_ImplSDL3_InitForVulkan(w);
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, nullptr, 0);   // its pads: ours (window_nav_buttons)
    if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, load_fn)) {
        SDL_Log("[overlay] Vulkan functions missing");
        g_init_failed = true;
        return false;
    }
    ImGui_ImplVulkan_InitInfo ii = {};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = vkc.instance, ii.PhysicalDevice = vkc.gpu, ii.Device = vkc.device;
    ii.QueueFamily = vkc.family, ii.Queue = vkc.queue;
    ii.DescriptorPoolSize = 8;
    ii.MinImageCount = 2;
    ii.ImageCount = 3;   // its vertex buffers in turn: the present thread has 2 frames in flight
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.UseDynamicRendering = true;
    static VkFormat color;
    color = fmt;
    ii.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ii.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineRenderingCreateInfo.pColorAttachmentFormats = &color;
    ii.CheckVkResultFn = vk_result;
    if (!ImGui_ImplVulkan_Init(&ii)) {
        SDL_Log("[overlay] ImGui's Vulkan backend failed");
        g_init_failed = true;
        return false;
    }
    ImGuiStyle &st = ImGui::GetStyle();   // as the launcher's (src/launcher/ui.cpp ui_init)
    st.WindowPadding = ImVec2(0, 0);
    st.WindowBorderSize = 0;
    st.PopupBorderSize = 0;
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
    st.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);
    st.PopupRounding = 6;
    st.WindowRounding = 6;
    g_ingame = true;
    g_inited = true;
    SDL_Log("[overlay] ready (%s)", ImGui::GetVersion());
    return true;
}

// ---------------------------------------------------------------- open, close
static void open() {
    if (!init())
        return;
    char code[9] = {};
    uint64_t lang = runtime_game_language();
    std::memcpy(code, &lang, 8);
    g_lang = lang_from_code(code);   // the game's language: Russian, else English (the launcher has no others)
    if (g_cfg_start.empty()) {   // the settings this run started with: what differs from them needs a restart
        for (const std::string &l : cfg_lines()) {
            size_t eq = l.find('=');
            if (eq != std::string::npos)
                g_cfg_start.emplace_back(l.substr(0, eq), l.substr(eq + 1));
        }
        g_cfg_start.emplace_back("", "");   // not empty again, whatever the file holds
    }
    {   // the scale for this window (and again whenever it changes while open: ovl_frame)
        int w = 0, h = 0;
        SDL_GetWindowSize(window_handle(), &w, &h);
        float scale = layout_scale(ImVec2((float)w, (float)h), &g_wide);
        if (SDL_fabsf(scale - g_scale_built) > 0.01f)
            fonts_build(scale);
    }
    g_cfg_at = 0;
    g_focus_next = true;
    g_modal = MODAL_NONE;
    g_cap.on = false;
    M.loaded = false, M.failed = false;   // the mods read again
    g_open.store(1, std::memory_order_release);
    window_overlay_shown(1);
    runtime_audio_duck(0.3f);
    g_last_frame_ns = 0;
    SDL_Log("[overlay] open");
}
static void close_now() {
    if (!g_open.load())
        return;
    g_open.store(0, std::memory_order_release);
    SDL_LockMutex(g_gate_mu);
    SDL_BroadcastCondition(g_gate_cv);
    SDL_UnlockMutex(g_gate_mu);
    g_modal = MODAL_NONE;
    g_cap.on = false;
    window_overlay_shown(0);
    runtime_audio_duck(1);
    vkp_wake();
    SDL_Log("[overlay] closed");
}
static void request_close() {   // Back: closes, or first asks to restart when a change needs one
    if (g_modal != MODAL_NONE)
        return;
    if (restart_needed())
        g_modal = MODAL_RESTART;
    else
        close_now();
}

extern "C" int ovl_is_open(void) { return g_open.load(std::memory_order_acquire); }
extern "C" int ovl_visible(void) { return g_open.load(std::memory_order_acquire); }
extern "C" uint64_t ovl_seq(void) { return g_seq.load(std::memory_order_acquire); }
extern "C" void ovl_toggle(void) {
    if (g_open.load())
        request_close();
    else
        open();
}

// Every SDL event first while the overlay is open: 1 when it took it. The controller's events go on to the window
// (Back + Start close it there, and the game is given nothing while it is open); F11 and Alt+Enter too.
extern "C" int ovl_event(const SDL_Event *e) {
    if (!g_open.load())
        return 0;
    if ((e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_KEY_UP) &&
        (e->key.scancode == SDL_SCANCODE_F11 || (e->key.scancode == SDL_SCANCODE_RETURN && (e->key.mod & SDL_KMOD_ALT))))
        return 0;
    if (ui_event(e))   // a key or button being bound (the Controls page)
        return 1;
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
        if (e->key.scancode == SDL_SCANCODE_F1 && !e->key.repeat)
            request_close();
        return 1;
    case SDL_EVENT_KEY_UP: case SDL_EVENT_TEXT_INPUT: case SDL_EVENT_TEXT_EDITING: return 1;
    case SDL_EVENT_MOUSE_MOTION: case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: case SDL_EVENT_MOUSE_WHEEL:
        ImGui_ImplSDL3_ProcessEvent(e);
        return 1;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE: case SDL_EVENT_WINDOW_FOCUS_GAINED: case SDL_EVENT_WINDOW_FOCUS_LOST:
        ImGui_ImplSDL3_ProcessEvent(e);
        return 0;
    default: return 0;
    }
}

/* TOTK_OVERLAY_DEMO=<seconds>[,<tab 0-3>[,<row>]] (development: screenshots without input): the overlay opens by itself
   that long after the start, on that tab, describing that row (row_scale, act_a...) while nothing has the focus. */
static const char *g_demo_row;
static uint64_t g_demo_cycle;   // tab "c": the tabs in turn, 4 s each, from this time
static void demo_tick() {
    static int done = -1;
    static uint64_t t0;
    if (g_demo_cycle && g_open.load()) {
        uint64_t ms = (runtime_now_ns() - g_demo_cycle) / 1000000;
        int t = (int)(ms / 4000);
        g_otab = t % OVL_TABS;
        g_demo_row = g_otab == TAB_GENERAL ? "row_scale" : nullptr;
        static int shot = -1;   // TOTK_OVERLAY_SHOTS=<folder>: each tab once into <folder>/tab<N>.ppm, 2 s after it shows
        const char *dir = SDL_getenv("TOTK_OVERLAY_SHOTS");
        if (dir && t < OVL_TABS && shot < t && ms % 4000 >= 2000) {
            shot = t;
            vkp_shot_request((std::string(dir) + "/tab" + std::to_string(t) + ".ppm").c_str());
        }
        // TOTK_OVERLAY_DEMO_WINDOW=<window mode>: after the tabs, the window switched to it, a shot 3 s later (the
        // overlay's scale follows the window)
        static int switched;
        const char *wm = SDL_getenv("TOTK_OVERLAY_DEMO_WINDOW");
        if (wm && t >= OVL_TABS && !switched) {
            switched = 1;
            totk_cfg_env("window", wm);
            window_apply_mode();
        }
        if (wm && dir && switched == 1 && t >= OVL_TABS && ms % 4000 >= 3000) {
            switched = 2;
            vkp_shot_request((std::string(dir) + "/after-window.ppm").c_str());
        }
        // TOTK_OVERLAY_DEMO_LIVE=<key=value,...>: after the tabs, applied as the overlay applies a change (ultratears.cfg
        // is not written), a shot 3 s later
        static int lived;
        const char *live = SDL_getenv("TOTK_OVERLAY_DEMO_LIVE");
        if (live && t >= OVL_TABS && !lived) {
            lived = 1;
            std::string all = live;
            for (size_t p = 0; p <= all.size();) {
                size_t e = all.find(',', p);
                std::string kv = all.substr(p, e == std::string::npos ? std::string::npos : e - p);
                size_t eq = kv.find('=');
                if (eq != std::string::npos) {
                    SDL_Log("[overlay] TOTK_OVERLAY_DEMO_LIVE: %s", kv.c_str());
                    apply_live(kv.substr(0, eq), kv.substr(eq + 1));
                }
                if (e == std::string::npos) break;
                p = e + 1;
            }
        }
        if (live && dir && lived == 1 && t >= OVL_TABS && ms % 4000 >= 3000) {
            lived = 2;
            vkp_shot_request((std::string(dir) + "/after-live.ppm").c_str());
        }
        if (live && lived == 2 && t > OVL_TABS) {   // then closed: the game runs on with the changes
            lived = 3;
            g_demo_cycle = 0;
            close_now();
        }
        // TOTK_OVERLAY_DEMO_RESTART=<marker file>: after the tabs, the restart once (the marker keeps the new start from
        // doing it again: it gets the same environment)
        const char *mark = SDL_getenv("TOTK_OVERLAY_DEMO_RESTART");
        if (mark && t >= OVL_TABS && !SDL_GetPathInfo(mark, nullptr)) {
            SDL_SaveFile(mark, "1", 1);
            SDL_Log("[overlay] TOTK_OVERLAY_DEMO_RESTART: restarting");
            totk_restart();
        }
    }
    if (done < 0) {
        done = !SDL_getenv("TOTK_OVERLAY_DEMO");
        t0 = runtime_now_ns();
    }
    if (done)
        return;
    const char *e = SDL_getenv("TOTK_OVERLAY_DEMO");
    double secs = SDL_atof(e);
    if (runtime_now_ns() - t0 < (uint64_t)(secs * 1e9))
        return;
    done = 1;
    const char *c = SDL_strchr(e, ',');
    if (c && c[1] == 'c')
        g_demo_cycle = runtime_now_ns();
    else if (c) {
        g_otab = SDL_clamp(SDL_atoi(c + 1), 0, OVL_TABS - 1);
        static std::string row;
        const char *r = SDL_strchr(c + 1, ',');
        if (r)
            row = r + 1, g_demo_row = row.c_str();
    }
    SDL_Log("[overlay] TOTK_OVERLAY_DEMO: opening");
    open();
}

extern "C" int ovl_wait_ms(void) {   // the window loop's wait: the next overlay frame, else 100 ms
    demo_tick();
    if (!g_open.load())
        return 100;
    float hz = SDL_clamp(window_refresh_hz(), 30.0f, 120.0f);
    if (hz < 30.5f)
        hz = 60;
    uint64_t period = (uint64_t)(1e9 / hz), now = runtime_now_ns();
    if (!g_last_frame_ns || now - g_last_frame_ns >= period)
        return 0;
    return (int)((period - (now - g_last_frame_ns)) / 1000000) + 1;
}

// ---------------------------------------------------------------- the hints: the game's bindings
// The name of what is bound to action id now, on the device used last: the controller's button as its layout labels it,
// else the first key
static std::string bound_name(const char *id, bool kb) {
    const Ctl *c = ctl_find(id);
    std::vector<std::string> l = c ? ctl_current(!kb, *c) : std::vector<std::string>();
    if (l.empty())
        return kb ? (!SDL_strcmp(id, "a") ? "Enter" : !SDL_strcmp(id, "b") ? "Esc" : "") : "";
    return kb ? key_label(l[0]) : pad_label(l[0]);
}
// A button's mark: a light circle with its letter (a face button: one letter), else a light rounded box with its name;
// returns its width (design pixels)
static bool mark_round(const std::string &name, bool kb) { return !kb && name.size() == 1 && SDL_isalpha((unsigned char)name[0]); }
static float button_mark(float x, float cy, const std::string &name, bool kb) {
    ImFont *f = g_media.control;
    float tw = text_w(f, name.c_str());
    bool round = mark_round(name, kb);
    float w = round ? 24 : SDL_max(24.0f, tw + 12);
    if (round)
        DL->AddCircleFilled(P(x + 12, cy), 12 * S, C_LIGHT, 32);
    else
        DL->AddRectFilled(P(x, cy - 12), P(x + w, cy + 12), C_LIGHT, 6 * S);
    text(f, x + (w - tw) / 2, cy - f->FontSize / S / 2, IM_COL32(17, 17, 17, 255), name.c_str());
    return w;
}
// A hint at the bottom right, from x_right leftwards: the mark and its words; clickable (the mouse); returns its left
static float hint(const char *id, float x_right, float cy, const char *action, const char *words, bool *clicked) {
    bool kb = window_last_keyboard();
    std::string name = bound_name(action, kb);
    float tw = text_w(g_media.regular, words), mw = name.empty() ? 0 : SDL_max(24.0f, text_w(g_media.control, name.c_str()) + 12);
    if (mark_round(name, kb))
        mw = 24;
    float x = x_right - tw - (mw ? mw + 8 : 0);
    bool hov, down;
    *clicked = hit(id, x - 4, cy - 15, x_right - x + 8, 30, &hov, &down);
    if (mw)
        button_mark(x, cy, name, kb);
    text(g_media.regular, x + (mw ? mw + 8 : 0), cy - g_media.regular->FontSize / S / 2, hov ? C_LIGHT_HOVER : C_TEXT, words);
    return x;
}

// ---------------------------------------------------------------- the frame
static void tabs_header() {
    static const char *const TABS[] = {"tab_main", "tab_controls", "tab_mods", "tab_debug"};
    bool popup = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    if (!capturing() && !popup && g_modal == MODAL_NONE) {   // L / R (as bound) go through the tabs
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false))
            g_otab = (g_otab + OVL_TABS - 1) % OVL_TABS, g_cap.on = false;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false))
            g_otab = (g_otab + 1) % OVL_TABS, g_cap.on = false;
    }
    bool kb = window_last_keyboard();
    std::string ln = bound_name("l", kb), rn = bound_name("r", kb);
    float x = 36;
    if (!ln.empty())
        x += button_mark(x, 36, ln, kb) + 10;
    for (int i = 0; i < OVL_TABS; i++) {
        const char *l = T(TABS[i]);
        float w = text_w(g_media.control, l) + 24;
        bool hov, down;
        if (hit(TABS[i], x, 20, w, 32, &hov, &down))
            g_otab = i, g_cap.on = false;
        if (i == g_otab || hov)
            DL->AddRectFilled(P(x, 20), P(x + w, 52), i == g_otab ? C_DARK : C_CARD_HOVER, 6 * S);
        text(g_media.control, x + 12, 36 - g_media.control->FontSize / S / 2, C_TEXT, l);
        x += w + 2;
    }
    if (!rn.empty())
        button_mark(x + 8, 36, rn, kb);
    // Quit the game: the header's right end (asked first)
    float right = g_wide ? 1090 - 36 : 764, bw = text_w(g_media.medium, T("ovl_quit")) + 34;
    if (button("ovl_quit", T("ovl_quit"), right - bw, 20, bw, 32, DARK))
        g_modal = MODAL_QUIT;
}

static void body() {
    switch (g_otab) {
    case TAB_GENERAL: {
        float top = 80;
        text(g_media.small, 52, top + 20 - g_media.small->FontSize / S / 2, C_SUB, T("sec_screen"));
        float cy = general_rows(top + 52);
        card(top, cy + 28);
        ImGui::SetCursorScreenPos(P(0, cy + 36));   // the body scrolls down to here
        ImGui::Dummy(ImVec2(1, 1));
        break;
    }
    case TAB_CTL: screen_controls(); break;
    case TAB_MODS: screen_mods(); break;
    case TAB_DBG: {
        float bottom = debug_switches(80);
        ImGui::SetCursorScreenPos(P(0, bottom + 16));
        ImGui::Dummy(ImVec2(1, 1));
        break;
    }
    }
}

// The description of the row under the mouse or with the focus (the tab's own when none): its name, what it does and
// when it applies. Wide: a card beside the pages; else two lines in the bottom bar (returns nothing then).
static float rich_height(const std::string &s, float w) {   // the height rich() would give, without drawing
    float cx = 0, lines = 1, space = text_w(g_media.small, " ");
    size_t p = 0;
    while (p < s.size()) {
        size_t e = s.find(' ', p);
        std::string word = s.substr(p, e == std::string::npos ? std::string::npos : e - p);
        float ww = text_w(g_media.small, word.c_str());
        if (cx > 0 && cx + space + ww > w)
            lines++, cx = ww;
        else
            cx += (cx > 0 ? space : 0) + ww;
        p = e == std::string::npos ? s.size() : e + 1;
    }
    return lines * 17;
}

static void describe(float x, float top, float w, float bottom, bool bar) {
    static const char *const TAB_DESC[] = {"tab_main.desc", "tab_controls.desc", "tab_mods.desc", "tab_debug.desc"};
    const char *row = g_hot_shown;
    std::string key = row ? std::string(row) + ".desc" : "";
    if (row && !SDL_strncmp(row, "act_", 4))
        key = "act.desc";
    if (!row || !has_text(key.c_str()))
        row = nullptr, key = TAB_DESC[g_otab];
    if (bar) {   // the bottom bar: the words only, two lines at most (cut with an ellipsis)
        std::string d = T(key.c_str()), out, line;
        int lines = 0;
        size_t p = 0;
        while (p < d.size() && lines < 2) {
            size_t e = d.find(' ', p);
            std::string word = d.substr(p, e == std::string::npos ? std::string::npos : e - p);
            std::string next = line.empty() ? word : line + " " + word;
            if (!line.empty() && text_w(g_media.small, next.c_str()) > w) {
                out += (out.empty() ? "" : " ") + line;
                lines++;
                line = word;
            } else
                line = next;
            p = e == std::string::npos ? d.size() : e + 1;
        }
        if (lines < 2 && !line.empty())
            out += (out.empty() ? "" : " ") + line, line.clear();
        if (p < d.size() || !line.empty()) {   // more than fits: the last word or two give way to the ellipsis
            while (!out.empty() && text_w(g_media.small, (out.substr(out.rfind(' ') + 1) + "…").c_str()) > 0 &&
                   rich_height(out + "…", w) > 2 * 17 + 1)
                out.erase(out.rfind(' ') == std::string::npos ? 0 : out.rfind(' '));
            out += "…";
        }
        rich(g_media.small, x, top, w, {{out.c_str(), C_SUB}}, 17);
        return;
    }
    DL->ChannelsSplit(2);
    DL->ChannelsSetCurrent(1);
    float y = top + 18;
    const char *title = row ? T(row) : T(g_otab == TAB_GENERAL ? "tab_main" : g_otab == TAB_CTL ? "tab_controls" : g_otab == TAB_MODS ? "tab_mods" : "tab_debug");
    y += rich(g_media.medium, x + 16, y, w - 32, {{title, C_TEXT}}) + 8;
    y += rich(g_media.small, x + 16, y, w - 32, {{T(key.c_str()), C_SUB}}, 18) + 12;
    if (row && !SDL_strncmp(row, "row_", 4) && SDL_strcmp(row, "row_layout")) {
        bool later = ui_row_restart(row);
        y += rich(g_media.small, x + 16, y, w - 32, {{T(later ? "ovl_applies_restart" : "ovl_applies_now"), later ? C_TEXT : C_OK}}, 18) + 12;
    }
    DL->ChannelsSetCurrent(0);
    DL->AddRectFilled(P(x, top), P(x + w, SDL_min(SDL_max(y + 4, top + 60), bottom)), IM_COL32(28, 28, 28, 235), 10 * S);
    DL->ChannelsMerge();
}

static void modal() {
    if (g_modal == MODAL_NONE)
        return;
    if (!ImGui::IsPopupOpen("##modal"))
        ImGui::OpenPopup("##modal");
    const float MW = 440;
    bool restart = g_modal == MODAL_RESTART;
    const char *title = T(restart ? "ovl_restart_title" : "ovl_quit_title"), *body = T(restart ? "ovl_restart_text" : "ovl_quit_text");
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x / 2, io.DisplaySize.y / 2), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(MW * S, 190 * S));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0, 0, 0, 0));
    if (ImGui::BeginPopupModal("##modal", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        ImDrawList *keep = DL;
        ImVec2 keep_o = O;
        DL = ImGui::GetWindowDrawList();
        O = ImGui::GetWindowPos();
        DL->AddRectFilled(P(0, 0), P(MW, 190), C_CARD, 12 * S);
        DL->AddRect(P(0, 0), P(MW, 190), IM_COL32(52, 52, 52, 255), 12 * S, 0, S);
        float y = 24;
        y += rich(g_media.title, 28, y, MW - 56, {{title, C_TEXT}}) + 8;
        rich(g_media.regular, 28, y, MW - 56, {{body, C_SUB}});
        const char *go = T(restart ? "ovl_restart_now" : "ovl_quit_go"), *no = T(restart ? "ovl_later" : "cancel");
        float w1 = text_w(g_media.medium, go) + 34, w2 = text_w(g_media.medium, no) + 34;
        bool yes = button("m_go", go, MW - 28 - w1, 190 - 28 - 40, w1, 40, PRIMARY);
        bool not_now = button("m_no", no, MW - 28 - w1 - 8 - w2, 190 - 28 - 40, w2, 40, DARK);
        bool back = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
        DL = keep, O = keep_o;
        if (yes) {
            ImGui::CloseCurrentPopup();
            if (restart)
                totk_restart();   // returns only when it failed: closed then, as with Later
            else {
                SDL_Event q = {};
                q.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&q);   // the window loop ends the game as its close button does
            }
            g_modal = MODAL_NONE;
            if (restart)
                close_now();
        } else if (not_now || back) {
            ImGui::CloseCurrentPopup();
            g_modal = MODAL_NONE;
            if (restart)
                close_now();   // Later: the changes stay in ultratears.cfg for the next start
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
}

// The game's bindings into ImGui's gamepad keys: A activates, B cancels, the d-pad moves, L / R the tabs; the sticks
static void nav_input() {
    ImGuiIO &io = ImGui::GetIO();
    float st[4];
    uint64_t b = window_nav_buttons(st);
    struct { uint64_t bit; ImGuiKey key; } map[] = {
        {1, ImGuiKey_GamepadFaceDown}, {2, ImGuiKey_GamepadFaceRight}, {64, ImGuiKey_GamepadL1}, {128, ImGuiKey_GamepadR1},
        {8192, ImGuiKey_GamepadDpadUp}, {32768, ImGuiKey_GamepadDpadDown}, {4096, ImGuiKey_GamepadDpadLeft},
        {16384, ImGuiKey_GamepadDpadRight}};
    for (auto &m : map)
        io.AddKeyEvent(m.key, (b & m.bit) != 0);
    auto axis = [&](ImGuiKey neg, ImGuiKey pos, float v) {
        float a = SDL_fabsf(v) < 0.2f ? 0 : v;
        io.AddKeyAnalogEvent(neg, a < 0, a < 0 ? -a : 0);
        io.AddKeyAnalogEvent(pos, a > 0, a > 0 ? a : 0);
    };
    axis(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, st[0]);
    axis(ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown, st[1]);
    axis(ImGuiKey_GamepadRStickLeft, ImGuiKey_GamepadRStickRight, st[2]);
    axis(ImGuiKey_GamepadRStickUp, ImGuiKey_GamepadRStickDown, st[3]);
}

extern "C" void ovl_frame(void) {
    if (!g_open.load() || !g_inited)
        return;
    if (ovl_wait_ms() > 0)
        return;
    g_last_frame_ns = runtime_now_ns();
    ImGuiIO &io = ImGui::GetIO();
    {   // the window changed size (fullscreen <-> window, a resize): the scale follows once it has held for 150 ms
        static float pending;
        static uint64_t since;
        int w = 0, h = 0;
        SDL_GetWindowSize(window_handle(), &w, &h);
        bool wide;
        float scale = layout_scale(ImVec2((float)w, (float)h), &wide);
        if (SDL_fabsf(scale - g_scale_built) <= 0.01f) {
            pending = 0, g_wide = wide;
        } else if (SDL_fabsf(scale - pending) > 0.01f) {
            pending = scale, since = g_last_frame_ns;
        } else if (g_last_frame_ns - since > 150000000ull) {
            fonts_build(scale);
            g_wide = wide, pending = 0;
        }
    }
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    S = g_scale_built;
    nav_input();
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;   // (the SDL backend's own pads are off: manual, none)
    bool popup_before = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    // while a key or button is awaited (and until it is let go) the controls do not move the focus (as the launcher)
    if (g_input_held) {
        bool held = SDL_GetMouseState(nullptr, nullptr) != 0;
        int n = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&n);   // the controllers themselves (not window_nav_buttons: it hands out
        for (int i = 0; ids && i < n && !held; i++) {   // a modifier's press once)
            SDL_Gamepad *g = SDL_GetGamepadFromID(ids[i]);
            for (int b = 0; g && b < SDL_GAMEPAD_BUTTON_COUNT && !held; b++)
                held = SDL_GetGamepadButton(g, (SDL_GamepadButton)b);
            if (g && !held)
                held = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 8000 || SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8000;
        }
        SDL_free(ids);
        n = 0;
        const bool *keys = SDL_GetKeyboardState(&n);
        for (int i = 0; i < n && !held; i++)
            held = keys[i];
        if (!held)
            g_input_held = false;
    }
    if (g_cap.on && SDL_GetTicks() - g_cap.t0 > 8000)
        g_cap.on = false;
    if (g_cap.on || g_input_held)
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    else
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    ImGui::NewFrame();

    g_H = io.DisplaySize.y / S;
    float block = g_wide ? 1090 : 800;   // the pages and the description beside them, centred
    g_ox = SDL_max(0.0f, (io.DisplaySize.x - block * S) / 2);
    // the paused game behind: dimmed, the launcher's pattern faintly over it
    ImDrawList *bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(12, 12, 12, 196));
    if (g_media.pattern.atlas) {
        float t = g_media.pattern.w * S;
        for (float y = 0; y < io.DisplaySize.y; y += t)
            for (float x = 0; x < io.DisplaySize.x; x += t)
                bg->AddImage(icon_tex(g_media.pattern), ImVec2(x, y), ImVec2(x + t, y + t), g_media.pattern.uv0,
                             g_media.pattern.uv1, IM_COL32(255, 255, 255, 110));
    }
    ImU32 line = IM_COL32(40, 40, 40, 255);
    bg->AddLine(ImVec2(0, 60 * S), ImVec2(io.DisplaySize.x, 60 * S), line, S);
    bg->AddLine(ImVec2(0, (g_H - footer_h()) * S), ImVec2(io.DisplaySize.x, (g_H - footer_h()) * S), line, S);

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    if (g_focus_next || !ImGui::GetCurrentContext()->NavWindow)
        ImGui::SetNextWindowFocus();
    g_focus_next = false;
    ImGui::Begin("overlay", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    DL = ImGui::GetWindowDrawList();
    O = ImVec2(g_ox, 0);
    g_row = nullptr;
    const char *hot_before = g_row_hot;
    g_row_hot = nullptr;

    // the pages: the body scrolls between the tabs and the bottom bar
    float body_h = g_H - footer_h() - 72;
    ImGui::SetCursorScreenPos(P(0, 72));
    ImGui::BeginChild("body", ImVec2(800 * S, body_h * S), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoBackground);
    DL = ImGui::GetWindowDrawList();
    DL->ChannelsSplit(2);   // 0: the cards, drawn after what is on them once their height is known
    DL->ChannelsSetCurrent(1);
    O = ImVec2(ImGui::GetWindowPos().x, ImGui::GetWindowPos().y - ImGui::GetScrollY() - 72 * S);
    body();
    {   // the right stick scrolls (not on the sticks page, which shows them), a drag with the mouse too
        bool sticks_page = g_otab == TAB_CTL && g_ctl_tab == CTL_STICKS;
        float rs = sticks_page ? 0 : io.KeysData[ImGuiKey_GamepadRStickDown - ImGuiKey_NamedKey_BEGIN].AnalogValue -
                                         io.KeysData[ImGuiKey_GamepadRStickUp - ImGuiKey_NamedKey_BEGIN].AnalogValue;
        if (SDL_fabsf(rs) > 0.2f && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
            ImGui::SetScrollY(ImGui::GetScrollY() + rs * 700 * S * io.DeltaTime);
        // the left stick moves the focus as the d-pad does (a push one step; held, again after 0.35 s every 0.12 s)
        static ImGuiDir held = ImGuiDir_None;
        static double next_at;
        auto mag = [&](ImGuiKey k) { return io.KeysData[k - ImGuiKey_NamedKey_BEGIN].AnalogValue; };
        float lx = mag(ImGuiKey_GamepadLStickRight) - mag(ImGuiKey_GamepadLStickLeft);
        float ly = mag(ImGuiKey_GamepadLStickDown) - mag(ImGuiKey_GamepadLStickUp);
        ImGuiDir dir = ImGuiDir_None;
        if (!sticks_page) {
            if (SDL_fabsf(lx) > SDL_fabsf(ly))
                dir = lx > 0.5f ? ImGuiDir_Right : lx < -0.5f ? ImGuiDir_Left : ImGuiDir_None;
            else
                dir = ly > 0.5f ? ImGuiDir_Down : ly < -0.5f ? ImGuiDir_Up : ImGuiDir_None;
        }
        double now = ImGui::GetTime();
        if (dir != ImGuiDir_None && !g_cap.on && !g_input_held && (dir != held || now >= next_at)) {
            ImGui::NavMoveRequestSubmit(dir, dir, ImGuiNavMoveFlags_None, ImGuiScrollFlags_KeepVisibleEdgeX | ImGuiScrollFlags_KeepVisibleEdgeY);
            ImGui::SetNavCursorVisible(true);
            next_at = now + (dir != held ? 0.35 : 0.12);
        }
        held = dir;
    }
    DL->ChannelsMerge();
    ImGui::EndChild();
    g_row = nullptr;   // the header's and the bottom bar's controls describe no row

    DL = ImGui::GetWindowDrawList();
    O = ImVec2(g_ox, 0);
    tabs_header();
    // the row described: the one under the mouse or with the focus; kept while a list is open
    if (g_row_hot || !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
        g_hot_shown = g_row_hot ? g_row_hot : g_demo_row;
    (void)hot_before;
    // the bottom bar: Select and Back as bound, right; the description left of them on a narrow screen
    float cy = g_H - 30, right = g_wide ? 1090 - 36 : 764;
    bool back_click, sel_click;
    float x = hint("h_back", right, cy, "b", T("ovl_back"), &back_click);
    x = hint("h_sel", x - 24, cy, "a", T("ovl_select"), &sel_click);
    if (g_wide)
        describe(800, 80, 1090 - 36 - 800, g_H - 72, false);
    else   // above the hints, the bar's width
        describe(36, g_H - footer_h() + 10, 764 - 36, g_H, true);
    bool modal_before = g_modal != MODAL_NONE;
    modal();
    if (!modal_before && g_modal == MODAL_NONE && !g_cap.on && !g_input_held &&
        (back_click || (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) && !popup_before)))
        request_close();
    ImGui::End();
    ImGui::Render();

    // the snapshot for the present thread
    ImDrawData *dd = ImGui::GetDrawData();
    SDL_LockMutex(g_draw_mu);
    for (ImDrawList *l : g_snap_lists)
        IM_DELETE(l);
    g_snap_lists.resize(0);
    for (ImDrawList *l : dd->CmdLists)
        g_snap_lists.push_back(l->CloneOutput());
    g_snap.Clear();
    g_snap.Valid = true;
    g_snap.CmdListsCount = g_snap_lists.Size;
    g_snap.CmdLists = g_snap_lists;
    g_snap.TotalVtxCount = dd->TotalVtxCount;
    g_snap.TotalIdxCount = dd->TotalIdxCount;
    g_snap.DisplayPos = dd->DisplayPos;
    g_snap.DisplaySize = dd->DisplaySize;
    g_snap.FramebufferScale = dd->FramebufferScale;
    g_snap_seq = g_seq.load() + 1;
    SDL_UnlockMutex(g_draw_mu);
    g_seq.store(g_snap_seq, std::memory_order_release);
    vkp_wake();
}

// The present thread: the overlay over the swapchain image (in TRANSFER_DST_OPTIMAL before and after); returns the
// snapshot's number drawn (the present thread shows the last game frame again for a newer one)
static bool g_rec_held;   // (present thread) g_draw_mu held from the overlay's recording until the submit
extern "C" void ovl_record_done(void) {   // present.c, after the frame's submit
    if (g_rec_held)
        g_rec_held = false, SDL_UnlockMutex(g_draw_mu);
}
extern "C" uint64_t ovl_record(VkCommandBuffer cb, VkImage img, VkImageView view, VkExtent2D ext) {
    if (!g_open.load(std::memory_order_acquire) || !g_inited || !view)
        return g_seq.load();
    SDL_LockMutex(g_draw_mu);
    uint64_t seq = g_snap_seq;
    if (!g_snap.Valid || !g_snap.CmdListsCount) {
        SDL_UnlockMutex(g_draw_mu);
        return seq;
    }
    g_rec_held = true;   // until the submit (ovl_record_done): the font texture stays as it is meanwhile
    auto barrier = [&](VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 s0, VkAccessFlags2 a0,
                       VkPipelineStageFlags2 s1, VkAccessFlags2 a1) {
        VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = s0, b.srcAccessMask = a0, b.dstStageMask = s1, b.dstAccessMask = a1;
        b.oldLayout = from, b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo d = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.imageMemoryBarrierCount = 1, d.pImageMemoryBarriers = &b;
        vk.vkCmdPipelineBarrier2(cb, &d);
    };
    barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, ext};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vk.vkCmdBeginRendering(cb, &ri);
    ImGui_ImplVulkan_RenderDrawData(&g_snap, cb);
    vk.vkCmdEndRendering(cb);
    barrier(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    return seq;
}
