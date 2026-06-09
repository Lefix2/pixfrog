// splashgen — bakes the animated Collecti'Frog logo (docs/img/frog-anim.svg)
// into a 1bpp frame strip the firmware splash replays via canvas_draw_mask.
// That SVG is also what the web About page embeds, so the two stay in sync.
//
// The web logo is a static SVG (frog head + eye rings + pupils + 4 water
// bands) driven entirely by CSS transforms:
//   - the creature RISES from translateY(440)->0 (emerges from the water),
//   - the 4 wave bands POP IN (scale 0.05->1) then idle-bob,
//   - the pupils BLINK (scaleY 1->0.06->1).
// A horizontal clip at viewBox y=507 (the water line) hides the lower frog so
// it looks like it surfaces. We can't run CSS on an MCU, so this tool replays
// that timeline at gen-time: per frame it applies each group's affine to the
// parsed bezier control points, rasterises (supersampled), composites the
// clipped frog under the waves, downsamples and thresholds to 1bpp.
//
// Output is a self-contained splash_anim.cpp exposing the accessors declared in
// ui_internal.h. Regenerate it; don't hand-edit. See README.md.

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg.h"
#include "nanosvgrast.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ── Scene geometry (viewBox units of the about.html SVG) ─────────────────────
// Window framed on the frog + its near water; the lower frog is clipped at the
// water line so the rise reads as "emerging".
constexpr float kWinX0 = 200.f, kWinY0 = 265.f, kWinX1 = 690.f, kWinY1 = 610.f;
constexpr float kWaterClipY = 507.f;  // SVG #waterClip lower edge
constexpr int kSS           = 3;      // supersample factor (AA before threshold)

// ── Shape grouping (document order from about.html) ──────────────────────────
// head:5  eye-rings:2  pupilR:2(base+mask)  pupilL:2(base+mask)  waves:4  = 15.
constexpr int kShapeCount = 15;
enum class Grp { Frog, FrogPupil, Wave, Skip };

// Per-shape group + which wave (for stagger), filled by classify().
struct ShapeInfo {
    Grp grp;
    int wave;  // 0..3 for waves, else -1
};

ShapeInfo classify(int i) {
    if (i <= 4) return { Grp::Frog, -1 };       // head
    if (i <= 6) return { Grp::Frog, -1 };       // eye rings
    if (i == 7) return { Grp::FrogPupil, -1 };  // pupil R base
    if (i == 8) return { Grp::Skip, -1 };       // pupil R mask highlight
    if (i == 9) return { Grp::FrogPupil, -1 };  // pupil L base
    if (i == 10) return { Grp::Skip, -1 };      // pupil L mask highlight
    return { Grp::Wave, i - 11 };               // waves 11..14
}

// ── CSS cubic-bezier easing: solve for y given time fraction x ───────────────
double bezAxis(double p0, double p1, double t) {
    const double mt = 1.0 - t;
    return 3 * mt * mt * t * p0 + 3 * mt * t * t * p1 + t * t * t;
}
double ease(double x1, double y1, double x2, double y2, double x) {
    if (x <= 0) return 0.0;
    if (x >= 1) return 1.0;
    double lo = 0, hi = 1, t = x;
    for (int i = 0; i < 24; ++i) {
        const double xx = bezAxis(x1, x2, t);
        if (std::fabs(xx - x) < 1e-5) break;
        if (xx < x)
            lo = t;
        else
            hi = t;
        t = 0.5 * (lo + hi);
    }
    return bezAxis(y1, y2, t);
}

