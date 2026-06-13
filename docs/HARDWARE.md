# pixfrog hardware

Target board for v0: **Waveshare ESP32-P4 Module DEV-KIT**.

Full schematic, pinout, datasheet and accessory documentation:

→ <https://docs.waveshare.com/ESP32-P4-Module-DEV-KIT/Resources-And-Documents>

All pin assignments in this document are exposed on the user-facing header connector documented by Waveshare; nothing collides with the internal Ethernet PHY (IP101GRI), PSRAM, or flash signals.

---

## 1. Board overview

![Waveshare ESP32-P4 Module DEV-KIT — the default pixfrog board](img/board-hero.jpg)

- ESP32-P4 dual-core RISC-V @ 400 MHz
- 32 MB octal PSRAM
- 16 MB external flash
- **IP101GRI Ethernet PHY** with onboard magnetic RJ45 jack (RMII pre-wired)
- USB-C OTG + UART debug bridge
- Header connector exposes the user GPIOs in the layout shown below

External components added for pixfrog:

- 1 × SSD1306 I2C OLED 128×64
- 1 × Adafruit seesaw QT rotary encoder #4991
- 2 × 74HCT245 level shifters (3.3 V → 5 V) — one per group of 8 GPIOs
- 8 × strip output connectors (DATA, CLOCK, GND)

---

## 2. Pinout

![Waveshare ESP32-P4 Module DEV-KIT — labelled header and interfaces](img/hardware-pinout.jpg)

Validated against the Waveshare ESP32-P4 Module DEV-KIT schematic (P6 40-pin header). Strapping pins latched at reset (ESP32-P4 datasheet §3.3): **GPIO 35** (boot/download select, driven by the on-board BOOT button — also doubles as RMII TXD1), **GPIO 36** (must read HIGH for a reliable serial-download boot — exposed on P6 pin 21, keep it free), **GPIO 34** (JTAG source select). None of the LED pins below land on a strapping pin. GPIO 16 is not exposed on P6 and must not be used.

### 2.1 LED 16-bit parallel bus (PARLIO TX / LCD_CAM)

ESP32-P4 routes every peripheral signal through the GPIO matrix, including the 16 data lines of the parallel LED bus. Any free GPIO is a valid bus data line — the choices below pick header pins that are easy to route in pairs. The bus is driven by the default **PARLIO TX** backend or the legacy **LCD_CAM** RGB backend (Kconfig choice); both clock the same 16 GPIOs from PSRAM via GDMA and emit identical sample streams (see §3 and `docs/PROTOCOLS.md` §3).

| Bus bit | Signal    | GPIO | Notes                       |
|--------:|-----------|-----:|-----------------------------|
| 0       | CH1_DATA  | 2    | strip 1 data                |
| 1       | CH1_CLOCK | 3    | strip 1 clock (clocked LEDs)|
| 2       | CH2_DATA  | 4    |                             |
| 3       | CH2_CLOCK | 5    |                             |
| 4       | CH3_DATA  | 22   |                             |
| 5       | CH3_CLOCK | 23   |                             |
| 6       | CH4_DATA  | 24   |                             |
| 7       | CH4_CLOCK | 25   |                             |
| 8       | CH5_DATA  | 26   |                             |
| 9       | CH5_CLOCK | 46   | P6 pin 33 (adj. to CH5_DATA)|
| 10      | CH6_DATA  | 32   |                             |
| 11      | CH6_CLOCK | 33   |                             |
| 12      | CH7_DATA  | 47   |                             |
| 13      | CH7_CLOCK | 48   |                             |
| 14      | CH8_DATA  | 53   |                             |
| 15      | CH8_CLOCK | 54   |                             |

> The bus clock (PCLK) pin stays internal on both backends — LCD_CAM with `pclk_gpio_num = -1`, PARLIO with `clk_out_gpio_num = GPIO_NUM_NC`. CLOCK signals for clocked LED protocols (APA102, …) are encoded as bits 1, 3, 5, … of the parallel sample stream, not derived from PCLK. See `docs/PROTOCOLS.md` §3.

### 2.2 Ethernet RMII (internal)

The PHY's RMII + management pins use fixed SoC GPIOs that are wired directly on the DEV-KIT (verified against the schematic, PHY U7 = IP101GRI) and are not brought out to the P6 header, so they impose no constraint on the LED bus pinout above. The RMII data/clock pins equal the IDF v5.5 `ETH_ESP32_EMAC_DEFAULT_CONFIG()` defaults, which match the board wiring — so `main/init_network()` only overrides MDC / MDIO / reset:

