# Images & diagrams

Drop the files below into this folder (`docs/img/`) using the **exact names**.

- They render on GitHub (in the `docs/*.md` files) and on the live site
  (`https://lefix2.github.io/pixfrog/`), where `docs/img/` is published as
  `/img/`.
- Nothing breaks while a file is missing: the landing page and docs viewer
  hide any image that fails to load.
- Prefer **PNG** for diagrams/screenshots, **JPG** for photos, **SVG** for the
  logo. Keep widths ≤ ~1600 px.

Status: ✅ added · ⬜ still needed.

| ✓ | File (`docs/img/…`)          | What it should show                                                                                  | Suggested size | Used by |
|---|------------------------------|------------------------------------------------------------------------------------------------------|----------------|---------|
| ✅ | `board-hero.jpg`             | Photo of the ESP32-P4 dev board (Waveshare DEV-KIT)                                                   | 1600×900       | Landing hero + HARDWARE §1 |
| ✅ | `nrz-encoding.png`           | 1-wire NRZ waveform: how a `0` and a `1` bit differ (high time)                                       | 1200×500       | PROTOCOLS §2 |
| ✅ | `clocked-encoding.png`       | SPI-like clocked encoding (APA102/SK9822): DATA aligned to CLOCK edges                                | 1200×500       | PROTOCOLS §4.2 |
| ✅ | `logo.svg`                   | Frog mark used in the nav + favicon                                                                   | square         | Site |
| ⬜ | `og-cover.png`               | Social/link-preview cover: “pixfrog — ESP32-P4 LED firmware” over the board                          | 1200×630       | `og:image` (auto) |
| ⬜ | `architecture-overview.png`  | System block diagram: Ethernet/lwIP → `artnet_rx_task` → `universe_pool` (double-buffer) → `render_task` → LCD_CAM + GDMA → 8 LED channels; UI task + NVS off to the side | 1400×900 | ARCHITECTURE §1 |
| ⬜ | `task-topology.png`          | FreeRTOS tasks mapped to the two cores with priorities (render_task, artnet_rx_task, ui_task, lwIP…) | 1400×800       | ARCHITECTURE §3 |
| ⬜ | `frame-pipeline.png`         | Timeline of one frame: Art-Net packet → pointer swap → encode → GDMA emit → done-semaphore           | 1400×700       | ARCHITECTURE §4 |
| ⬜ | `hardware-pinout.png`        | Board wiring overview: LCD_CAM 16-bit bus, RMII PHY, I²C (OLED + encoder), power                      | 1600×1000      | HARDWARE §2 |
| ⬜ | `level-shifter.png`          | 3.3 V → 5 V level-shifter schematic on the LED DATA/CLOCK lines                                       | 1200×800       | HARDWARE §6 |
| ⬜ | `peripherals-wiring.png`     | Seesaw rotary encoder + SSD1306 OLED on the shared I²C bus                                            | 1200×800       | HARDWARE §7–8 |
| ⬜ | `oled-ui.png`                | Screenshot/mockup of the OLED home screen and one config menu                                        | 1000×600       | HARDWARE §8 / landing |
| ⬜ | `clock-tree.png`             | LCD_CAM clock tree: source → divider → PCLK, with the pixfrog PCLK choice                             | 1200×700       | PROTOCOLS §3.1 |

## Embedding in the docs

`board-hero.jpg` and `og-cover.png` are wired automatically. For the diagrams,
add a line where you want it in the matching `docs/*.md` file — paths are
relative to `docs/`, so they work both on GitHub and on the site:

```markdown
![System overview](img/architecture-overview.png)
```
