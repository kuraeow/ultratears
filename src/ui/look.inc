// The look and the widgets the launcher (src/launcher/ui.cpp) and the game's overlay (src/overlay/overlay.cpp) share:
// included into each of them, so everything here is static to its file. Layout in design pixels, times the scale S.
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
static bool g_ingame;          // the game's overlay (src/overlay): settings apply at once where they can, its own words
// The settings row being laid out (row_label) and the one under the mouse or with the focus this frame: the overlay's
// description block shows what that one does.
static const char *g_row, *g_row_hot;
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
    if (enabled && g_row && (*hover || (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible)))
        g_row_hot = g_row;
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

// An icon's texture: its own (the launcher: SDL_Renderer), or the font atlas it was packed into (the overlay: media.cpp)
static ImTextureID icon_tex(const Icon &i) { return i.atlas ? ImGui::GetIO().Fonts->TexID : i.tex; }

static void icon(const Icon &i, float x, float y, float size = 0) {
    if (!i.tex && !i.atlas)
        return;
    float w = size ? size : i.w, h = size ? size : i.h;
    DL->AddImage(icon_tex(i), P(x, y), P(x + w, y + h), i.uv0, i.uv1);
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

