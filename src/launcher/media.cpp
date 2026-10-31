// The fonts, icons and images of assets/launcher (compiled in: build/assets.c), at the window's scale: text and icons
// are drawn at the screen's own pixels, not stretched.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "launcher.h"
#include <cstdio>
#include <cstring>
#include "nanosvg.h"
#include "nanosvgrast.h"
#include "stb_image.h"

extern "C" {
struct launcher_asset { const char *name; const unsigned char *data; size_t size; };
extern const struct launcher_asset launcher_assets[];
}

Media g_media;

static const launcher_asset *asset(const char *name) {
    for (const launcher_asset *a = launcher_assets; a->name; a++)
        if (std::strcmp(a->name, name) == 0)
            return a;
    SDL_Log("asset missing: %s", name);
    return nullptr;
}

static SDL_Texture *texture(SDL_Renderer *r, const unsigned char *rgba, int w, int h) {
    SDL_Texture *t = SDL_CreateTexture(r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);   // RGBA bytes
    if (!t)
        return nullptr;
    SDL_UpdateTexture(t, nullptr, rgba, w * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
    return t;
}

static void drop(Icon &i) {
    if (i.tex)
        SDL_DestroyTexture(i.tex);
    i = Icon();
}

static Icon png(SDL_Renderer *r, const char *name, float logical_w, float logical_h) {
    Icon icon;
    const launcher_asset *a = asset(name);
    if (!a)
        return icon;
    int w, h, n;
    unsigned char *px = stbi_load_from_memory(a->data, (int)a->size, &w, &h, &n, 4);
    if (!px)
        return icon;
    icon.tex = texture(r, px, w, h);
    icon.w = logical_w, icon.h = logical_h;
    stbi_image_free(px);
    return icon;
}

static Icon svg(SDL_Renderer *r, const char *name, float logical, float scale) {
    Icon icon;
    const launcher_asset *a = asset(name);
    if (!a)
        return icon;
    std::string text((const char *)a->data, a->size);   // nsvgParse edits its input
    NSVGimage *img = nsvgParse(&text[0], "px", 96);
    if (!img)
        return icon;
    int px = (int)(logical * scale + 0.5f);
    std::vector<unsigned char> rgba((size_t)px * px * 4);
    NSVGrasterizer *rast = nsvgCreateRasterizer();
    nsvgRasterize(rast, img, 0, 0, px / img->width, rgba.data(), px, px, px * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(img);
    icon.tex = texture(r, rgba.data(), px, px);
    icon.w = icon.h = logical;
    return icon;
}

static unsigned be16(const unsigned char *p) { return p[0] << 8 | p[1]; }
static unsigned be32(const unsigned char *p) { return be16(p) << 16 | be16(p + 2); }

// (ascender - descender) / unitsPerEm of a TrueType font: its whole height for a size of 1 em
static float em_to_height(const unsigned char *d, size_t n) {
    if (n < 12)
        return 1;
    unsigned head = 0, hhea = 0;
    for (unsigned i = 0, count = be16(d + 4); i < count && 12 + 16 * i + 16 <= n; i++) {
        const unsigned char *t = d + 12 + 16 * i;
        if (!std::memcmp(t, "head", 4))
            head = be32(t + 8);
        if (!std::memcmp(t, "hhea", 4))
            hhea = be32(t + 8);
    }
    if (!head || !hhea || head + 20 > n || hhea + 8 > n)
        return 1;
    float upem = (float)be16(d + head + 18);
    int asc = (int16_t)be16(d + hhea + 4), desc = (int16_t)be16(d + hhea + 6);
    return upem > 0 ? (asc - desc) / upem : 1;
}

static ImFont *font(const char *name, float px, const ImWchar *ranges) {
    const launcher_asset *a = asset(name);
    if (!a)
        return nullptr;
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;   // static data
    cfg.OversampleH = 2;
    cfg.PixelSnapH = false;
    std::snprintf(cfg.Name, sizeof cfg.Name, "%s %.0f", name, px);
    // px is the font size as Figma and CSS mean it (the em). ImGui 1.91 sizes a font by its whole height, ascender to
    // descender (hhea), 1.21 em for Inter and Montserrat: asked for the em, it drew every text a fifth too small.
    px *= em_to_height(a->data, a->size);
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF((void *)a->data, (int)a->size, px, &cfg, ranges);
}

static std::vector<std::pair<ImFont *, float>> g_line;   // each font's line height (design pixels), as in the design

float font_line(ImFont *f) {
    for (auto &l : g_line)
        if (l.first == f)
            return l.second;
    return f ? f->FontSize * 1.3f : 20;
}

static ImFont *style(const char *name, float em, float line, float scale, const ImWchar *ranges) {
    ImFont *f = font(name, em * scale, ranges);
    g_line.emplace_back(f, line);
    return f;
}

void media_load(SDL_Renderer *r, float scale) {
    static const ImWchar ranges[] = {
        0x0020, 0x00FF,   // Latin-1
        0x0400, 0x052F,   // Cyrillic
        0x2010, 0x2027,   // dashes, quotes, the ellipsis
        0x2190, 0x2193,   // arrows
        0x2248, 0x2248,   // ≈
        0,
    };
    ImGuiIO &io = ImGui::GetIO();
    io.Fonts->Clear();
    g_line.clear();
    // the design's text styles: size / line height in design pixels (Inter 15 / 20: the user, 2026-10-31)
    g_media.regular = style("Inter_18pt-Regular.ttf", 15, 20, scale, ranges);
    g_media.medium = style("Inter_18pt-Medium.ttf", 15, 20, scale, ranges);
    g_media.small = style("Inter_18pt-Regular.ttf", 13, 18, scale, ranges);
    g_media.title = style("MontserratAlternates-Bold.ttf", 25, 32, scale, ranges);
    g_media.brand = style("MontserratAlternates-SemiBold.ttf", 17, 22, scale, ranges);
    g_media.label = style("Inter_18pt-Regular.ttf", 16, 24, scale, ranges);
    g_media.control = style("Inter_18pt-Medium.ttf", 14, 20, scale, ranges);
    io.FontDefault = g_media.regular;
    for (Icon *i : {&g_media.emblem, &g_media.pattern, &g_media.check, &g_media.error, &g_media.folder, &g_media.warn, &g_media.chevron})
        drop(*i);
    g_media.emblem = png(r, "l_emblem.png", 40, 40);
    g_media.pattern = png(r, "l_pattern.png", 256, 256);
    g_media.check = svg(r, "l_ic_check.svg", 16, scale);
    g_media.error = svg(r, "l_ic_error.svg", 16, scale);
    g_media.warn = svg(r, "l_ic_warn.svg", 16, scale);
    g_media.chevron = svg(r, "l_ic_chevron.svg", 16, scale);
    g_media.folder = svg(r, "l_ic_folder.svg", 24, scale);
}