| Signal RMII   | GPIO P4 | Note                                  |
|---------------|--------:|---------------------------------------|
| EMAC_REF_CLK  | 50      | 50 MHz, fixed                         |
| EMAC_TX_EN    | 49      | fixed                                 |
| EMAC_TXD0     | 34      | fixed (also a boot strap)             |
| EMAC_TXD1     | 35      | fixed (also the BOOT-button strap)    |
| EMAC_RXD0     | 29      | fixed                                 |
| EMAC_RXD1     | 30      | fixed                                 |
| EMAC_CRS_DV   | 28      | fixed                                 |
| MDC           | 31      | U7 pin 22                             |
| MDIO          | 52      | U7 pin 23                             |
| PHY reset     | 51      | U7 pin 32 — drive HIGH to release     |
| PHY address   | 0x01    | IP101GRI strap (AD0/AD3)              |

> The old MDC=29 / MDIO=30 values were carried over from the Espressif Function EV Board and never link on the Waveshare DEV-KIT — GPIO 29/30 are actually the RMII RX data lines there. Use the values above.

### 2.3 I2C (OLED + encoder)

| Signal | GPIO | Note                                             |
|--------|-----:|--------------------------------------------------|
| SDA    | 7    | `SDA(GPIO7)` on the header silkscreen           |
| SCL    | 8    | `SCL(GPIO8)` on the header silkscreen           |

I2C addresses:

- SSD1306: `0x3C` or `0x3D` (depending on module)
- seesaw 4991: `0x36` (factory default)

Bus frequency: **400 kHz** (Fast Mode). Enough for OLED refresh at ~10 Hz with diff-based flushing.

### 2.4 Miscellaneous

| Signal       | GPIO | Note                                       |
|--------------|-----:|--------------------------------------------|
| STATUS_LED   | 1    | optional external heartbeat                |
| DEBUG_TX     | 37   | UART0 console — `TXD(GPIO37)` on silkscreen |
| DEBUG_RX     | 38   | UART0 — `RXD(GPIO38)`                       |

### 2.5 microSD (SDMMC 4-bit)

The FSEQ player reads `.fseq` shows from a microSD card on the SDMMC 4-bit bus:

| Signal | GPIO | Signal | GPIO |
|--------|-----:|--------|-----:|
| CLK    | 39   | D0     | 41   |
| CMD    | 40   | D1     | 42   |
|        |      | D2     | 43   |
|        |      | D3     | 44   |

None of these GPIOs appear in the LED bus, SPI display, I2C, Ethernet or UART pin lists.

### 2.6 Pad power domains (VDD_IO_5 LDO)

GPIO 39–48 sit in the **VDD_IO_5** pad domain, powered by the SoC's internal LDO output **VO4**. Left unprogrammed, VO4 idles near **1.2 V**, so those pads only swing ~1.2 V — not enough to drive a 5 V buffer or an SD card. `main::power_vdd_io5_pads()` programs VO4 to **3.3 V** at boot. This domain covers **CH5 CLOCK (46)**, **CH7 DATA/CLOCK (47/48)** and the entire **microSD** bus (39–44), so the LDO step is mandatory for those outputs.

---

## 3. Q1 — Can the parallel bus run free-running with an arbitrary clock?

**Yes — two backends do this**, both selected at build time (Kconfig `PIXFROG_LED_OUTPUT_BACKEND`, default PARLIO). They emit identical sample streams and differ only in the engine moving the bytes:

**Default — PARLIO TX (loop transmission).** The PARLIO TX unit is a plain "N-bit bus + clock, fed by DMA" engine with no LCD line/frame timing registers:

1. `parlio_new_tx_unit` with `data_width = 16`, `clk_src = PARLIO_CLK_SRC_DEFAULT` (PLL_F160M), `output_clk_freq_hz = 16 MHz` (160 MHz / 10, exact), `clk_out_gpio_num = NC`.
2. Two PSRAM frame buffers (`heap_caps_aligned_calloc`, `MALLOC_CAP_SPIRAM`); the render task encodes into the back buffer while the front one is on the wire.
3. In **loop transmission** mode the mounted frame is a flat DMA buffer repeated back-to-back with no gap — the encoded reset tail separates repeats, so the strips simply see the last frame re-sent continuously, like a video signal.
4. A new `parlio_tx_unit_transmit` while looping does not queue a transaction; the driver concatenates the new payload to the tail of the running link list, so the buffer swap happens at the next frame boundary, gapless.

**Legacy — LCD_CAM RGB panel** (`PIXFROG_LED_OUTPUT_LCD_CAM`, not CI-built). ESP32-P4's LCD_CAM peripheral is a superset of the ESP32-S3 one (i80 and RGB/DPI modes):

