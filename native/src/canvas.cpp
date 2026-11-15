#include "canvas.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <stb_truetype.h>
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include <stb_image.h>

// Embedded by CMake (fonts.cpp): the app's faces, so every engine's menu has them.
extern const unsigned char kBarlowRegular[], kBarlowSemiBold[], kAnton[];
extern const size_t kBarlowRegularSize, kBarlowSemiBoldSize, kAntonSize;

namespace {
struct Font {
    stbtt_fontinfo info{};
    std::vector<unsigned char> data;
    bool ok = false;
};

struct Glyph {
    std::vector<unsigned char> bitmap;
    int w = 0, h = 0, x0 = 0, y0 = 0;
};

std::mutex g_fontMutex;
Font g_fonts[4];
std::map<uint64_t, Glyph> g_glyphs;

Font& font(canvas::Face face) {
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        auto init = [](Font& f, const unsigned char* data, size_t size) {
            f.data.assign(data, data + size);
            f.ok = stbtt_InitFont(&f.info, f.data.data(), stbtt_GetFontOffsetForIndex(f.data.data(), 0)) != 0;
        };
        init(g_fonts[canvas::Body], kBarlowRegular, kBarlowRegularSize);
        init(g_fonts[canvas::Bold], kBarlowSemiBold, kBarlowSemiBoldSize);
        init(g_fonts[canvas::Display], kAnton, kAntonSize);
        // Diagnostics read in a monospaced face where Windows has one; Barlow otherwise.
        wchar_t dir[MAX_PATH];
        UINT n = GetWindowsDirectoryW(dir, MAX_PATH);
        std::wstring path = std::wstring(dir, n) + L"\\Fonts\\consola.ttf";
        if (FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO)) {
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            fseek(f, 0, SEEK_SET);
            g_fonts[canvas::Mono].data.resize(size > 0 ? size : 0);
            if (size > 0) fread(g_fonts[canvas::Mono].data.data(), 1, size, f);
            fclose(f);
            g_fonts[canvas::Mono].ok = size > 0 && stbtt_InitFont(&g_fonts[canvas::Mono].info, g_fonts[canvas::Mono].data.data(), 0) != 0;
        }
        if (!g_fonts[canvas::Mono].ok) init(g_fonts[canvas::Mono], kBarlowRegular, kBarlowRegularSize);
    }
    return g_fonts[face];
}

// UTF-8 to code points.
std::vector<int> decode(const std::string& s) {
    std::vector<int> out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        int cp = c, n = 1;
        if (c >= 0xF0) cp = c & 7, n = 4;
        else if (c >= 0xE0) cp = c & 15, n = 3;
        else if (c >= 0xC0) cp = c & 31, n = 2;
        for (int k = 1; k < n && i + k < s.size(); ++k) cp = (cp << 6) | ((unsigned char)s[i + k] & 63);
        out.push_back(cp);
        i += n;
    }
    return out;
}

const Glyph& glyph(canvas::Face face, float size, int cp) {
    uint64_t key = ((uint64_t)face << 56) | ((uint64_t)(size * 4) << 32) | (uint32_t)cp;
    auto it = g_glyphs.find(key);
    if (it != g_glyphs.end()) return it->second;
    Font& f = font(face);
    Glyph g;
    if (f.ok) {
        float scale = stbtt_ScaleForMappingEmToPixels(&f.info, size);
        unsigned char* bmp = stbtt_GetCodepointBitmap(&f.info, scale, scale, cp, &g.w, &g.h, &g.x0, &g.y0);
        if (bmp) {
            g.bitmap.assign(bmp, bmp + (size_t)g.w * g.h);
            stbtt_FreeBitmap(bmp, nullptr);
        }
    }
    if (g_glyphs.size() > 4096) g_glyphs.clear();
    return g_glyphs.emplace(key, std::move(g)).first->second;
}

float advance(canvas::Face face, float size, int cp, int next) {
    Font& f = font(face);
    if (!f.ok) return size * 0.5f;
    float scale = stbtt_ScaleForMappingEmToPixels(&f.info, size);
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&f.info, cp, &adv, &lsb);
    float a = adv * scale;
    if (next) a += stbtt_GetCodepointKernAdvance(&f.info, cp, next) * scale;
    return a;
}

