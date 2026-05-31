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

Validated against the Waveshare ESP32-P4 Module DEV-KIT schematic (P6 40-pin header). Strapping pins to avoid at boot: **GPIO 0** (bootloader entry), **GPIO 6** (download/normal mode), **GPIO 35** (SDIO). GPIO 16 is not exposed on P6 and must not be used.

### 2.1 LED 16-bit parallel bus (LCD_CAM)

ESP32-P4 routes every peripheral signal through the GPIO matrix, including the 16 data lines of the LCD_CAM bus. Any free GPIO is a valid LCD_CAM data line — the choices below pick header pins that are easy to route in pairs.

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

> The LCD_CAM PCLK pin is configured with `pclk_gpio_num = -1` — it stays internal. CLOCK signals for clocked LED protocols (APA102, …) are encoded as bits 1, 3, 5, … of the parallel sample stream, not derived from PCLK. See `docs/PROTOCOLS.md` §3.

### 2.2 Ethernet RMII (internal)

The PHY's RMII pins are wired directly to the SoC on the DEV-KIT and are not brought out to the header, so they impose no constraint on the LED bus pinout above. Only MDC / MDIO / reset are configurable from firmware:

| Signal RMII           | GPIO P4         |
|-----------------------|-----------------|
| EMAC_REF_CLK          | internal        |
| EMAC_TX_EN / TXD[0:1] | internal        |
| EMAC_RXD[0:1] / CRS_DV | internal       |
| MDC                   | GPIO 29         |
| MDIO                  | GPIO 30         |
| PHY reset             | -1 (PHY uses onboard RC POR) |
| PHY address           | 0x01 (strapped) |

### 2.3 I2C (OLED + encoder)

| Signal | GPIO | Note                                             |
|--------|-----:|--------------------------------------------------|
| SDA    | 7    | `SDA(GPIO7)` on the header silkscreen           |
| SCL    | 8    | `SCL(GPIO8)` on the header silkscreen           |
| INT    | 21   | active-LOW IRQ from the seesaw                  |

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

---

## 3. Q1 — Can LCD_CAM produce a 16-bit free-running bus with an arbitrary clock?

**Yes.** ESP32-P4's LCD_CAM peripheral is a superset of the ESP32-S3 one:

- **i80 mode** (Intel 8080): 8/16/24-bit + WR/RD strobes. Overkill for our use case.
- **RGB (DPI) mode**: 16/24-bit with HSYNC/VSYNC. Ideal here — set HSYNC/VSYNC porches to 0 and the bus is effectively continuous, DMA-driven.

Our setup:

1. `esp_lcd_new_rgb_panel` with `data_width = 16`, `flags.fb_in_psram = true`, `num_fbs = 2`, `refresh_on_demand = true`.
2. PCLK divides from `LCD_CLK_SRC_DEFAULT`; we ask for the exact frequency we computed (`pclk_hz = 16 MHz` — see `PROTOCOLS.md` §3).
3. GDMA streams the PSRAM frame buffer to the 16 chosen GPIOs continuously while a frame is being emitted.
4. The bus is free-running for the duration of one frame; between frames, the trailing samples (zeros) keep DATA low long enough to satisfy 1-wire latch (TRESET).

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

## 5. Q3 — Can the CLOCK line be a bit of the LCD_CAM bus?

**Yes — and that's the chosen strategy.**

The 16-bit bus is DMA-driven: every PCLK tick, all 16 GPIOs are updated simultaneously from the current sample word. So:

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
INT          ───► GPIO 21 (active LOW, internal pull-up on seesaw)
ADR (option) ───► float → address 0x36
SS (option)  ───► unused (I2C only)
```

The seesaw INT is configured to assert on:

- encoder rotation (every detent)
- button (separate press / release)

Firmware path:

1. Falling edge on GPIO 21 → `xSemaphoreGiveFromISR(ui_wakeup_sem)`
2. `ui_task` consumes the semaphore, reads position + button over I2C
3. Reads `GPIO_INTFLAG` + `ENCODER_DELTA` to clear the seesaw interrupts so the next change re-asserts INT

A 100 ms polling fallback also runs (used to refresh HOME stats), so even if a seesaw interrupt is ever missed, the next poll picks it up.

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

## 9. Power

- ESP32-P4 + 3.3 V logic: LDO or DC-DC on the DEV-KIT (already done)
- LED strips at 5 V: **separate supply**, **common ground with the MCU**
- 1000 µF decoupling on the strip side, ideally per metre of strip
- 470 Ω series resistor between level shifter output and the first LED DATA pin (damps reflections)

> Never power more than a handful of LEDs from the ESP32 board's 5 V rail — not enough current, and the noise injected into the SoC will cause weird failures.

---

## 10. Future hardware (v1, custom PCB)

- 2-layer PCB minimum, continuous ground plane
- Dedicated 1 A LDO for the 3.3 V rail
- Isolated DC-DC for the level shifters if the run to the strips exceeds ~30 cm
- 8 × JST-XH 3-pin connectors per channel (DATA, CLOCK, GND)
- Front-panel Ethernet jack
- Passive aluminium heatsink — octal PSRAM dissipates at full throughput
