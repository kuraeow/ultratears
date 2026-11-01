// UltraTears Launcher: the window (SDL3 + Dear ImGui on SDL_Renderer), its scale and the frame loop. The screens are
// in ui.cpp. Design size 800x480, scaled by the display's scale; on a Steam Deck x1.6 (1280x768 of its 1280x800).
#include "launcher.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include <csignal>
#include <cstdio>

static const char *TITLE = "UltraTears";
static const float W = 800, H = 480;

static float window_scale(SDL_Window *w) {
    const char *deck = SDL_getenv("SteamDeck");
    if (deck && SDL_strcmp(deck, "1") == 0)
        return 1.6f;
    const char *force = SDL_getenv("ULTRS_LAUNCHER_SCALE");   // to try other scales
    if (force && *force)
        return (float)SDL_atof(force);
    float s = w ? SDL_GetDisplayContentScale(SDL_GetDisplayForWindow(w)) : SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    return s > 0 ? s : 1;
}

// The launcher's log, ultratears-launcher.log beside its settings (own_dir(): beside the AppImage file on Linux), the
// last run's kept as ultratears-launcher-previous.log: SDL_Log lines, with the time since the start. Before
// 2026-11-01 the launcher logged nothing, and a start that failed (Steam Deck, Game Mode) left no trace.
static FILE *g_log;
static void log_out(void *, int, SDL_LogPriority, const char *msg) {
    double t = SDL_GetTicks() / 1000.0;
    if (g_log) {
        fprintf(g_log, "[%8.3f] %s\n", t, msg);
        fflush(g_log);
    }
    fprintf(stderr, "%s\n", msg);
}
static std::string log_open() {
    std::string path = path_join(own_dir(), "ultratears-launcher.log");
    std::string prev = path_join(own_dir(), "ultratears-launcher-previous.log");
    SDL_RemovePath(prev.c_str());
    SDL_RenamePath(path.c_str(), prev.c_str());
    g_log = fopen(path.c_str(), "w");
    SDL_SetLogOutputFunction(log_out, nullptr);
    return path;
}
static int start_failed(const std::string &log, const char *what) {
    std::string err = std::string(what) + ": " + SDL_GetError();
    SDL_Log("cannot start: %s", err.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, TITLE, Tf("start_failed", {{"err", err}, {"log", log}}).c_str(), nullptr);
    return 1;
}

