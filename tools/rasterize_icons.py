#!/usr/bin/env python3
"""Rasterize the header status-icon SVGs into 8bpp alpha-mask C arrays.

Each SVG in components/ui/icons/ is rendered at 4x (supersampled) and Lanczos-
downsampled to 16x16, then its coverage (alpha) is emitted as a 256-byte array
in components/ui/src/icons_status.h. The UI composites these masks in any theme
colour via canvas_draw_mask_aa (see the StatusStrip component in menu.cpp).

Deps: cairosvg, Pillow.  Run:  python3 tools/rasterize_icons.py
"""
import io
import os
import glob

import cairosvg
from PIL import Image

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "components", "ui", "icons")
OUT = os.path.join(REPO, "components", "ui", "src", "icons_status.h")
SIZE = 16
SS = 4  # supersample factor


def rasterize(path):
    svg = open(path, "rb").read()
    png = cairosvg.svg2png(bytestring=svg, output_width=SIZE * SS, output_height=SIZE * SS)
    im = Image.open(io.BytesIO(png)).convert("RGBA")
    a = im.split()[3].resize((SIZE, SIZE), Image.LANCZOS)
    return list(a.tobytes())


def emit(f, name, data):
    f.write(f"static const uint8_t kIcon_{name}[{SIZE * SIZE}] = {{\n")
    for row in range(SIZE):
        vals = data[row * SIZE:(row + 1) * SIZE]
        f.write("    " + ", ".join(f"0x{v:02X}" for v in vals) + ",\n")
    f.write("};\n")


def main():
    svgs = sorted(glob.glob(os.path.join(SRC, "*.svg")))
    with open(OUT, "w") as f:
        f.write("// Auto-generated 8bpp alpha masks (16x16, row-major) from "
                "components/ui/icons/*.svg.\n")
        f.write("// Anti-aliased via 4x supersampling + Lanczos downsample.\n")
        f.write("// Regenerate via tools/rasterize_icons.py — do not hand-edit.\n")
        f.write("#pragma once\n#include <stdint.h>\n\n")
        for path in svgs:
            name = os.path.splitext(os.path.basename(path))[0]
            emit(f, name, rasterize(path))
    print(f"wrote {OUT} from {len(svgs)} svg(s)")


if __name__ == "__main__":
    main()