1. `esp_lcd_new_rgb_panel` with `data_width = 16`, `flags.fb_in_psram = true`, `num_fbs = 2`, `refresh_on_demand = true`, `pclk_hz = 16 MHz`.
2. The P4 LCD_CAM horizontal timing registers are only 12-bit, so a frame is laid out as `v_res` lines of ≤ 4095 samples — **not** one giant line (that silently truncates in hardware). Hardware inserts one blank PCLK per line (the HSYNC tick); `h_res` is chosen as a multiple of every active NRZ bit period so that tick always lands on an inter-bit LOW and merely stretches it by 62.5 ns.
3. GDMA streams the PSRAM frame buffer to the 16 chosen GPIOs while a frame is being emitted; `esp_lcd_panel_draw_bitmap` re-arms the next emission.
4. Between frames, the trailing samples (zeros) keep DATA low long enough to satisfy the 1-wire latch (TRESET).

---

## 4. Q2 — Which Ethernet PHY?

**IP101GRI**, already soldered on the Waveshare DEV-KIT (and on the Espressif Function EV Board).

| PHY        | REF_CLK         | Logic | Pros                                            | Cons                          |
|------------|-----------------|------:|--------------------------------------------------|-------------------------------|
| **IP101GRI** | 50 MHz int/ext | 3.3 V | Officially supported by Espressif, drivers stable in IDF, already on the DEV-KIT | QFN32 package, sourcing variable |
| LAN8720A   | 50 MHz ext only | 3.3 V | Very common, breakout boards everywhere         | Needs external 50 MHz oscillator, picky reset |
| KSZ8081RNB | 25 MHz ext + PLL | 3.3 V | Robust, excellent Microchip docs                | More expensive, MDIO pull-up sometimes needed |
| RTL8201F   | 50 MHz int/ext | 3.3 V | Cheap, integrates the clock                     | Less battle-tested driver path on P4 |

**Decision**: stick with IP101GRI for v0. The IDF helper `esp_eth_phy_new_ip101()` covers it natively.

---

## 5. Q3 — Can the CLOCK line be a bit of the parallel bus?

**Yes — and that's the chosen strategy.**

The 16-bit bus is DMA-driven on both backends: every PCLK tick, all 16 GPIOs are updated simultaneously from the current sample word. So:

1. **Hardware-guaranteed synchronism** between DATA and CLOCK of the same channel — they leave on the same PCLK edge.
2. CLOCK waveform is **encoded into bits 1/3/5/.../15 of the DMA buffer**, exactly like the DATA bits.
3. For 1-wire protocols (WS2815, …) the CLOCK bit is held at 0 — the strip ignores its CLOCK pin since it has none.
4. For clocked protocols (APA102, …) the CLOCK bit alternates at the ratio of PCLK to the requested CLOCK rate.

**Bandwidth-wise**, the DMA throughput is identical regardless of protocol — only the sample encoding differs.

---

## 6. Level shifters

The SoC drives **3.3 V CMOS**. 5 V strips have variable thresholds:

| Strip               | Vih @ Vcc=5 V              | 3.3 V direct OK? |
|---------------------|----------------------------|------------------|
| WS2815              | typ 0.7 × Vcc = 3.5 V      | **No** — borderline |
| WS2812B             | typ 0.7 × Vcc = 3.5 V      | No (same)        |
| WS2811              | 0.7 × Vcc = 3.5 V          | No               |
| SK6812              | 0.65 × Vcc = 3.25 V        | Marginal         |
| APA102 / SK9822     | 0.7 × Vcc = 3.5 V          | No               |

**Decision**: level shifters are mandatory for production. For early bring-up with short strips (< 30 LEDs) near the PSU, the first WS2815 often regenerates the signal — fine for a scope test, not for shipping.

**Part**: **74HCT245** (8 bits per package, two packages cover all 16 GPIOs). The `HCT` (not HC) variant is critical: Vih = 2.0 V (CMOS-to-TTL threshold), guarantees clean switching from 3.3 V CMOS into a 5 V CMOS strip.

![3.3 V → 5 V level shifting with a 74HCT245 buffer](img/level-shifter.svg)

> Alternatives: **SN74LVC1T45** per GPIO (overkill but very clean), or **TXS0108E** (avoid — its auto-direction is too slow for fast clocked protocols).

**Realised as the pixfrog shield** — KiCad project + JLCPCB production files in
[`hardware/pixfrog_shield/`](../hardware/pixfrog_shield/README.md): 2× 74HCT245,
DIP-selectable series termination per line (DATA 249 Ω ↔ ≈34 Ω, CLOCK 33 Ω ↔ ≈18 Ω),
one 5 V TVS clamp per output, 8× JST-XH (DATA/CLOCK/GND), plugs onto the
devkit's 2×20 header.

