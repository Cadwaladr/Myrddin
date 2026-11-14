// The performance panel: frame rates and frame pacing, shown over the picture on request
// (sc-turbo fg-panel; a desktop shortcut can be bound to that). Three steps:
//   1  the frame rate that is shown, generated frames included ("FG")
//   2  that, and the game's own frame rate ("BASE"), with the multiplier
//   3  both, and a graph of the time between shown frames: even pacing is a flat line
// Its picture is drawn here, on the CPU: a few thousand pixels, in tens of microseconds. The
// layer uploads it and a small pass puts it over each frame that goes out. Nothing in this file
// knows about Vulkan, so tools/hud-preview.cpp can draw the same panel into a file.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "hud_font.h"

namespace hud_panel {

struct Colour {
    float r, g, b, a; // not premultiplied, 0 to 1
};

inline constexpr Colour BACK{0.055f, 0.063f, 0.082f, 0.80f};
inline constexpr Colour EDGE{1.0f, 1.0f, 1.0f, 0.09f};
inline constexpr Colour TEXT{0.96f, 0.97f, 0.98f, 1.0f};
inline constexpr Colour DIM{0.62f, 0.66f, 0.71f, 1.0f};
inline constexpr Colour SHOWN{0.38f, 0.90f, 0.66f, 1.0f}; // the shown frames: label and graph
inline constexpr Colour BASE{0.45f, 0.72f, 1.0f, 1.0f};   // the game's own frames
inline constexpr Colour GUIDE{1.0f, 1.0f, 1.0f, 0.16f};

/// A picture in premultiplied RGBA, 8 bits each.
struct Canvas {
    int w{}, h{};
    std::vector<uint8_t> px;