// ── Animation timeline (seconds), mirrors the about.html CSS ─────────────────
float creatureRiseTY(double t) {
    const double p = ease(0.18, 0.78, 0.26, 1.0, (t - 1.3) / 1.7);
    return static_cast<float>((1.0 - p) * 440.0);
}
float waveScale(int w, double t) {
    static const double delay[4] = { 0.20, 0.32, 0.26, 0.14 };
    const double p               = ease(0.22, 0.8, 0.25, 1.0, (t - delay[w]) / 1.1);
    return static_cast<float>(0.05 + 0.95 * p);
}
float waveOpacity(int w, double t) {
    static const double delay[4] = { 0.20, 0.32, 0.26, 0.14 };
    const double p               = (t - delay[w]) / 1.1;
    if (p <= 0) return 0.f;
    if (p >= 0.28) return 1.f;
    return static_cast<float>(p / 0.28);
}
float waveBobTY(int w, double t) {
    static const double wy[4]    = { 1.0, -0.7, 0.85, -1.0 };
    static const double durM[4]  = { 1.0, 1.28, 0.84, 1.45 };
    static const double delay[4] = { 1.7, 1.9, 2.1, 1.8 };
    const double dt              = t - delay[w];
    if (dt <= 0) return 0.f;
    const double dur = 4.2 * durM[w];
    return static_cast<float>(6.5 * wy[w] * std::sin(2.0 * M_PI * dt / dur));
}
float pupilBlinkSY(double t) {
    double ph = (t - 3.05) / 4.0;
    if (ph < 0) return 1.f;
    ph -= std::floor(ph);  // phase 0..1 within the 4s period
    // keyframes: 0%->1, 4%->0.06, 8%->1
    if (ph < 0.04) return static_cast<float>(1.0 + (0.06 - 1.0) * (ph / 0.04));
    if (ph < 0.08) return static_cast<float>(0.06 + (1.0 - 0.06) * ((ph - 0.04) / 0.04));
    return 1.f;
}

