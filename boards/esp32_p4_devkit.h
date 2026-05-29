// Board definition: Espressif ESP32-P4 Function EV Board
// (datasheet: https://www.espressif.com/sites/default/files/documentation/esp32-p4_datasheet_en.pdf)
//
// This header is the single source of truth for hardware pinout.
// To target a different board, add boards/<name>.h and select it via CMake.
//
// All GPIO assignments here MUST be revalidated against the ESP32-P4 datasheet
// (peripheral matrix capabilities, strapping pins, PSRAM/flash conflicts)
// before running on real hardware. See docs/HARDWARE.md for rationale.

#pragma once

#include <stdint.h>

namespace pixfrog::board {

// ────────────────────────────────────────────────────────────────────────────
// Identification
// ────────────────────────────────────────────────────────────────────────────

constexpr const char* kBoardName = "esp32_p4_devkit";
constexpr const char* kBoardRev  = "v0";

// ────────────────────────────────────────────────────────────────────────────
// LCD_CAM 16-bit parallel — DATA[k] and CLOCK[k] for 8 LED channels
// ────────────────────────────────────────────────────────────────────────────
//
// Bit position on the parallel bus → physical GPIO.
// Bit 0 = CH1 DATA, Bit 1 = CH1 CLOCK, Bit 2 = CH2 DATA, etc.
//
// ⚠ Pre-flight TODO: confirm these GPIOs do not conflict with the
//   on-board RMII pins on the Function EV Board. Likely conflicts on
//   GPIO 31-35; expect to remap.

constexpr int kLedDataGpio [8] = { 36, 38, 40, 42, 44, 46, 48, 50 };
constexpr int kLedClockGpio[8] = { 37, 39, 41, 43, 45, 47, 49, 51 };

// Convenience: the full 16-pin bus in bit order (DATA0, CLK0, DATA1, CLK1, …)
constexpr int kLedBusGpio[16] = {
    36, 37, 38, 39, 40, 41, 42, 43,
    44, 45, 46, 47, 48, 49, 50, 51,
};

// ────────────────────────────────────────────────────────────────────────────
// Ethernet — RMII to on-board IP101GRI PHY
// ────────────────────────────────────────────────────────────────────────────

constexpr int kEthMdcGpio       = 29;
constexpr int kEthMdioGpio      = 30;
constexpr int kEthPhyResetGpio  = 27;
constexpr int kEthPhyAddress    = 0x01;  // strap-determined on IP101GRI

// REF_CLK and TX/RX data pairs are routed through the GPIO matrix on
// ESP32-P4. The default mapping is documented in HARDWARE.md §2.2 — the
// IDF EMAC driver picks them up from menuconfig (CONFIG_ETH_RMII_*).

// ────────────────────────────────────────────────────────────────────────────
// I2C — shared bus for OLED + seesaw encoder
// ────────────────────────────────────────────────────────────────────────────

constexpr int     kI2cPort        = 0;       // I2C peripheral instance
constexpr int     kI2cSdaGpio     = 7;
constexpr int     kI2cSclGpio     = 8;
constexpr uint32_t kI2cFreqHz     = 400'000; // Fast-mode

constexpr uint8_t kOledI2cAddr    = 0x3C;
constexpr uint8_t kEncoderI2cAddr = 0x36;    // Adafruit seesaw 4991 default
constexpr int     kEncoderIntGpio = 9;       // active-LOW IRQ from seesaw

// ────────────────────────────────────────────────────────────────────────────
// Miscellaneous
// ────────────────────────────────────────────────────────────────────────────

constexpr int kStatusLedGpio = 4;

}  // namespace pixfrog::board
