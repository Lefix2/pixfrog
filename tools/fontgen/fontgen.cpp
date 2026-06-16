// fontgen — rasterise a TTF into the pixfrog UI alpha font tables.
//
// Emits a C++ source defining two coverage tables:
//   kFontAlpha[95][kFontCellWidth*kFontHeight]            — 6×8, all builds
//   kFontLargeAlpha[95][kFontLargeCellWidth*kFontLargeHeight] — 12×16, TFT only
//
// One 8-bit coverage (anti-aliased) value per pixel of a fixed cell, row-major,
// for printable ASCII 0x20..0x7E. The UI renderer blends fg/bg by this coverage
// on the colour TFT and thresholds it to 1bpp on the mono OLED. The large cell
// is exactly 2× the small one so the TFT can swap it in for even text scales
// without touching any layout maths — natively rasterised 12×16 glyphs are far
// crisper than pixel-doubling the 6×8 cell.
//
//   fontgen <font.ttf> <out.cpp> [px=10.0] [baseline=6] [xpad=0] [gain=1.2]
//                                [px_l=19.0] [baseline_l=12] [gain_l=1.1]
//
// px / px_l    nominal pixel height handed to stb (tune so caps fill the cell)
// baseline(_l) cell row of the glyph baseline (descenders fall below it)
// xpad         extra left inset inside the visible glyph box (small cell only)
// gain(_l)     coverage multiplier — thin stems cap well below 255 at small
//              sizes, so a boost keeps the text from looking washed out

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
constexpr int kFirst = 0x20;
constexpr int kLast  = 0x7E;
constexpr int kCount = kLast - kFirst + 1;  // 95

struct CellGeom {
    int cell_w;  // advance (ink + spacing column)
    int cell_h;
    int ink_w;  // visible columns; the rest stays blank for spacing
};

constexpr CellGeom kSmall{ 6, 8, 5 };
constexpr CellGeom kLarge{ 12, 16, 11 };
constexpr CellGeom kXL{ 18, 24, 16 };  // splash wordmark only

// cells[glyph][row*cell_w + col] = coverage 0..255
std::vector<uint8_t> rasterize(const stbtt_fontinfo& font, const CellGeom& g, float px,
                               int baseline, int xpad, float gain) {
    const float scale = stbtt_ScaleForPixelHeight(&font, px);
    const int area    = g.cell_w * g.cell_h;
    std::vector<uint8_t> cells(static_cast<size_t>(kCount) * area, 0);

    for (int ci = 0; ci < kCount; ++ci) {
        const int cp = kFirst + ci;
        int w = 0, h = 0, xoff = 0, yoff = 0;
        unsigned char* bmp = stbtt_GetCodepointBitmap(&font, 0, scale, cp, &w, &h, &xoff, &yoff);
        uint8_t* cell      = &cells[static_cast<size_t>(ci) * area];
        if (bmp) {
            // Centre the ink box horizontally in the visible field; place
            // vertically by the glyph baseline (yoff is top-of-bitmap → baseline).
            const int x0 = xpad + (g.ink_w - w) / 2;
            const int y0 = baseline + yoff;
            for (int gy = 0; gy < h; ++gy) {
                const int cy = y0 + gy;
                if (cy < 0 || cy >= g.cell_h) continue;
                for (int gx = 0; gx < w; ++gx) {
                    const int cx = x0 + gx;
                    if (cx < 0 || cx >= g.ink_w) continue;
                    const int v              = static_cast<int>(bmp[gy * w + gx] * gain + 0.5f);
                    cell[cy * g.cell_w + cx] = static_cast<uint8_t>(v > 255 ? 255 : v);
                }
            }
            stbtt_FreeBitmap(bmp, nullptr);
        }
    }
    return cells;
}

void preview(const std::vector<uint8_t>& cells, const CellGeom& g, const char* text) {
    const char* ramp = " .:-=+*#%@";
    const int area   = g.cell_w * g.cell_h;
    for (int row = 0; row < g.cell_h; ++row) {
        for (const char* t = text; *t; ++t) {
            const int gi = static_cast<unsigned char>(*t) - kFirst;
            if (gi < 0 || gi >= kCount) {
                for (int col = 0; col < g.cell_w; ++col)
                    std::printf(".");
                continue;
            }
            const uint8_t* cell = &cells[static_cast<size_t>(gi) * area];
            for (int col = 0; col < g.cell_w; ++col)
                std::printf("%c", ramp[cell[row * g.cell_w + col] * 9 / 255]);
        }
        std::printf("\n");
    }
}

