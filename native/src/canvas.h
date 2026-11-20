// Copyright (C) 2026 EncidXVII
// SPDX-License-Identifier: GPL-3.0-only

#pragma once
// A small CPU canvas for the VR menu: rounded fills, rings and masks, text in the app's faces (Barlow, Barlow SemiBold,
// Anton; Consolas where Windows has it) and images, into premultiplied, sRGB-encoded RGBA8. Engine- and
// graphics-API-neutral: the core uploads the result as the menu's headset layer.
#include <cstdint>
#include <string>
#include <vector>

namespace canvas {
struct Color {
    float r = 0, g = 0, b = 0, a = 1;  // sRGB 0..1, straight alpha
};
Color rgb(int hex, float a = 1);
Color mix(const Color& a, const Color& b, float t);

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    float cx() const { return x + w / 2; }
    float cy() const { return y + h / 2; }
};

enum Face { Body, Bold, Display, Mono };
enum Align { Left, Center, TopLeft };

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;  // straight alpha
    // The size it was last drawn at, box-filtered once: resampling a 600x900 cover every paint cost about 1 ms.
    mutable int sw = 0, sh = 0;
    mutable std::vector<uint8_t> scaled;
};
// JPG or PNG from disk (stb_image); false when it cannot be read.
bool load_image(const std::wstring& path, Image& out);

class Canvas {
public:
    void resize(int w, int h);
    void clear();
    int width() const { return w_; }
    int height() const { return h_; }
    const uint8_t* pixels() const { return px_.data(); }

    // mode 0: fill; 1: a 2 px ring inside the edge; 2: paints only outside the rounded corners (rounds an image).
    void round(const Rect& r, Color c, float radius, int mode = 0);
    void fill(const Rect& r, Color c);
    void image(const Rect& r, const Image& img);
    void text(const Rect& r, const std::string& utf8, Face face, float size, Color c, Align align = Left, bool wrap = false);
    float measure(const std::string& utf8, Face face, float size);
    // `text` cut with "…" to fit `width`.
    std::string fit(const std::string& utf8, Face face, float size, float width);
    float fade = 1;  // multiplies every alpha drawn (the menu fading in)

private:
    void blend(int x, int y, Color c, float coverage);
    int w_ = 0, h_ = 0;
    std::vector<uint8_t> px_;  // premultiplied
};
}  // namespace canvas
