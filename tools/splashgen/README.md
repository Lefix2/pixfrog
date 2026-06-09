# splashgen

Bakes the animated **Collecti'Frog logo**
[`docs/img/frog-anim.svg`](../../docs/img/frog-anim.svg) into the boot-splash
frame strip consumed by `components/ui/src/splash.cpp`. The same SVG is embedded
by the web About page (`.github/pages/about.html`), so the two never drift.

The logo is a static SVG (frog head + eye rings + pupils + 4 water bands)
animated purely by CSS: the creature rises out of the water, the bands pop in
and idle-bob, the pupils blink. We can't run CSS on the MCU, so this tool
replays that timeline at generation time — per frame it applies each group's
affine transform to the parsed bezier control points, rasterises (supersampled,
via vendored nanosvg), composites the water-clipped frog under the waves,
downsamples and thresholds to **1bpp**. The firmware then blits frame *t* with
`canvas_draw_mask` (saffron ink, dark-green backdrop).

The output `splash_anim.cpp` is **generated**, not hand-edited — it carries a
banner saying so. Regenerate it instead of touching it.

## Build & run

```bash
cd tools/splashgen && cmake -B build && cmake --build build
./build/splashgen ../../docs/img/frog-anim.svg \
    ../../components/ui/src/splash_anim.cpp        # [outW] [fps] [durMs] are tunable
```

Defaults bake **43 frames, 140×99, @12 fps** over a 3600 ms timeline (≈75 KB of
1bpp mask data). The tool extracts the first `<svg>…</svg>` from its input, so it
also accepts an HTML page that embeds the logo.

| arg     | meaning                                                       |
|---------|---------------------------------------------------------------|
| `outW`  | output frame width in px; height follows the window aspect    |
| `fps`   | playback rate baked into the strip (`splash_anim_frame_ms()`) |
| `durMs` | total animation length to sample (frame count = durMs / fps)  |

Quick visual check without writing the table: pass `preview` as the output path
to dump a late frame as terminal ASCII shading.

```bash
./build/splashgen ../../docs/img/frog-anim.svg preview
```

The scene window, water-clip line and supersample factor are `constexpr` at the
top of `splashgen.cpp`; the shape→group mapping in `classify()` assumes the SVG
element order (head 5, eye-rings 2, pupils 2×2, waves 4 = 15 shapes) and the tool
aborts if that count changes.

After regenerating, rebuild the emulator to preview the real splash:
`cd ../emulator && cmake --build build && ./build/pixfrog_emu` — or headless,
`echo "splash 2400 /tmp/s.bmp" | ./build/pixfrog_emu --headless`.

`nanosvg.h` / `nanosvgrast.h` are vendored here (zlib licence).
