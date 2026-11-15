// The settings pages the launcher's Play screen and the game's overlay share (included after look.inc): the keys of
// ultratears.cfg, the lists, the General page, Controls, Mods and the Debug switches. The including file gives
// ui_game_dir(): the game folder (ultratears.cfg, mods, tools).
static std::string ui_game_dir();
static void ui_cfg_changed(const char *key);   // after a key of ultratears.cfg was written
static bool ui_row_restart(const char *row);   // the overlay: this row's setting needs the game started again

// ---------------------------------------------------------------- Play: the game's settings
// The player's keys in the install folder's ultratears.cfg, read by the game at its start (src/host/boot.c) and kept by
// setup.py on an update: fps=, window=, vsync=, lang=. A key left out is the game's default.
static std::string cfg_path() { return path_join(ui_game_dir(), "ultratears.cfg"); }

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
    ui_cfg_changed(key);
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

static bool toggle(const char *id, float x_right, float cy, bool on) {   // the design's switch, 38 x 22
    float x = x_right - 38, y = cy - 11;
    bool hov, down;
    bool clicked = hit(id, x, y, 38, 22, &hov, &down);
    ImU32 bg = on ? (hov ? C_GREEN_HOVER : C_GREEN) : (hov ? C_FIELD_HOVER : C_FIELD);
    DL->AddRectFilled(P(x, y), P(x_right, y + 22), bg, 11 * S);
    DL->AddCircleFilled(P(on ? x_right - 11 : x + 11, cy), 8 * S, C_TEXT, 24);
    return clicked;
}

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

static void row_label(float cy, const char *key) {
    text(g_media.label, 52, cy - g_media.label->FontSize / S / 2, C_TEXT, T(key));
    g_row = key;   // the controls that follow are this row's (hit)
    if (hovered_rect(P(36, cy - 19), P(764, cy + 19)) && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
        g_row_hot = key;
    if (ui_row_restart(key))   // in words after the name, as the frame counter's key (the circular arrow is "default")
        text(g_media.small, 52 + text_w(g_media.label, T(key)) + 10, cy - g_media.small->FontSize / S / 2, C_SUB,
             T("ovl_after_restart"));
}

// The General page's rows (the screen: window, resolution, scale, anti-aliasing, sharpness, frame limit, vsync, the
// frame counter) from the row at cy down; returns the last row's cy. The caller draws the card under them.
static float general_rows(float cy) {
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
    g_row = nullptr;
    return cy;
}

// Debug's switches in their card from top: the game's Debug mode (debug=1: the statistics lines in the log and a
// console window with it) and what only means something with it. Returns the card's bottom.
static float debug_switches(float top) {
    float cy = top + 28;
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
    g_row = nullptr;
    return bottom;
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
        // the game's own keys (window_sdl.c: F1 the settings overlay, F9 snapshots, F10 the frame counter, F11 full
        // screen; Esc, which lets the mouse go, cancels the wait above)
        bool reserved = sc == SDL_SCANCODE_F1 || sc == SDL_SCANCODE_F9 || sc == SDL_SCANCODE_F10 || sc == SDL_SCANCODE_F11;
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
    g_row = nullptr;   // the rows on it are done
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
    const char *note = g_calib.msg.empty() ? T(g_ingame ? "sticks_note_now" : "sticks_note") : g_calib.msg.c_str();
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
    rich(g_media.small, 52, top + 12, 748 - bw - 16 - 52, {{T(g_ingame ? "ctl_applies_now" : "ctl_applies"), C_SUB}}, 17);
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
    return d.empty() ? path_join(ui_game_dir(), "mods") : (d.size() > 1 && (d[0] == '/' || d[1] == ':') ? d : path_join(ui_game_dir(), d));
}
static void mods_list() {
    if (M.p) {
        M.again = true;   // a change while the list is read: read it once more after
        return;
    }
    std::string py = python_path(ui_game_dir());
    if (py.empty()) {
        M.failed = true;
        return;
    }
    M.p = setup_start({py, path_join(ui_game_dir(), "tools/mods_code.py"), "list"}, ui_game_dir(), path_join(ui_game_dir(), "ultratears-mods.log"));
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
    rich(g_media.small, 52, top + 4, 748 - bw - rw - 24 - 52, {{T(g_ingame ? "mods_note_game" : "mods_note"), C_SUB}}, 17);
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

