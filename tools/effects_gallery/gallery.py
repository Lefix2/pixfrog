#!/usr/bin/env python3
"""Effects gallery: every generator, and the layers an effect can carry over
one (dimmer phaser, invert, Block / Groups / Wings), as space-time strips from
the firmware's own renderer (fill_effect_run), for the docs and the site.

    tools/effects_gallery/gallery.py                 # → docs/img/effects/{effects,layers}-gallery.png
    tools/effects_gallery/gallery.py --strips        # + one PNG per strip

x = time going right (6 s at 40 fps), y = pixel along a 144-px line. Writes
the two labelled sheets (--strips: one PNG per strip too). Needs a C++17
compiler and pillow.
"""
import argparse
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
COMP = os.path.join(REPO, "components")
SCALE_X, SCALE_Y = 2, 2  # 240×144 strips → 480×288


def build(tmp):
    exe = os.path.join(tmp, "gallery")
    src = [os.path.join(HERE, "gallery.cpp")] + [
        os.path.join(COMP, "led_protocols", "src", f)
        for f in ("led_protocols.cpp", "encoder_nrz.cpp", "encoder_spi.cpp")
    ]
    inc = [os.path.join(COMP, d) for d in (
        "dmx_manager/include", "dmx_manager/src", "led_protocols/include",
        "led_protocols/src", "config_store/include")]
    cmd = [os.environ.get("CXX", "c++"), "-std=c++17", "-O2", "-fno-exceptions", "-fno-rtti",
           "-o", exe, *src] + [f"-I{i}" for i in inc]
    subprocess.run(cmd, check=True)
    return exe


def main():
    from PIL import Image, ImageDraw, ImageFont

    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", default=os.path.join(REPO, "docs", "img", "effects"))
    ap.add_argument("--strips", action="store_true", help="one PNG per effect as well")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    try:
        font = ImageFont.truetype("DejaVuSansMono.ttf", 16)
    except OSError:
        font = ImageFont.load_default()

    def sheet(tmp, exe, mode, stem):
        """One labelled grid, on the site's dark background."""
        names = subprocess.run([exe, tmp, *mode], check=True, capture_output=True,
                               text=True).stdout.split()
        strips = []
        for name in names:
            img = Image.open(os.path.join(tmp, name + ".ppm")).convert("RGB")
            img = img.resize((img.width * SCALE_X, img.height * SCALE_Y), Image.NEAREST)
            strips.append((name, img))
            if args.strips:
                path = os.path.join(args.out, f"{name}.png")
                img.save(path, optimize=True)
                print("wrote", os.path.relpath(path, REPO))
        cols, pad, label_h = 4, 18, 30
        w, h = strips[0][1].size
        rows = (len(strips) + cols - 1) // cols
        out = Image.new("RGB", (cols * (w + pad) + pad, rows * (h + label_h + pad) + pad),
                        (12, 13, 12))
        draw = ImageDraw.Draw(out)
        for i, (name, img) in enumerate(strips):
            x = pad + (i % cols) * (w + pad)
            y = pad + (i // cols) * (h + label_h + pad)
            draw.text((x, y + 4), name.replace("_", " ").capitalize(), fill=(237, 238, 234),
                      font=font)
            out.paste(img, (x, y + label_h))
        path = os.path.join(args.out, stem + ".png")
        out.save(path, optimize=True)
        print("wrote", os.path.relpath(path, REPO))

    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp)
        sheet(tmp, exe, [], "effects-gallery")
        sheet(tmp, exe, ["layers"], "layers-gallery")
    return 0


if __name__ == "__main__":
    sys.exit(main())
