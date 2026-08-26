# Images & diagrams

All figures referenced by the docs and the site live here. They render on
GitHub (in the `docs/*.md` files) and on the published site
(`https://lefix2.github.io/pixfrog/`), where `docs/img/` is served as `/img/`.

The diagrams are **hand-authored SVG** (crisp, versionable, no licensing
issues); photos are JPG and the social cover is a PNG. The landing page and
docs viewer hide any image that fails to load, so adding/replacing one is safe.

| File (`docs/img/…`)         | Kind | Shows                                                            | Used by |
|-----------------------------|------|-----------------------------------------------------------------|---------|
| `architecture-overview.svg` | SVG  | System block diagram (ingest → pool → render → PARLIO TX → strips) | ARCHITECTURE §1 |
| `task-topology.svg`         | SVG  | FreeRTOS tasks across the two cores, with priorities            | ARCHITECTURE §3 |
| `frame-pipeline.svg`        | SVG  | One frame's render stages, GDMA + network in parallel          | ARCHITECTURE §4 |
| `clock-tree.svg`            | SVG  | LCD_CAM clock tree + PCLK / bit / clock formulas               | PROTOCOLS §3.1 |
| `nrz-encoding.png`          | PNG  | 1-wire NRZ bit timing (T0H / T1H / TRESET)                     | PROTOCOLS §2 |
| `clocked-encoding.png`      | PNG  | SPI-like clocked encoding (DATA on CLOCK edges)               | PROTOCOLS §4.2 |
| `hardware-pinout.jpg`       | JPG  | Waveshare DEV-KIT labelled header + interfaces                 | HARDWARE §2 |
| `level-shifter.svg`         | SVG  | 3.3 V → 5 V buffering with a 74HCT245, series R + TVS clamp     | HARDWARE §3 |
| `peripherals-wiring.svg`    | SVG  | OLED + seesaw encoder on the shared I²C bus                    | HARDWARE §4 |
| `oled-ui.svg`               | SVG  | SSD1306 home-screen mockup                                     | HARDWARE §6 |
| `board-hero.jpg`            | JPG  | Photo of the ESP32-P4 DEV-KIT                                  | HARDWARE §1 |
| `ui-home.png`               | PNG  | NV3007 bar display home screen: IP, services, the 8 channels   | Landing page §02 |
| `rack-closeup.jpg`          | JPG  | 1U rack front panel — screen and rotary encoder                | Landing hero |
| `rack-open.jpg`             | JPG  | Rack with the lid off: daughterboard, supply, vent panels      | Landing page §01 |
| `rack-full.jpg`             | JPG  | Complete 1U rack, eight XLR outputs across the front           | Landing page §01 |
| `web-dashboard.png`         | PNG  | Embedded web UI dashboard                                      | Landing page §02 |
| `web-channels.png`          | PNG  | Embedded web UI channel patch page                             | Landing page §02 |
| `og-cover.png`              | PNG  | Social / link-preview cover (rendered from `og-cover.svg`)    | `og:image` |
| `logo.svg`                  | SVG  | Frog mark — nav brand + favicon                               | Site |
| `frog-anim.svg`             | SVG  | Animated frog logo (self-contained CSS); baked into the splash | About page + `tools/splashgen` |

`*.svg` sources for the rasterised covers (`og-cover.svg`) are kept alongside
so they can be re-exported with `rsvg-convert`.

The landing hero is cropped with `object-fit: cover` at `object-position: 56% 50%`
under a left-to-right dark gradient, so it wants a wide frame with the subject
right of centre and room to go dark on the left. It ships as JPG (77 kB rather
than 450 kB as PNG): the render's smooth shading hides the compression, and it
sits under a heavy scrim anyway. The 3D renders and UI captures are exported at
2×; downscale to 1920 (hero) / 1600 (renders) / 1440 (UI) before committing.