float line_metrics(canvas::Face face, float size, float& ascent) {
    Font& f = font(face);
    if (!f.ok) {
        ascent = size * 0.8f;
        return size;
    }
    float scale = stbtt_ScaleForMappingEmToPixels(&f.info, size);
    int a = 0, d = 0, gap = 0;
    stbtt_GetFontVMetrics(&f.info, &a, &d, &gap);
    ascent = a * scale;
    return (a - d) * scale;
}

float width_of(const std::vector<int>& cps, size_t from, size_t to, canvas::Face face, float size) {
    float w = 0;
    for (size_t i = from; i < to; ++i) w += advance(face, size, cps[i], i + 1 < to ? cps[i + 1] : 0);
    return w;
}

// Signed distance from p to a rounded rect (centre c, half extents h, radius r): negative inside.
float rounded(float px, float py, float cx, float cy, float hx, float hy, float r) {
    float qx = std::fabs(px - cx) - (hx - r), qy = std::fabs(py - cy) - (hy - r);
    float ox = std::max(qx, 0.f), oy = std::max(qy, 0.f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.f) - r;
}
}  // namespace

namespace canvas {
Color rgb(int hex, float a) { return {((hex >> 16) & 255) / 255.f, ((hex >> 8) & 255) / 255.f, (hex & 255) / 255.f, a}; }

Color mix(const Color& a, const Color& b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

bool load_image(const std::wstring& path, Image& out) {
    FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> bytes(size > 0 ? size : 0);
    if (size > 0) fread(bytes.data(), 1, size, f);
    fclose(f);
    int w = 0, h = 0, n = 0;
    unsigned char* data = stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &n, 4);
    if (!data) return false;
    out.w = w, out.h = h;
    out.rgba.assign(data, data + (size_t)w * h * 4);
    stbi_image_free(data);
    return true;
}

void Canvas::resize(int w, int h) {
    w_ = w, h_ = h;
    px_.assign((size_t)w * h * 4, 0);
}

void Canvas::clear() { std::fill(px_.begin(), px_.end(), (uint8_t)0); }

void Canvas::blend(int x, int y, Color c, float coverage) {
    if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
    float a = c.a * coverage * fade;
    if (a <= 0) return;
    uint8_t* p = &px_[((size_t)y * w_ + x) * 4];
    float inv = 1 - a;
    p[0] = (uint8_t)std::lround(std::min(255.f, c.r * a * 255 + p[0] * inv));
    p[1] = (uint8_t)std::lround(std::min(255.f, c.g * a * 255 + p[1] * inv));
    p[2] = (uint8_t)std::lround(std::min(255.f, c.b * a * 255 + p[2] * inv));
    p[3] = (uint8_t)std::lround(std::min(255.f, a * 255 + p[3] * inv));
}

void Canvas::fill(const Rect& r, Color c) {
    int x0 = std::max(0, (int)std::floor(r.x)), y0 = std::max(0, (int)std::floor(r.y));
    int x1 = std::min(w_, (int)std::ceil(r.x + r.w)), y1 = std::min(h_, (int)std::ceil(r.y + r.h));
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            // Partial coverage on fractional edges.
            float cx = std::clamp(std::min((float)x + 1, r.x + r.w) - std::max((float)x, r.x), 0.f, 1.f);
            float cy = std::clamp(std::min((float)y + 1, r.y + r.h) - std::max((float)y, r.y), 0.f, 1.f);
            blend(x, y, c, cx * cy);
        }
}

void Canvas::round(const Rect& r, Color c, float radius, int mode) {
    radius = std::clamp(radius, 0.f, std::min(r.w, r.h) / 2);
    int x0 = std::max(0, (int)std::floor(r.x) - 1), y0 = std::max(0, (int)std::floor(r.y) - 1);
    int x1 = std::min(w_, (int)std::ceil(r.x + r.w) + 1), y1 = std::min(h_, (int)std::ceil(r.y + r.h) + 1);
    float cx = r.cx(), cy = r.cy(), hx = r.w / 2, hy = r.h / 2;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            float d = rounded(x + 0.5f, y + 0.5f, cx, cy, hx, hy, radius);
            float inside = std::clamp(0.5f - d, 0.f, 1.f), cov = inside;
            if (mode == 1) cov = inside - std::clamp(0.5f - (d + 2.f), 0.f, 1.f);
            else if (mode == 2) {
                bool inRect = x + 0.5f >= r.x && x + 0.5f < r.x + r.w && y + 0.5f >= r.y && y + 0.5f < r.y + r.h;
                cov = inRect ? 1 - inside : 0;
            }
            if (cov > 0) blend(x, y, c, cov);
        }
}

