// Effects gallery: renders every generator with the firmware's own
// fill_effect_run() as a space-time strip — x = time going right, y = pixel
// along the line (pixel 0 at the top) — one binary PPM per effect, for the
// documentation.
// Driven by gallery.py (compile, run, convert to PNG, compose the sheet).
//
//   gallery <out_dir>   →  <out_dir>/<effect>.ppm
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "config_store.h"
#include "dmx_logic.h"

using namespace pixfrog;

namespace {

constexpr uint16_t kPixels = 144;  // one 1 m WS2812B strip
constexpr int kFrames      = 240;  // columns, left to right
constexpr int kFrameMs     = 25;   // 40 fps → 6 s of animation

struct Look {
    uint8_t fx, speed, param, n;
    uint8_t rgb[config::kSceneColorsMax][3];
};

// Speeds (Effect::speed units) and palettes picked to show each effect's
// motion within 6 s.
const Look kLooks[config::kSceneFxCount] = {
    { config::kSceneFxSolid, 4, 0, 2, { { 255, 150, 60 }, { 255, 255, 255 } } },
    { config::kSceneFxChase, 20, 6, 3, { { 255, 40, 40 }, { 40, 255, 80 }, { 60, 90, 255 } } },
    { config::kSceneFxRainbow, 4, 1, 1, { { 255, 255, 255 } } },
    { config::kSceneFxBlobs, 30, 4, 3, { { 0, 90, 255 }, { 255, 0, 140 }, { 0, 255, 160 } } },
    { config::kSceneFxGradient, 15, 2, 3, { { 255, 60, 0 }, { 255, 0, 120 }, { 40, 60, 255 } } },
    { config::kSceneFxFade, 20, 0, 3, { { 255, 30, 30 }, { 30, 255, 90 }, { 40, 80, 255 } } },
    { config::kSceneFxTwinkle, 60, 90, 2, { { 255, 200, 120 }, { 160, 200, 255 } } },
    { config::kSceneFxFire,
      60,
      0,
      4,
      { { 180, 16, 0 }, { 255, 80, 0 }, { 255, 170, 20 }, { 255, 240, 150 } } },
    { config::kSceneFxScanner, 45, 6, 2, { { 255, 0, 0 }, { 0, 160, 255 } } },
    { config::kSceneFxWave, 25, 3, 2, { { 0, 200, 255 }, { 120, 0, 255 } } },
    { config::kSceneFxStripes, 10, 6, 3, { { 255, 255, 255 }, { 255, 0, 60 }, { 0, 80, 255 } } },
};

bool write_ppm(const std::string& path, const std::vector<uint8_t>& rgb) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %u\n255\n", kFrames, kPixels);
    std::fwrite(rgb.data(), 1, rgb.size(), f);
    return std::fclose(f) == 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: gallery <out_dir>\n");
        return 2;
    }
    for (const Look& l : kLooks) {
        config::Effect e{};
        e.generator  = l.fx;
        e.speed      = l.speed;
        e.param      = l.param;
        e.num_colors = l.n;
        std::memcpy(e.colors, l.rgb, sizeof(e.colors));

        // One frame per column: frame f's pixel p lands at row p, column f.
        std::vector<uint8_t> img(static_cast<size_t>(kPixels) * kFrames * 3);
        uint8_t frame[kPixels * 3];
        for (int f = 0; f < kFrames; ++f) {
            dmx::logic::fill_effect_run(frame, sizeof(frame), kPixels, 3, e,
                                        static_cast<uint64_t>(f) * kFrameMs + 10'000);
            for (uint16_t p = 0; p < kPixels; ++p)
                std::memcpy(&img[(static_cast<size_t>(p) * kFrames + f) * 3], &frame[p * 3], 3);
        }
        std::string name = config::scene_fx_label(l.fx);
        for (char& c : name)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!write_ppm(std::string(argv[1]) + "/" + name + ".ppm", img)) {
            std::fprintf(stderr, "cannot write %s\n", name.c_str());
            return 1;
        }
        std::printf("%s\n", name.c_str());
    }
    return 0;
}