int main(int argc, char **argv) {
#ifndef _WIN32
    // Started with SIGCHLD ignored (a parent may leave it so; Steam's start was suspected, tools/appimage_runtime), the
    // launcher's children are reaped by the kernel and waitpid cannot tell how the builder ended. Back to the default.
    signal(SIGCHLD, SIG_DFL);
#endif
    std::string log = log_open();
    g_lang = system_lang();   // the settings' language comes with ui_init; messages before it in the system's
    SDL_Log("UltraTears Launcher, %s", package_version_line(exe_dir()).c_str());
    SDL_Log("launcher %s, settings in %s", argc > 0 ? argv[0] : "?", own_dir().c_str());
    for (const char *v : {"APPIMAGE", "SteamDeck", "SteamGameId", "XDG_CURRENT_DESKTOP", "WAYLAND_DISPLAY", "DISPLAY"}) {
        const char *s = SDL_getenv(v);
        if (s && *s)
            SDL_Log("  %s=%s", v, s);
    }
    // --play (Steam's launch option, through the AppImage's AppRun): the graphics prepared, then the game, in its place
    bool play = argc > 1 && !SDL_strcmp(argv[1], "--play");
    const char *dev_shot = SDL_getenv("ULTRS_LAUNCHER_SHOT");   // development screenshots run beside a real launcher
    if (!(dev_shot && *dev_shot) && !single_instance(TITLE))
        return 0;
    SDL_SetHint(SDL_HINT_APP_NAME, "UltraTears Launcher");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
        return start_failed(log, "SDL_Init");
    SDL_Log("video driver %s", SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?");
    float scale = window_scale(nullptr);
    SDL_Window *win = SDL_CreateWindow(TITLE, (int)(W * scale), (int)(H * scale), SDL_WINDOW_HIDDEN);
    if (!win)
        return start_failed(log, "SDL_CreateWindow");
    SDL_Renderer *ren = SDL_CreateRenderer(win, nullptr);
    if (!ren)
        return start_failed(log, "SDL_CreateRenderer");
    SDL_Log("window %dx%d at scale %.2f, renderer %s", (int)(W * scale), (int)(H * scale), scale, SDL_GetRendererName(ren));
    SDL_SetRenderVSync(ren, 1);
    window_dark_title(win);
    window_icon(win);
    float real = window_scale(win);   // the display the window opened on may have another scale
    if (real != scale) {
        scale = real;
        SDL_SetWindowSize(win, (int)(W * scale), (int)(H * scale));
    }
    SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;   // nothing of ImGui's own on disk
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;   // Steam Deck: the pad
    ImGui_ImplSDL3_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer3_Init(ren);
    media_load(ren, scale);
    ui_init(win);
    // development: ULTRS_LAUNCHER_DEMO=<screen> shows sample data (ui_demo); ULTRS_LAUNCHER_SHOT=<file.bmp> saves the
    // window after a few frames (ULTRS_LAUNCHER_SHOT_SECS: after that many seconds) and quits
    const char *demo = SDL_getenv("ULTRS_LAUNCHER_DEMO"), *shot = SDL_getenv("ULTRS_LAUNCHER_SHOT");
    const char *shot_secs = SDL_getenv("ULTRS_LAUNCHER_SHOT_SECS");
    Uint64 shot_at = shot_secs ? SDL_GetTicks() + (Uint64)(SDL_atof(shot_secs) * 1000) : 0;
    if (demo && *demo)
        ui_demo(demo);
    if (play)
        ui_play_mode();
    int frame = 0;
    bool shown = false;

    bool running = true;
    while (running) {
        SDL_Event e;
        bool got = SDL_WaitEventTimeout(&e, 50);   // idle: a few frames a second (the build's progress), no busy loop
        while (got) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(win))) {
                if (ui_close_request())
                    running = false;
            } else if (e.type == SDL_EVENT_DROP_FILE && e.drop.data) {
                ui_drop(e.drop.data);
                SDL_RaiseWindow(win);
            } else if (e.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) {
                float s = window_scale(win);
                if (s != scale) {
                    scale = s;
                    SDL_SetWindowSize(win, (int)(W * scale), (int)(H * scale));
                    ImGui_ImplSDLRenderer3_DestroyFontsTexture();
                    media_load(ren, scale);
                    ImGui_ImplSDLRenderer3_CreateFontsTexture();
                }
            }
            got = SDL_PollEvent(&e);
        }
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ui_frame(scale);   // also minimized: the build's events are read there
        if (ui_quit_wanted())
            running = false;
        if (!shown && ui_window_wanted()) {
            SDL_ShowWindow(win);
            shown = true;
        }
        if (SDL_GetWindowFlags(win) & SDL_WINDOW_MINIMIZED) {
            ImGui::EndFrame();
            continue;
        }
        ImGui::Render();
        SDL_SetRenderDrawColor(ren, 12, 12, 12, 255);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ren);
        if (shot && *shot && ++frame >= 8 && SDL_GetTicks() >= shot_at) {
            SDL_Surface *s = SDL_RenderReadPixels(ren, nullptr);
            if (s) {
                SDL_SaveBMP(s, shot);
                SDL_DestroySurface(s);
            }
            running = false;
        }
        SDL_RenderPresent(ren);
    }
    ui_quit();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    SDL_Log("closed");
    if (play && ui_quit_wanted() && !ui_play_finish())
        return 1;
    return 0;
}