void Canvas::image(const Rect& r, const Image& img) {
    if (img.w <= 0 || img.h <= 0) return;
    int x0 = std::max(0, (int)std::floor(r.x)), y0 = std::max(0, (int)std::floor(r.y));
    int x1 = std::min(w_, (int)std::ceil(r.x + r.w)), y1 = std::min(h_, (int)std::ceil(r.y + r.h));
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            float u = ((x + 0.5f) - r.x) / r.w * img.w - 0.5f, v = ((y + 0.5f) - r.y) / r.h * img.h - 0.5f;
            int iu = std::clamp((int)std::floor(u), 0, img.w - 1), iv = std::clamp((int)std::floor(v), 0, img.h - 1);
            int iu1 = std::min(iu + 1, img.w - 1), iv1 = std::min(iv + 1, img.h - 1);
            float fu = std::clamp(u - iu, 0.f, 1.f), fv = std::clamp(v - iv, 0.f, 1.f);
            float c[4];
            for (int k = 0; k < 4; ++k) {
                auto at = [&](int a, int b) { return img.rgba[((size_t)b * img.w + a) * 4 + k] / 255.f; };
                c[k] = (at(iu, iv) * (1 - fu) + at(iu1, iv) * fu) * (1 - fv) + (at(iu, iv1) * (1 - fu) + at(iu1, iv1) * fu) * fv;
            }
            blend(x, y, {c[0], c[1], c[2], c[3]}, 1);
        }
}

float Canvas::measure(const std::string& utf8, Face face, float size) {
    std::lock_guard lock(g_fontMutex);
    auto cps = decode(utf8);
    return width_of(cps, 0, cps.size(), face, size);
}

std::string Canvas::fit(const std::string& utf8, Face face, float size, float width) {
    if (measure(utf8, face, size) <= width) return utf8;
    auto cps = decode(utf8);
    for (size_t n = cps.size(); n > 0; --n) {
        std::string cut;
        size_t bytes = 0, count = 0;
        for (size_t i = 0; i < utf8.size() && count < n; ++count) {
            unsigned char c = (unsigned char)utf8[i];
            size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
            i += len;
            bytes = i;
        }
        cut = utf8.substr(0, bytes);
        while (!cut.empty() && cut.back() == ' ') cut.pop_back();
        cut += "\xE2\x80\xA6";
        if (measure(cut, face, size) <= width) return cut;
    }
    return "\xE2\x80\xA6";
}

void Canvas::text(const Rect& r, const std::string& utf8, Face face, float size, Color c, Align align, bool wrap) {
    std::lock_guard lock(g_fontMutex);
    auto cps = decode(utf8);
    // Lines: the whole text, or words wrapped to the rect's width.
    std::vector<std::pair<size_t, size_t>> lines;
    if (!wrap) lines.push_back({0, cps.size()});
    else {
        size_t start = 0, lastSpace = std::string::npos;
        for (size_t i = 0; i <= cps.size(); ++i) {
            if (i == cps.size() || cps[i] == '\n') {
                lines.push_back({start, i});
                start = i + 1;
                lastSpace = std::string::npos;
                continue;
            }
            if (cps[i] == ' ') lastSpace = i;
            if (width_of(cps, start, i + 1, face, size) > r.w && lastSpace != std::string::npos && lastSpace > start) {
                lines.push_back({start, lastSpace});
                start = lastSpace + 1;
                lastSpace = std::string::npos;
            }
        }
    }
    float ascent = 0, lineH = line_metrics(face, size, ascent) * 1.05f;
    float total = lineH * lines.size();
    float y = align == TopLeft ? r.y : r.y + (r.h - total) / 2;
    for (auto [from, to] : lines) {
        float w = width_of(cps, from, to, face, size);
        float x = align == Center ? r.x + (r.w - w) / 2 : r.x;
        float baseline = y + ascent;
        for (size_t i = from; i < to; ++i) {
            const Glyph& g = glyph(face, size, cps[i]);
            int gx = (int)std::lround(x) + g.x0, gy = (int)std::lround(baseline) + g.y0;
            for (int j = 0; j < g.h; ++j)
                for (int k = 0; k < g.w; ++k) {
                    unsigned char a = g.bitmap[(size_t)j * g.w + k];
                    if (a) blend(gx + k, gy + j, c, a / 255.f);
                }
            x += advance(face, size, cps[i], i + 1 < to ? cps[i + 1] : 0);
        }
        y += lineH;
    }
}
}  // namespace canvas
