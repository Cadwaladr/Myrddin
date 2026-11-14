// The performance panel drawn into files, to look at it without the game: the same code the
// layer uses (fg/hud_panel.h), put over a picture or a flat colour.
//
//   g++ -O2 -std=c++20 -I fg tools/hud-preview.cpp -o build/hud-preview
//   build/hud-preview OUT.ppm [BACKGROUND.ppm] [SCALE=0|1]
//   build/hud-preview --bench        how long drawing takes
//
// Writes one picture with the three steps side by side as they would sit at the top left of
// the frame, each with numbers of a different kind of moment (even pacing, a hitch, uneven).
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>

#include "hud_panel.h"

namespace {

struct Picture {
    int w{}, h{};
    std::vector<uint8_t> rgb;
};

bool ReadPpm(const char* path, Picture& p) {
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        return false;
    }
    int max = 0;
    const bool ok = std::fscanf(f, "P6 %d %d %d", &p.w, &p.h, &max) == 3 && max == 255;
    std::fgetc(f);
    if (ok) {
        p.rgb.resize(static_cast<size_t>(p.w) * static_cast<size_t>(p.h) * 3);
        if (std::fread(p.rgb.data(), 1, p.rgb.size(), f) != p.rgb.size()) {
            p.rgb.clear();
        }
    }
    std::fclose(f);
    return ok && !p.rgb.empty();
}

/// The panel over the picture at (x, y): what the layer's pass does.
void Over(Picture& p, const hud_panel::Canvas& c, int x, int y) {
    for (int cy = 0; cy < c.h; ++cy) {
        for (int cx = 0; cx < c.w; ++cx) {
            if (x + cx >= p.w || y + cy >= p.h) {
                continue;
            }
            const uint8_t* s = &c.px[(static_cast<size_t>(cy) * static_cast<size_t>(c.w) + static_cast<size_t>(cx)) * 4];
            uint8_t* d = &p.rgb[(static_cast<size_t>(y + cy) * static_cast<size_t>(p.w) + static_cast<size_t>(x + cx)) * 3];
            for (int i = 0; i < 3; ++i) {
                d[i] = static_cast<uint8_t>(std::min(255, s[i] + d[i] * (255 - s[3]) / 255));
            }
        }
    }
}

} // namespace

/// How long the panel takes to draw (the layer does it for every frame of the game's).
int Bench() {
    std::vector<float> gaps(512);
    for (size_t i = 0; i < gaps.size(); ++i) {
        gaps[i] = 8.3f + static_cast<float>(i % 7) * 0.3f;
    }
    for (int scale = 0; scale < hud_font::SCALES; ++scale) {
        for (int step = 1; step <= 3; ++step) {
            hud_panel::Stats s;
            s.shown = 117.0f;
            s.base = 39.0f;
            s.multiplier = 3;
            s.gaps = gaps.data();
            s.count = static_cast<int>(gaps.size());
            hud_panel::Canvas c, back;
            hud_panel::Back(back, step, scale);
            for (int kept = 0; kept < 2; ++kept) {
                const auto start = std::chrono::steady_clock::now();
                constexpr int RUNS = 2000;
                for (int i = 0; i < RUNS; ++i) {
                    s.shown = 100.0f + static_cast<float>(i % 20);
                    hud_panel::Draw(c, step, s, scale, kept ? &back : nullptr);
                }
                const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / RUNS;
                std::printf("scale %.1f step %d %s: %6.1f us (%dx%d)\n", static_cast<double>(hud_font::SCALE[scale]), step,
                            kept ? "box kept " : "box drawn", us, c.w, c.h);
            }
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && !std::strcmp(argv[1], "--bench")) {
        return Bench();
    }
    if (argc < 2) {
        std::fprintf(stderr, "usage: hud-preview OUT.ppm [BACKGROUND.ppm] [SCALE=0|1]\n");
        return 1;
    }
    const int scale = argc > 3 ? std::atoi(argv[3]) : 0;
    Picture p;
    if (argc < 3 || !std::strcmp(argv[2], "-") || !ReadPpm(argv[2], p)) {
        p.w = 1000;
        p.h = 640;
        p.rgb.resize(static_cast<size_t>(p.w) * static_cast<size_t>(p.h) * 3);
        for (int y = 0; y < p.h; ++y) { // a sky that is light at the top, dark below: the panel over both
            for (int x = 0; x < p.w; ++x) {
                uint8_t* d = &p.rgb[(static_cast<size_t>(y) * static_cast<size_t>(p.w) + static_cast<size_t>(x)) * 3];
                const float t = static_cast<float>(y) / static_cast<float>(p.h);
                d[0] = static_cast<uint8_t>(205.0f - 185.0f * t);
                d[1] = static_cast<uint8_t>(215.0f - 185.0f * t);
                d[2] = static_cast<uint8_t>(230.0f - 180.0f * t);
            }
        }
    }
    std::mt19937 random{7};
    std::normal_distribution<float> noise{0.0f, 1.0f};
    const float k = hud_font::SCALE[scale];
    int x = static_cast<int>(20 * k);
    for (int row = 0; row < 3; ++row) {
        // row 0: x3, evenly paced; row 1: x4 with a hitch; row 2: x2, uneven
        std::vector<float> gaps;
        const float base = row == 0 ? 39.0f : row == 1 ? 29.6f : 57.0f;
        const int multiplier = row == 0 ? 3 : row == 1 ? 4 : 2;
        const float shown = base * static_cast<float>(multiplier);
        for (int i = 0; i < 400; ++i) {
            float gap = 1000.0f / shown + noise(random) * (row == 2 ? 1.9f : 0.25f);
            if (row == 1 && (i == 250 || i == 251)) {
                gap = i == 250 ? 31.0f : 2.0f;
            }
            if (row == 2 && i % 2) {
                gap += 2.5f;
            }
            gaps.push_back(std::max(gap, 0.5f));
        }
        hud_panel::Stats s;
        s.shown = shown;
        s.base = base;
        s.multiplier = multiplier;
        s.gaps = gaps.data();
        s.count = static_cast<int>(gaps.size());
        int px = x;
        for (int step = 1; step <= 3; ++step) {
            hud_panel::Canvas c;
            hud_panel::Draw(c, step, s, scale);
            Over(p, c, px, static_cast<int>(20 * k) + row * static_cast<int>(190 * k));
            px += c.w + static_cast<int>(24 * k);
        }
    }
    FILE* f = std::fopen(argv[1], "wb");
    if (!f) {
        return 1;
    }
    std::fprintf(f, "P6\n%d %d\n255\n", p.w, p.h);
    std::fwrite(p.rgb.data(), 1, p.rgb.size(), f);
    std::fclose(f);
    return 0;
}