void emit_table(std::FILE* o, const std::vector<uint8_t>& cells, const CellGeom& g,
                const char* name, const char* dims) {
    const int area = g.cell_w * g.cell_h;
    std::fprintf(o, "const uint8_t %s[%d][%s] = {\n", name, kCount, dims);
    for (int ci = 0; ci < kCount; ++ci) {
        const int cp     = kFirst + ci;
        const char label = (cp >= 0x20 && cp <= 0x7E) ? static_cast<char>(cp) : '?';
        std::fprintf(o, "    {");
        const uint8_t* cell = &cells[static_cast<size_t>(ci) * area];
        for (int k = 0; k < area; ++k) {
            std::fprintf(o, "%3u%s", cell[k], (k + 1 < area) ? "," : "");
        }
        std::fprintf(o, "},  // 0x%02X '%c'\n", cp, label);
    }
    std::fprintf(o, "};\n");
}

// Fallback for out-of-range chars: solid outlined box over the ink field.
void emit_fallback(std::FILE* o, const CellGeom& g, const char* name, const char* dims) {
    std::fprintf(o, "static const uint8_t %s[%s] = {\n", name, dims);
    const int box_h = g.cell_h / 2 + g.cell_h / 4;  // leave the descender rows clear
    for (int row = 0; row < g.cell_h; ++row) {
        std::fprintf(o, "    ");
        for (int col = 0; col < g.cell_w; ++col) {
            const bool edge = row < box_h && col < g.ink_w &&
                              (row == 0 || row == box_h - 1 || col == 0 || col == g.ink_w - 1);
            std::fprintf(o, "%3u%s", edge ? 255 : 0, (col + 1 < g.cell_w) ? "," : ",\n");
        }
    }
    std::fprintf(o, "};\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: %s <font.ttf> <out.cpp> [px] [baseline] [xpad] [gain]"
                     " [px_l] [baseline_l] [gain_l]\n",
                     argv[0]);
        return 2;
    }
    const char* ttf_path  = argv[1];
    const char* out_path  = argv[2];
    const float px        = (argc > 3) ? std::atof(argv[3]) : 10.0f;
    const int baseline    = (argc > 4) ? std::atoi(argv[4]) : 6;
    const int xpad        = (argc > 5) ? std::atoi(argv[5]) : 0;
    const float gain      = (argc > 6) ? std::atof(argv[6]) : 1.2f;
    const float px_l      = (argc > 7) ? std::atof(argv[7]) : 19.0f;
    const int baseline_l  = (argc > 8) ? std::atoi(argv[8]) : 12;
    const float gain_l    = (argc > 9) ? std::atof(argv[9]) : 1.1f;
    const float px_xl     = (argc > 10) ? std::atof(argv[10]) : 26.0f;
    const int baseline_xl = (argc > 11) ? std::atoi(argv[11]) : 19;
    const float gain_xl   = (argc > 12) ? std::atof(argv[12]) : 1.05f;

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

    const auto small = rasterize(font, kSmall, px, baseline, xpad, gain);
    const auto large = rasterize(font, kLarge, px_l, baseline_l, 0, gain_l);
    const auto xl    = rasterize(font, kXL, px_xl, baseline_xl, 0, gain_xl);

    // Debug: if out_path looks like "preview:TEXT", render TEXT as terminal
    // ASCII shading instead of writing a source file. Lets metrics be tuned fast.
    if (std::strncmp(out_path, "preview:", 8) == 0) {
        const char* text = out_path + 8;
        preview(small, kSmall, text);
        std::printf("\n");
        preview(large, kLarge, text);
        return 0;
    }

    std::FILE* o = std::fopen(out_path, "wb");
    if (!o) {
        std::fprintf(stderr, "fontgen: cannot write %s\n", out_path);
        return 1;
    }
    std::fprintf(
        o,
        "// AUTO-GENERATED by tools/fontgen — DO NOT EDIT.\n"
        "// source: %s  (px=%.1f baseline=%d xpad=%d gain=%.2f"
        " px_l=%.1f baseline_l=%d gain_l=%.2f"
        " px_xl=%.1f baseline_xl=%d gain_xl=%.2f)\n"
        "// 8-bit coverage, row-major, %dx%d + %dx%d + %dx%d cells, ASCII 0x%02X..0x%02X.\n"
        "// Regenerate: see tools/fontgen/README.md\n\n"
        "#include \"font.h\"\n\n"
        "namespace pixfrog::ui::detail {\n\n"
        "// clang-format off\n",
        ttf_path, px, baseline, xpad, gain, px_l, baseline_l, gain_l, px_xl, baseline_xl, gain_xl,
        kSmall.cell_w, kSmall.cell_h, kLarge.cell_w, kLarge.cell_h, kXL.cell_w, kXL.cell_h, kFirst,
        kLast);

    emit_table(o, small, kSmall, "kFontAlpha", "kFontCellWidth * kFontHeight");
    std::fprintf(o, "\n");
    emit_fallback(o, kSmall, "kFallbackAlpha", "kFontCellWidth * kFontHeight");
    std::fprintf(o, "\n"
                    "const uint8_t* font_alpha_for(char c) {\n"
                    "    const uint8_t uc = static_cast<uint8_t>(c);\n"
                    "    if (uc < static_cast<uint8_t>(kFontFirstChar) ||\n"
                    "        uc > static_cast<uint8_t>(kFontLastChar)) {\n"
                    "        return kFallbackAlpha;\n"
                    "    }\n"
                    "    return kFontAlpha[uc - static_cast<uint8_t>(kFontFirstChar)];\n"
                    "}\n\n"
                    "// The 12x16 cell ships only on the TFT build — the OLED never scales\n"
                    "// text and would carry the table as dead flash.\n"
                    "#ifdef CONFIG_PIXFROG_DISPLAY_TFT\n");
    emit_table(o, large, kLarge, "kFontLargeAlpha", "kFontLargeCellWidth * kFontLargeHeight");
    std::fprintf(o, "\n");
    emit_fallback(o, kLarge, "kFallbackLargeAlpha", "kFontLargeCellWidth * kFontLargeHeight");
    std::fprintf(o, "\n"
                    "const uint8_t* font_large_alpha_for(char c) {\n"
                    "    const uint8_t uc = static_cast<uint8_t>(c);\n"
                    "    if (uc < static_cast<uint8_t>(kFontFirstChar) ||\n"
                    "        uc > static_cast<uint8_t>(kFontLastChar)) {\n"
                    "        return kFallbackLargeAlpha;\n"
                    "    }\n"
                    "    return kFontLargeAlpha[uc - static_cast<uint8_t>(kFontFirstChar)];\n"
                    "}\n\n");
    emit_table(o, xl, kXL, "kFontXLAlpha", "kFontXLCellWidth * kFontXLHeight");
    std::fprintf(o, "\n");
    emit_fallback(o, kXL, "kFallbackXLAlpha", "kFontXLCellWidth * kFontXLHeight");
    std::fprintf(o, "\n"
                    "const uint8_t* font_xl_alpha_for(char c) {\n"
                    "    const uint8_t uc = static_cast<uint8_t>(c);\n"
                    "    if (uc < static_cast<uint8_t>(kFontFirstChar) ||\n"
                    "        uc > static_cast<uint8_t>(kFontLastChar)) {\n"
                    "        return kFallbackXLAlpha;\n"
                    "    }\n"
                    "    return kFontXLAlpha[uc - static_cast<uint8_t>(kFontFirstChar)];\n"
                    "}\n"
                    "#endif  // CONFIG_PIXFROG_DISPLAY_TFT\n"
                    "// clang-format on\n\n"
                    "}  // namespace pixfrog::ui::detail\n");
    std::fclose(o);
    std::printf("fontgen: wrote %s (%d glyphs, %dx%d + %dx%d cells)\n", out_path, kCount,
                kSmall.cell_w, kSmall.cell_h, kLarge.cell_w, kLarge.cell_h);
    return 0;
}