    void Reset(int width, int height) {
        w = width;
        h = height;
        px.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    }
    /// `colour` over what is there, as far as `coverage` (0 to 1) says.
    void Blend(int x, int y, const Colour& colour, float coverage) {
        const float a = colour.a * coverage;
        if (x < 0 || y < 0 || x >= w || y >= h || a <= 0.0f) {
            return;
        }
        uint8_t* p = &px[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4];
        const float keep = 1.0f - a;
        p[0] = static_cast<uint8_t>(colour.r * a * 255.0f + static_cast<float>(p[0]) * keep + 0.5f);
        p[1] = static_cast<uint8_t>(colour.g * a * 255.0f + static_cast<float>(p[1]) * keep + 0.5f);
        p[2] = static_cast<uint8_t>(colour.b * a * 255.0f + static_cast<float>(p[2]) * keep + 0.5f);
        p[3] = static_cast<uint8_t>(a * 255.0f + static_cast<float>(p[3]) * keep + 0.5f);
    }
};

/// How much of the pixel at (px, py) a box with rounded corners covers.
inline float BoxCoverage(float px, float py, float x, float y, float w, float h, float radius) {
    const float qx = std::fabs(px - (x + w * 0.5f)) - (w * 0.5f - radius);
    const float qy = std::fabs(py - (y + h * 0.5f)) - (h * 0.5f - radius);
    const float distance = std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - radius;
    return std::clamp(0.5f - distance, 0.0f, 1.0f);
}

/// A filled box with rounded corners, and a line one pixel wide along its inside (`edge`).
inline void Box(Canvas& c, float x, float y, float w, float h, float radius, const Colour& fill, const Colour& edge) {
    for (int py = static_cast<int>(std::floor(y)); py < static_cast<int>(std::ceil(y + h)); ++py) {
        for (int px = static_cast<int>(std::floor(x)); px < static_cast<int>(std::ceil(x + w)); ++px) {
            const float fx = static_cast<float>(px) + 0.5f, fy = static_cast<float>(py) + 0.5f;
            const float outer = BoxCoverage(fx, fy, x, y, w, h, radius);
            if (outer <= 0.0f) {
                continue;
            }
            c.Blend(px, py, fill, outer);
            const float inner = BoxCoverage(fx, fy, x + 1.0f, y + 1.0f, w - 2.0f, h - 2.0f, radius - 1.0f);
            c.Blend(px, py, edge, outer - inner);
        }
    }
}

/// The table's index of a character (the multiplication sign, UTF-8 C3 97, has the last place); -1: not in it.
inline int Code(const char*& s) {
    const auto ch = static_cast<unsigned char>(*s++);
    if (ch == 0xc3 && static_cast<unsigned char>(*s) == 0x97) {
        ++s;
        return 95;
    }
    return ch >= 32 && ch < 127 ? ch - 32 : -1;
}

/// How far a text reaches. Figures all take the widest figure's room (numbers that change do not shift).
inline float Width(const hud_font::Face& face, const char* text, float tracking = 0.0f) {
    float width = 0.0f;
    while (*text) {
        const int code = Code(text);
        if (code >= 0) {
            width += (code >= 16 && code <= 25 ? face.digit : face.glyphs[code].advance) + tracking;
        }
    }
    return width - (width > 0.0f ? tracking : 0.0f);
}

/// A text from `x` on, on the base line `base`. Returns where it ends.
inline float Text(Canvas& c, const hud_font::Face& face, float x, float base, const char* text, const Colour& colour,
                  float tracking = 0.0f) {
    while (*text) {
        const int code = Code(text);
        if (code < 0) {
            continue;
        }
        const hud_font::Glyph& g = face.glyphs[code];
        const bool figure = code >= 16 && code <= 25;
        const float room = figure ? face.digit : g.advance;
        const int left = static_cast<int>(std::lround(x + (room - g.advance) * 0.5f)) + g.left;
        const int top = static_cast<int>(std::lround(base)) + g.top;
        for (int gy = 0; gy < g.h; ++gy) {
            const uint8_t* row = &hud_font::ATLAS[(g.y + gy) * hud_font::ATLAS_W + g.x];
            for (int gx = 0; gx < g.w; ++gx) {
                if (row[gx]) {
                    c.Blend(left + gx, top + gy, colour, static_cast<float>(row[gx]) / 255.0f);
                }
            }
        }
        x += room + tracking;
    }
    return x;
}

/// A text that ends at `right`.
inline void TextRight(Canvas& c, const hud_font::Face& face, float right, float base, const char* text, const Colour& colour,
                      float tracking = 0.0f) {
    Text(c, face, right - Width(face, text, tracking), base, text, colour, tracking);
}

/// What the panel shows.
struct Stats {
    float shown{};        ///< frames a second that go to the display
    float base{};         ///< the game's own frames a second
    int multiplier{1};    ///< shown frames per frame of the game (1: none generated)
    float target_ms{8.333f}; ///< the display's frame time
    const float* gaps{};  ///< the time between shown frames, ms, the newest last
    int count{};
    /// The graph's caption (the middle gap and the one 99 in 100 stay under), where the caller keeps
    /// them: numbers that change with every frame cannot be read. Below 0: worked out from the gaps.
    float middle{-1.0f}, high{-1.0f};
};

/// The graph of the gaps between shown frames, newest at the right: one column per frame (where
/// there are more frames than columns, a column spans what its frames spanned). `top_ms`: the
/// value at the upper edge.
inline void Graph(Canvas& c, float x, float y, float w, float h, const Stats& s, float top_ms, float line) {
    const int columns = static_cast<int>(w);
    // the display's frame time: where evenly paced frames lie
    if (s.target_ms < top_ms) {
        const int gy = static_cast<int>(std::lround(y + h - s.target_ms / top_ms * h));
        for (int i = 0; i < columns; ++i) {
            if ((i / 3) % 2 == 0) {
                c.Blend(static_cast<int>(x) + i, gy, GUIDE, 1.0f);
            }
        }
    }
    if (s.count < 2) {
        return;
    }
    const int shown = std::min(s.count, columns);
    const float* gaps = s.gaps + (s.count - shown);
    const auto level = [&](float ms) { return y + h - std::clamp(ms / top_ms, 0.0f, 1.0f) * (h - line) - line * 0.5f; };
    float before = level(gaps[0]);
    for (int i = 0; i < shown; ++i) {
        const float at = level(gaps[i]);
        const float next = level(gaps[std::min(i + 1, shown - 1)]);
        // the line's reach in this column: half way to its neighbours
        const float low = std::min({at, (at + before) * 0.5f, (at + next) * 0.5f});
        const float high = std::max({at, (at + before) * 0.5f, (at + next) * 0.5f});
        const int px = static_cast<int>(x) + columns - shown + i;
        for (int py = static_cast<int>(std::floor(low - line)); py <= static_cast<int>(std::ceil(y + h)); ++py) {
            const float fy = static_cast<float>(py) + 0.5f;
            const float away = fy < low ? low - fy : fy > high ? fy - high : 0.0f;
            const float on = std::clamp(line * 0.5f + 0.5f - away, 0.0f, 1.0f);
            if (fy > high && fy < y + h) { // under the line: a fill that fades out downwards
                const float depth = (fy - high) / std::max(y + h - high, 1.0f);
                c.Blend(px, py, Colour{SHOWN.r, SHOWN.g, SHOWN.b, 0.20f * (1.0f - depth)}, 1.0f - on);
            }
            c.Blend(px, py, SHOWN, on);
        }
        before = at;
    }
}

/// The middle value and the one 99 in 100 stay under.
inline void Levels(const Stats& s, float& middle, float& high) {
    middle = high = 0.0f;
    if (s.count < 1) {
        return;
    }
    std::vector<float> sorted(s.gaps, s.gaps + s.count);
    std::sort(sorted.begin(), sorted.end());
    middle = sorted[sorted.size() / 2];
    high = sorted[std::min(sorted.size() - 1, sorted.size() * 99 / 100)];
}

/// The panel's size for a step (1 to 3) at a scale (an index into hud_font::SCALE).
inline void Size(int step, int scale, int& width, int& height) {
    const float k = hud_font::SCALE[scale];
    const float w = step <= 1 ? 112.0f : step == 2 ? 172.0f : 276.0f;
    const float h = step <= 1 ? 42.0f : step == 2 ? 74.0f : 132.0f;
    width = static_cast<int>(std::lround(w * k));
    height = static_cast<int>(std::lround(h * k));
}

/// The panel's empty box for a step. Most of the work of drawing the panel is this, and it never
/// changes: who draws the panel for every frame keeps it and hands it to Draw.
inline void Back(Canvas& c, int step, int scale) {
    int width = 0, height = 0;
    Size(step, scale, width, height);
    c.Reset(width, height);
    Box(c, 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 12.0f * hud_font::SCALE[scale], BACK, EDGE);
}

/// How many frames the graph of step 3 has room for: a column each.
inline int GraphColumns(int scale) {
    const float k = hud_font::SCALE[scale];
    return static_cast<int>(276.0f * k - 2.0f * 15.0f * k);
}

/// Draws the panel into `c` (set to its size). `back`: its empty box, if kept (see Back).
inline void Draw(Canvas& c, int step, const Stats& s, int scale, const Canvas* back = nullptr) {
    using namespace hud_font;
    const float k = SCALE[scale];
    const Face& big = FACE[scale][BIG];
    const Face& label = FACE[scale][LABEL];
    const Face& small = FACE[scale][SMALL];
    int width = 0, height = 0;
    Size(step, scale, width, height);
    if (back && back->w == width && back->h == height) {
        c = *back;
    } else {
        Back(c, step, scale);
    }
    const float w = static_cast<float>(width);

    const float pad = 15.0f * k;
    char text[32];
    const auto rate = [&](float value) {
        std::snprintf(text, sizeof text, "%d", static_cast<int>(std::lround(std::clamp(value, 0.0f, 999.0f))));
        return text;
    };
    // The numbers end at one column; three figures have room.
    const float labels = step <= 1 ? Width(label, "FG", k) : Width(label, "BASE", k);
    const float column = pad + labels + 12.0f * k + 3.0f * big.digit;
    const float row = 32.0f * k;
    const float first = 30.0f * k; // the first row's base line
    const float rise = 5.0f * k;   // the small letters stand this much higher: their middles line up with the figures'

    if (step <= 2) {
        Text(c, label, pad, first - rise, "FG", SHOWN, k);
        TextRight(c, big, column, first, rate(s.shown), TEXT);
    }
    if (step == 2) {
        if (s.multiplier > 1) {
            std::snprintf(text, sizeof text, "\xc3\x97%d", s.multiplier);
            Text(c, label, column + 9.0f * k, first - rise, text, DIM);
        }
        Text(c, label, pad, first + row - rise, "BASE", BASE, k);
        TextRight(c, big, column, first + row, rate(s.base), TEXT);
    }
    if (step >= 3) {
        // One row: the shown rate with the multiplier at the left, the game's own at the right.
        // (Each number in room for its most figures: nothing moves sideways when a rate passes 100.)
        const float end = Text(c, label, pad, first - rise, "FG", SHOWN, k) + 10.0f * k + 3.0f * big.digit;
        TextRight(c, big, end, first, rate(s.shown), TEXT);
        if (s.multiplier > 1) {
            std::snprintf(text, sizeof text, "\xc3\x97%d", s.multiplier);
            Text(c, label, end + 8.0f * k, first - rise, text, DIM);
        }
        TextRight(c, big, w - pad, first, rate(s.base), TEXT);
        TextRight(c, label, w - pad - (s.base >= 99.5f ? 3.0f : 2.0f) * big.digit - 10.0f * k, first - rise, "BASE", BASE, k);
        // Under the numbers: the graph, and what it says in two numbers.
        float middle = s.middle, high = s.high;
        if (middle < 0.0f || high < 0.0f) {
            Levels(s, middle, high);
        }
        // The upper edge: two and a half display frames, or more where frames come slower than that.
        float top = 2.5f * s.target_ms;
        while (middle * 1.6f > top) {
            top *= 2.0f;
        }
        const float gx = pad, gy = first + 13.0f * k, gw = w - 2.0f * pad, gh = 50.0f * k;
        Graph(c, gx, gy, gw, gh, s, top, 1.6f * k);
        const float caption = gy + gh + 17.0f * k;
        Text(c, small, pad, caption, "PACING", DIM, 0.6f * k);
        if (s.count > 0) {
            std::snprintf(text, sizeof text, "%.1f", static_cast<double>(high));
            const float right = w - pad;
            const float number = Width(small, text);
            TextRight(c, small, right, caption, text, TEXT);
            const float p99 = right - number - 5.0f * k;
            TextRight(c, small, p99, caption, "p99", DIM);
            std::snprintf(text, sizeof text, "%.1f ms", static_cast<double>(middle));
            TextRight(c, small, p99 - Width(small, "p99") - 12.0f * k, caption, text, TEXT);
        }
    }
}

} // namespace hud_panel
