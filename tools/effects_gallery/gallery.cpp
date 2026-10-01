// Effects gallery: renders every scene effect with the firmware's own
// fill_scene_pattern() as a space-time strip — x = pixel along the line,
// y = time going down — one binary PPM per effect, for the documentation.
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
constexpr int kRows        = 240;  // frames, top to bottom
constexpr int kFrameMs     = 25;   // 40 fps → 6 s of animation

struct Look {
    uint8_t fx, speed, param, n;
    uint8_t rgb[config::kSceneColorsMax][3];
};

// Speeds and palettes picked to show each effect's motion within 6 s.
const Look kLooks[config::kSceneFxCount] = {
    { config::kSceneFxSolid, 4, 0, 2, { { 255, 150, 60 }, { 255, 255, 255 } } },
    { config::kSceneFxChase, 40, 6, 3, { { 255, 40, 40 }, { 40, 255, 80 }, { 60, 90, 255 } } },
    { config::kSceneFxRainbow, 8, 1, 1, { { 255, 255, 255 } } },
    { config::kSceneFxBlobs, 60, 4, 3, { { 0, 90, 255 }, { 255, 0, 140 }, { 0, 255, 160 } } },
    { config::kSceneFxGradient, 30, 2, 3, { { 255, 60, 0 }, { 255, 0, 120 }, { 40, 60, 255 } } },
    { config::kSceneFxFade, 40, 0, 3, { { 255, 30, 30 }, { 30, 255, 90 }, { 40, 80, 255 } } },
    { config::kSceneFxTwinkle, 120, 90, 2, { { 255, 200, 120 }, { 160, 200, 255 } } },
    { config::kSceneFxFire,
      120,
      0,
      4,
      { { 180, 16, 0 }, { 255, 80, 0 }, { 255, 170, 20 }, { 255, 240, 150 } } },
    { config::kSceneFxScanner, 90, 6, 2, { { 255, 0, 0 }, { 0, 160, 255 } } },
    { config::kSceneFxWave, 50, 3, 2, { { 0, 200, 255 }, { 120, 0, 255 } } },
    { config::kSceneFxStripes, 20, 6, 3, { { 255, 255, 255 }, { 255, 0, 60 }, { 0, 80, 255 } } },
};

bool write_ppm(const std::string& path, const std::vector<uint8_t>& rgb) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%u %d\n255\n", kPixels, kRows);
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
        config::Scene s{};
        s.effect = l.fx;
        s.speed  = l.speed;
        s.param  = l.param;
        for (uint8_t k = 0; k < l.n; ++k)
            config::set_scene_color(s, k, l.rgb[k][0], l.rgb[k][1], l.rgb[k][2]);
        s.num_colors = l.n;

        std::vector<uint8_t> img;
        img.reserve(static_cast<size_t>(kPixels) * 3 * kRows);
        uint8_t row[kPixels * 3];
        for (int y = 0; y < kRows; ++y) {
            dmx::logic::fill_scene_pattern(row, sizeof(row), kPixels, 3, s,
                                           static_cast<uint64_t>(y) * kFrameMs + 10'000);
            img.insert(img.end(), row, row + sizeof(row));
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