// ── Affine helpers applied to flattened bezier control points ────────────────
void shapeCenter(const NSVGshape* s, float& cx, float& cy) {
    cx = 0.5f * (s->bounds[0] + s->bounds[2]);
    cy = 0.5f * (s->bounds[1] + s->bounds[3]);
}
// Restore original pts, then transform: scale (sx,sy) about (cx,cy) then +tx,+ty.
void applyXform(NSVGshape* s, const std::vector<float>& orig, size_t& cursor, float sx, float sy,
                float cx, float cy, float tx, float ty) {
    for (NSVGpath* p = s->paths; p; p = p->next) {
        for (int i = 0; i < p->npts * 2; i += 2) {
            const float ox = orig[cursor++];
            const float oy = orig[cursor++];
            p->pts[i]      = cx + (ox - cx) * sx + tx;
            p->pts[i + 1]  = cy + (oy - cy) * sy + ty;
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: %s <frog-anim.svg> <out.cpp> [outW=140] [fps=12] [durMs=3600]\n"
                     "       %s <svg> preview            (ASCII dump of a late frame)\n",
                     argv[0], argv[0]);
        return 2;
    }
    const char* in_path  = argv[1];
    const char* out_path = argv[2];
    const bool preview   = std::strcmp(out_path, "preview") == 0;
    const int outW       = argc > 3 ? std::atoi(argv[3]) : 140;
    const int fps        = argc > 4 ? std::atoi(argv[4]) : 12;
    const int durMs      = argc > 5 ? std::atoi(argv[5]) : 3600;

    // Slurp the file and hand nanosvg just the <svg>…</svg> element — works
    // whether the input is the standalone frog-anim.svg or an HTML page embedding it.
    FILE* in = std::fopen(in_path, "rb");
    if (!in) {
        std::fprintf(stderr, "splashgen: cannot open %s\n", in_path);
        return 1;
    }
    std::fseek(in, 0, SEEK_END);
    const long in_len = std::ftell(in);
    std::fseek(in, 0, SEEK_SET);
    std::string src(static_cast<size_t>(in_len), '\0');
    if (std::fread(&src[0], 1, static_cast<size_t>(in_len), in) != static_cast<size_t>(in_len)) {
        std::fprintf(stderr, "splashgen: short read on %s\n", in_path);
        std::fclose(in);
        return 1;
    }
    std::fclose(in);

    const size_t svg_beg = src.find("<svg");
    const size_t svg_end = src.find("</svg>");
    if (svg_beg == std::string::npos || svg_end == std::string::npos) {
        std::fprintf(stderr, "splashgen: no <svg>…</svg> found in %s\n", in_path);
        return 1;
    }
    std::string svg = src.substr(svg_beg, svg_end - svg_beg + 6);

    // nsvgParse mutates its input buffer in place.
    NSVGimage* img = nsvgParse(&svg[0], "px", 96.0f);
    if (!img) {
        std::fprintf(stderr, "splashgen: failed to parse SVG from %s\n", in_path);
        return 1;
    }

    // Collect shapes in document order; back up every control point.
    std::vector<NSVGshape*> shapes;
    for (NSVGshape* s = img->shapes; s; s = s->next)
        shapes.push_back(s);
    if (static_cast<int>(shapes.size()) != kShapeCount) {
        std::fprintf(stderr, "splashgen: expected %d shapes, got %zu — SVG structure changed\n",
                     kShapeCount, shapes.size());
        return 1;
    }
    std::vector<std::vector<float>> orig(shapes.size());
    for (size_t si = 0; si < shapes.size(); ++si) {
        for (NSVGpath* p = shapes[si]->paths; p; p = p->next)
            for (int i = 0; i < p->npts * 2; ++i)
                orig[si].push_back(p->pts[i]);
    }

    const float winW = kWinX1 - kWinX0, winH = kWinY1 - kWinY0;
    const int W  = outW;
    const int H  = static_cast<int>(std::lround(winH / winW * W));
    const int sw = W * kSS, sh = H * kSS;
    const float scale   = static_cast<float>(sw) / winW;  // uniform viewBox->px
    const float tx0     = -kWinX0 * scale;
    const float ty0     = -kWinY0 * scale;
    const int clipRowSS = static_cast<int>((kWaterClipY - kWinY0) * scale);

    const int frameMs = static_cast<int>(std::lround(1000.0 / fps));
    const int frames  = preview ? 1 : std::max(1, durMs / frameMs);

    NSVGrasterizer* rast = nsvgCreateRasterizer();
    std::vector<unsigned char> bufFrog(sw * sh * 4), bufWave(sw * sh * 4);
    std::vector<unsigned char> cov(W * H);  // downsampled coverage 0..255

    const int stride = (W + 7) / 8;
    std::vector<std::vector<uint8_t>> outFrames;

    auto setVisible = [&](Grp want) {
        for (size_t si = 0; si < shapes.size(); ++si) {
            const Grp g   = classify(static_cast<int>(si)).grp;
            const bool on = (want == Grp::Frog) ? (g == Grp::Frog || g == Grp::FrogPupil)
                                                : (g == Grp::Wave);
            if (on)
                shapes[si]->flags |= NSVG_FLAGS_VISIBLE;
            else
                shapes[si]->flags &= ~NSVG_FLAGS_VISIBLE;
        }
    };

    for (int f = 0; f < frames; ++f) {
        const double t = preview ? (durMs * 0.001 * 0.92) : (f * frameMs * 0.001);

        // Position every shape for this instant.
        const float riseTY = creatureRiseTY(t);
        const float blink  = pupilBlinkSY(t);
        for (size_t si = 0; si < shapes.size(); ++si) {
            const ShapeInfo info = classify(static_cast<int>(si));
            size_t cur           = 0;
            float cx, cy;
            shapeCenter(shapes[si], cx, cy);
            switch (info.grp) {
            case Grp::Frog: applyXform(shapes[si], orig[si], cur, 1, 1, cx, cy, 0, riseTY); break;
            case Grp::FrogPupil:  // blink scaleY about its centre, then ride the rise
                applyXform(shapes[si], orig[si], cur, 1, blink, cx, cy, 0, riseTY);
                break;
            case Grp::Wave: {
                const float s  = waveScale(info.wave, t);
                const float by = waveBobTY(info.wave, t);
                applyXform(shapes[si], orig[si], cur, s, s, cx, cy, 0, by);
                shapes[si]->opacity = waveOpacity(info.wave, t);
                break;
            }
            case Grp::Skip: break;
            }
        }

        // Two passes: clipped frog, then waves; composite by max coverage
        // (single ink colour, so max is exact).
        std::memset(bufFrog.data(), 0, bufFrog.size());
        std::memset(bufWave.data(), 0, bufWave.size());
        setVisible(Grp::Frog);
        nsvgRasterize(rast, img, tx0, ty0, scale, bufFrog.data(), sw, sh, sw * 4);
        for (int y = clipRowSS; y < sh; ++y)  // hide frog below the water line
            std::memset(&bufFrog[y * sw * 4], 0, sw * 4);
        setVisible(Grp::Wave);
        nsvgRasterize(rast, img, tx0, ty0, scale, bufWave.data(), sw, sh, sw * 4);

        // Box-downsample alpha, then threshold to 1bpp.
        std::vector<uint8_t> frame(stride * H, 0);
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                int acc = 0;
                for (int dy = 0; dy < kSS; ++dy)
                    for (int dx = 0; dx < kSS; ++dx) {
                        const int sx = x * kSS + dx, sy = y * kSS + dy;
                        const int idx  = (sy * sw + sx) * 4 + 3;  // alpha
                        const int a    = bufFrog[idx] > bufWave[idx] ? bufFrog[idx] : bufWave[idx];
                        acc           += a;
                    }
                cov[y * W + x] = static_cast<uint8_t>(acc / (kSS * kSS));
                if (cov[y * W + x] >= 128) frame[y * stride + (x >> 3)] |= 0x80u >> (x & 7);
            }
        }
        outFrames.push_back(std::move(frame));

        if (preview) {
            const char* ramp = " .:-=+*#%@";
            for (int y = 0; y < H; y += 2) {
                std::string line;
                for (int x = 0; x < W; ++x)
                    line += ramp[cov[y * W + x] * 9 / 255];
                std::printf("%s\n", line.c_str());
            }
            std::printf("\nframe %dx%d (window %.0fx%.0f, scale %.3f)\n", W, H, winW, winH, scale);
        }
    }

    if (preview) {
        nsvgDelete(img);
        nsvgDeleteRasterizer(rast);
        return 0;
    }

    FILE* out = std::fopen(out_path, "w");
    if (!out) {
        std::fprintf(stderr, "splashgen: cannot write %s\n", out_path);
        return 1;
    }
    std::fprintf(out,
                 "// GENERATED by tools/splashgen — do not edit.\n"
                 "// Source: .github/pages/about.html animated Collecti'Frog logo.\n"
                 "// %d frames %dx%d @ %d fps (%d ms/frame), 1bpp MSB-first, stride %d.\n\n"
                 "#include \"ui_internal.h\"\n\n"
                 "#ifdef CONFIG_PIXFROG_DISPLAY_TFT\n\n"
                 "namespace pixfrog::ui::detail {\n\n"
                 "namespace {\n"
                 "constexpr int kW = %d;\n"
                 "constexpr int kH = %d;\n"
                 "constexpr int kStride = %d;\n"
                 "constexpr int kCount = %d;\n"
                 "constexpr uint32_t kFrameMs = %d;\n\n"
                 "const uint8_t kFrames[kCount][kStride * kH] = {\n",
                 frames, W, H, fps, frameMs, stride, W, H, stride, frames, frameMs);
    for (int f = 0; f < frames; ++f) {
        std::fprintf(out, "  {");
        const auto& fr = outFrames[f];
        for (size_t i = 0; i < fr.size(); ++i) {
            if (i % 16 == 0) std::fprintf(out, "\n    ");
            std::fprintf(out, "0x%02x,", fr[i]);
        }
        std::fprintf(out, "\n  },\n");
    }
    std::fprintf(out, "};\n"
                      "}  // namespace\n\n"
                      "int splash_anim_w() { return kW; }\n"
                      "int splash_anim_h() { return kH; }\n"
                      "int splash_anim_count() { return kCount; }\n"
                      "uint32_t splash_anim_frame_ms() { return kFrameMs; }\n"
                      "const uint8_t* splash_anim_frame(int i) {\n"
                      "    if (i < 0) i = 0;\n"
                      "    if (i >= kCount) i = kCount - 1;\n"
                      "    return kFrames[i];\n"
                      "}\n\n"
                      "}  // namespace pixfrog::ui::detail\n\n"
                      "#endif  // CONFIG_PIXFROG_DISPLAY_TFT\n");
    std::fclose(out);
    std::printf("splashgen: wrote %s — %d frames %dx%d @ %dfps, %d B mask data\n", out_path, frames,
                W, H, fps, static_cast<int>(stride * H * frames));

    nsvgDelete(img);
    nsvgDeleteRasterizer(rast);
    return 0;
}
