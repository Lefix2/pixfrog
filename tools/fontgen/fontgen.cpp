// fontgen — rasterise a TTF into the pixfrog UI alpha font table.
//
// Emits a C++ source defining `kFontAlpha[95][kFontCellWidth*kFontHeight]`:
// one 8-bit coverage (anti-aliased) value per pixel of a fixed cell, row-major,
// for printable ASCII 0x20..0x7E. The UI renderer blends fg/bg by this coverage
// on the colour TFT and thresholds it to 1bpp on the mono OLED.
//
//   fontgen <font.ttf> <out.cpp> [px=10.0] [baseline=6] [xpad=0] [gain=1.2]
//
// px       nominal pixel height handed to stb (tune so caps fill ~rows 0..6)
// baseline cell row of the glyph baseline (descenders fall below it)
// xpad     extra left inset inside the visible glyph box
// gain     coverage multiplier — thin stems at this size cap well below 255,
//          so a mild boost keeps the text from looking washed out

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

// Must match components/ui/src/font.h.
constexpr int kFirst    = 0x20;
constexpr int kLast     = 0x7E;
constexpr int kCount    = kLast - kFirst + 1;  // 95
constexpr int kCellW    = 6;                   // advance (5 ink cols + 1 spacing)
constexpr int kCellH    = 8;
constexpr int kInkW     = 5;  // visible columns; column 5 stays blank for spacing
constexpr int kCellArea = kCellW * kCellH;

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <font.ttf> <out.cpp> [px] [baseline] [xpad] [gain]\n",
                     argv[0]);
        return 2;
    }
    const char* ttf_path = argv[1];
    const char* out_path = argv[2];
    const float px       = (argc > 3) ? std::atof(argv[3]) : 10.0f;
    const int baseline   = (argc > 4) ? std::atoi(argv[4]) : 6;
    const int xpad       = (argc > 5) ? std::atoi(argv[5]) : 0;
    const float gain     = (argc > 6) ? std::atof(argv[6]) : 1.2f;

    std::FILE* f = std::fopen(ttf_path, "rb");
    if (!f) {
        std::fprintf(stderr, "fontgen: cannot open %s\n", ttf_path);
        return 1;
    }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> ttf(sz);
    if (std::fread(ttf.data(), 1, sz, f) != static_cast<size_t>(sz)) {
        std::fprintf(stderr, "fontgen: short read on %s\n", ttf_path);
        return 1;
    }
    std::fclose(f);

    stbtt_fontinfo font;
    if (!stbtt_InitFont(&font, ttf.data(), stbtt_GetFontOffsetForIndex(ttf.data(), 0))) {
        std::fprintf(stderr, "fontgen: stbtt_InitFont failed\n");
        return 1;
    }
    const float scale = stbtt_ScaleForPixelHeight(&font, px);

    // cells[glyph][row*kCellW + col] = coverage 0..255
    std::vector<uint8_t> cells(static_cast<size_t>(kCount) * kCellArea, 0);

    for (int ci = 0; ci < kCount; ++ci) {
        const int cp = kFirst + ci;
        int w = 0, h = 0, xoff = 0, yoff = 0;
        unsigned char* bmp = stbtt_GetCodepointBitmap(&font, 0, scale, cp, &w, &h, &xoff, &yoff);
        uint8_t* cell      = &cells[static_cast<size_t>(ci) * kCellArea];
        if (bmp) {
            // Centre the ink box horizontally in the 5px visible field; place
            // vertically by the glyph baseline (yoff is top-of-bitmap → baseline).
            const int x0 = xpad + (kInkW - w) / 2;
            const int y0 = baseline + yoff;
            for (int gy = 0; gy < h; ++gy) {
                const int cy = y0 + gy;
                if (cy < 0 || cy >= kCellH) continue;
                for (int gx = 0; gx < w; ++gx) {
                    const int cx = x0 + gx;
                    if (cx < 0 || cx >= kInkW) continue;
                    const int v            = static_cast<int>(bmp[gy * w + gx] * gain + 0.5f);
                    cell[cy * kCellW + cx] = static_cast<uint8_t>(v > 255 ? 255 : v);
                }
            }
            stbtt_FreeBitmap(bmp, nullptr);
        }
    }

    // Debug: if out_path looks like "preview:TEXT", render TEXT as terminal
    // ASCII shading instead of writing a source file. Lets metrics be tuned fast.
    if (std::strncmp(out_path, "preview:", 8) == 0) {
        const char* text = out_path + 8;
        const char* ramp = " .:-=+*#%@";
        for (int row = 0; row < kCellH; ++row) {
            for (const char* t = text; *t; ++t) {
                const int gi = static_cast<unsigned char>(*t) - kFirst;
                if (gi < 0 || gi >= kCount) {
                    std::printf("......");
                    continue;
                }
                const uint8_t* cell = &cells[static_cast<size_t>(gi) * kCellArea];
                for (int col = 0; col < kCellW; ++col)
                    std::printf("%c", ramp[cell[row * kCellW + col] * 9 / 255]);
            }
            std::printf("\n");
        }
        return 0;
    }

    std::FILE* o = std::fopen(out_path, "wb");
    if (!o) {
        std::fprintf(stderr, "fontgen: cannot write %s\n", out_path);
        return 1;
    }
    std::fprintf(o,
                 "// AUTO-GENERATED by tools/fontgen — DO NOT EDIT.\n"
                 "// source: %s  (px=%.1f baseline=%d xpad=%d gain=%.2f)\n"
                 "// 8-bit coverage, row-major, %dx%d cell, ASCII 0x%02X..0x%02X.\n"
                 "// Regenerate: see tools/fontgen/README.md\n\n"
                 "#include \"font.h\"\n\n"
                 "namespace pixfrog::ui::detail {\n\n"
                 "// clang-format off\n"
                 "const uint8_t kFontAlpha[%d][kFontCellWidth * kFontHeight] = {\n",
                 ttf_path, px, baseline, xpad, gain, kCellW, kCellH, kFirst, kLast, kCount);

    for (int ci = 0; ci < kCount; ++ci) {
        const int cp     = kFirst + ci;
        const char label = (cp >= 0x20 && cp <= 0x7E) ? static_cast<char>(cp) : '?';
        std::fprintf(o, "    {");
        const uint8_t* cell = &cells[static_cast<size_t>(ci) * kCellArea];
        for (int k = 0; k < kCellArea; ++k) {
            std::fprintf(o, "%3u%s", cell[k], (k + 1 < kCellArea) ? "," : "");
        }
        std::fprintf(o, "},  // 0x%02X '%c'\n", cp, label);
    }

    std::fprintf(o, "};\n"
                    "// clang-format on\n\n"
                    "// Fallback for out-of-range chars: solid outlined box.\n"
                    "static const uint8_t kFallbackAlpha[kFontCellWidth * kFontHeight] = {\n"
                    "    255, 255, 255, 255, 255, 0, 255, 0, 0, 0, 255, 0,\n"
                    "    255, 0, 0, 0, 255, 0, 255, 0, 0, 0, 255, 0,\n"
                    "    255, 0, 0, 0, 255, 0, 255, 0, 0, 0, 255, 0,\n"
                    "    255, 255, 255, 255, 255, 0, 0, 0, 0, 0, 0, 0,\n"
                    "};\n\n"
                    "const uint8_t* font_alpha_for(char c) {\n"
                    "    const uint8_t uc = static_cast<uint8_t>(c);\n"
                    "    if (uc < static_cast<uint8_t>(kFontFirstChar) ||\n"
                    "        uc > static_cast<uint8_t>(kFontLastChar)) {\n"
                    "        return kFallbackAlpha;\n"
                    "    }\n"
                    "    return kFontAlpha[uc - static_cast<uint8_t>(kFontFirstChar)];\n"
                    "}\n\n"
                    "}  // namespace pixfrog::ui::detail\n");
    std::fclose(o);
    std::printf("fontgen: wrote %s (%d glyphs, %dx%d cell)\n", out_path, kCount, kCellW, kCellH);
    return 0;
}