![pixfrog shield](img/pixfrog-shield.png)

---

## 7. Encoder wiring (Adafruit seesaw 4991)

![OLED and seesaw encoder on the shared I²C bus](img/peripherals-wiring.svg)

Everything is over I2C:

```
seesaw 4991       ESP32-P4
─────────────     ──────────
Vin (3-5 V)  ───► 3.3 V
GND          ───► GND
SDA          ───► GPIO 7  (I2C SDA)
SCL          ───► GPIO 8  (I2C SCL)
INT          ───► not connected (see below)
ADR (option) ───► float → address 0x36
SS (option)  ───► unused (I2C only)
```

The seesaw INT_N line is **deliberately unused** — a 4-wire harness
(VCC/GND/SDA/SCL) is all the encoder needs:

- `ui_task` already loops at ~30 Hz for the encoder-LED animation and the
  diff-based display refresh, so time-polling adds at most one tick (~33 ms)
  of input latency — imperceptible on a rotary detent.
- Skipping the interrupt machinery removes two extra I2C reads per poll
  (the `GPIO_INTFLAG` + `ENCODER_DELTA` latch clears the seesaw requires to
  re-arm INT_N).

Firmware path: `ui_task` polls position + button over I2C every ~33 ms and
queues RotateLeft/RotateRight/Click/LongPress events into the menu FSM.
GPIO 21 (previously reserved for INT) is free.

Reference: [Adafruit Learn — I2C QT Rotary Encoder](https://learn.adafruit.com/adafruit-i2c-qt-rotary-encoder).

---

## 8. OLED wiring (SSD1306)

![SSD1306 home screen mockup](img/oled-ui.svg)

```
SSD1306 128×64    ESP32-P4
──────────────    ──────────
VCC (3-5 V)  ───► 3.3 V
GND          ───► GND
SDA          ───► GPIO 7 (shared with encoder)
SCL          ───► GPIO 8 (shared)
```

I2C pull-ups (4.7 kΩ) are needed once on the bus — check whether the OLED module already includes them.

OLED refresh: **10 Hz** via diff-based flush (only pages whose pixels changed are pushed). Idle bus traffic is essentially zero.

---

## 9. TFT display wiring (ST7789V / ILI9341 — optional, replaces OLED)

When `CONFIG_PIXFROG_DISPLAY_TFT=y` is selected, a **ST7789V (or pin-compatible ILI9341) 320×240 SPI display** is used instead of the OLED. The TFT is driven in landscape orientation (320 wide × 240 tall) via `esp_lcd_new_panel_st7789`.

```
ST7789V module    ESP32-P4
──────────────    ─────────
VCC (3.3 V)  ───► 3.3 V
GND          ───► GND
CLK (SCLK)   ───► GPIO 13
MOSI (SDA)   ───► GPIO 11
CS           ───► GPIO 12
DC (RS)      ───► GPIO 10
RST          ───► GPIO 9
```

SPI host: `SPI2_HOST`, 40 MHz. The display is portrait 240×320 at the hardware level; firmware issues `esp_lcd_panel_swap_xy(true)` after init to address it as landscape 320×240.

Color format: RGB565 big-endian (ST7789 native). `canvas_tft.cpp` applies `__builtin_bswap16` to every pixel value before writing to the DMA buffer.

The OLED and TFT share the same I2C encoder/interrupt wiring (§8 above). The I2C bus is still initialised regardless of display choice (encoder requires it).

---

## 10. Power

- ESP32-P4 + 3.3 V logic: LDO or DC-DC on the DEV-KIT (already done)
- LED strips at 5 V: **separate supply**, **common ground with the MCU**
- 1000 µF decoupling on the strip side, ideally per metre of strip
- 470 Ω series resistor between level shifter output and the first LED DATA pin (damps reflections)

> Never power more than a handful of LEDs from the ESP32 board's 5 V rail — not enough current, and the noise injected into the SoC will cause weird failures.

---

## 11. Future hardware (v1, custom PCB)

- 2-layer PCB minimum, continuous ground plane
- Dedicated 1 A LDO for the 3.3 V rail
- Isolated DC-DC for the level shifters if the run to the strips exceeds ~30 cm
- 8 × JST-XH 3-pin connectors per channel (DATA, CLOCK, GND)
- Front-panel Ethernet jack
- Passive aluminium heatsink — octal PSRAM dissipates at full throughput
