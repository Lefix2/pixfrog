// Board definition: Waveshare ESP32-P4 Module DEV-KIT
// (datasheet:
// https://www.espressif.com/sites/default/files/documentation/esp32-p4_datasheet_en.pdf)
//
// Pinout source: Waveshare ESP32-P4 Module DEV-KIT schematic (P6 40-pin header).
// Only GPIOs exposed on P6 are candidates here.
//
// ════════════════════════════════════════════════════════════════════════════
// P6  —  40-pin 2.54 mm header (even = left row, odd = right row)
// ════════════════════════════════════════════════════════════════════════════
//
//   Pin  Left              │  Pin  Right
//   ──────────────────────────────────────
//    2   GPIO7  (I2C SDA)  │   1   VCC_5V
//    4   GPIO8  (I2C SCL)  │   3   GND
//    6   GPIO23 (LED CH3 CLK) │ 5  GPIO37  (UART0 TXD)
//    8   GND               │   7   GPIO38  (UART0 RXD)
//   10   GPIO21 (enc. IRQ) │   9   GPIO22  (LED CH3 DATA)
//   12   —                 │  11   GND
//   14   GPIO6             │  13   GPIO5   (LED CH2 CLK)
//   16   —                 │  15   GPIO4   (LED CH2 DATA)
//   18   —                 │  17   GND
//   20   GPIO3  (LED CH1 CLK) │ 19  GPIO1  (status LED)
//   22   GPIO2  (LED CH1 DATA) │ 21  GPIO36 (strapping — keep HIGH)
//   24   GPIO0             │  23   GPIO32  (LED CH6 DATA)
//   26   GND               │  25   GPIO25  (LED CH4 CLK)
//   28   GPIO24 (LED CH4 DATA) │ 27  GND
//   30   GPIO33 (LED CH6 CLK) │ 29  GPIO54  (LED CH8 CLK)
//   32   GPIO26 (LED CH5 DATA) │ 31  GND
//   34   GPIO48 (LED CH7 CLK) │ 33  GPIO46  (LED CH5 CLK)
//   36   GPIO53 (LED CH8 DATA) │ 35  GPIO27
//   38   GPIO47 (LED CH7 DATA) │ 37  GPIO45
//   40   GND               │  39   —
//
// ════════════════════════════════════════════════════════════════════════════
// On-board peripheral  →  GPIO usage
// ════════════════════════════════════════════════════════════════════════════
//
//  LED bus (LCD_CAM 16-bit parallel, 16 MHz PCLK)
//    CH1 DATA/CLK : GPIO2  / GPIO3
//    CH2 DATA/CLK : GPIO4  / GPIO5
//    CH3 DATA/CLK : GPIO22 / GPIO23
//    CH4 DATA/CLK : GPIO24 / GPIO25
//    CH5 DATA/CLK : GPIO26 / GPIO46
//    CH6 DATA/CLK : GPIO32 / GPIO33
//    CH7 DATA/CLK : GPIO47 / GPIO48
//    CH8 DATA/CLK : GPIO53 / GPIO54
//
//  I2C shared bus (OLED SSD1306 + seesaw encoder)
//    SDA : GPIO7    SCL : GPIO8
//
//  SPI display (ST7789V / ILI9341, reuses I2S pins of on-board ES8311 codec)
//    CLK  : GPIO13   MOSI : GPIO11
//    CS   : GPIO12   DC   : GPIO10
//    RST  : GPIO9
//
//  Seesaw encoder IRQ : GPIO21 (active-LOW)
//  Status LED         : GPIO1
//  UART0 console      : TX=GPIO37  RX=GPIO38
//
//  Ethernet RMII (IP101GR PHY, fixed SoC pins — not on P6)
//    TXD0=34  TXD1=35  TX_EN=49  RXD0=29  RXD1=30  CRS_DV=28  REF_CLK=50
//    MDC=31   MDIO=52  PHY_RESET=51
//
//  ES8311 audio codec (I2S — unused by pixfrog, GPIOs repurposed for SPI)
//    MCLK=GPIO13  SCLK=GPIO12  LRCK=GPIO10  DOUT=GPIO11  DIN=GPIO9
//    Config via I2C shared bus above.
//
// ════════════════════════════════════════════════════════════════════════════
// Strapping pins (ESP32-P4 datasheet §3.3)
// ════════════════════════════════════════════════════════════════════════════
//
//  GPIO35 — boot/download select (driven by BOOT button, not on P6)
//  GPIO36 — must read HIGH for reliable serial-download boot
//  GPIO34 — JTAG source select at boot
//
// GPIO matrix: every peripheral signal is routed through the GPIO matrix,
// so any free GPIO is a valid LCD_CAM data line or SPI pin. The only
// hard constraints are the strapping pins above and the fixed RMII pins.

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

constexpr int kLedDataGpio[8]  = { 2, 4, 22, 24, 26, 32, 47, 53 };
constexpr int kLedClockGpio[8] = { 3, 5, 23, 25, 46, 33, 48, 54 };

// Full 16-pin bus in bit order (DATA0, CLK0, DATA1, CLK1, ...).
// clang-format off
constexpr int kLedBusGpio[16] = {
     2,  3,   4,  5,  22, 23,  24, 25,
    26, 46,  32, 33,  47, 48,  53, 54,
};
// clang-format on

// ────────────────────────────────────────────────────────────────────────────
// Ethernet — RMII to on-board IP101GRI PHY
// ────────────────────────────────────────────────────────────────────────────
//
// Verified against the Waveshare DEV-KIT schematic (PHY U7 = IP101GRI) and
// the official board pinout. The PHY's RMII + management pins use fixed SoC
// GPIOs (28-35, 49-52) and PHY RESET is GPIO51; none are on the P6 header,
// so they do not constrain the LED pinout above.
//
// RMII data/clock equal the ESP32-P4 ETH_ESP32_EMAC_DEFAULT_CONFIG() defaults
// (IDF v5.5), so main/init_network() only has to override MDC/MDIO:
//   TXD0=34  TXD1=35  TX_EN=49  RXD0=29  RXD1=30  CRS_DV=28  REF_CLK(50M)=50
//
// Management bus + reset are read straight off U7:
//   MDC = pin 22, MDIO = pin 23, RESET = pin 32.

constexpr int kEthMdcGpio      = 31;
constexpr int kEthMdioGpio     = 52;
constexpr int kEthPhyResetGpio = 51;    // drive HIGH to release U7 from reset
constexpr int kEthPhyAddress   = 0x01;  // IP101GRI strap (AD0/AD3); verify if link fails

// ────────────────────────────────────────────────────────────────────────────
// I2C — shared bus for OLED + seesaw encoder
// ────────────────────────────────────────────────────────────────────────────

constexpr int kI2cPort        = 0;
constexpr int kI2cSdaGpio     = 7;  // header pin SDA(GPIO7)
constexpr int kI2cSclGpio     = 8;  // header pin SCL(GPIO8)
constexpr uint32_t kI2cFreqHz = 400'000;

constexpr uint8_t kOledI2cAddr    = 0x3C;
constexpr uint8_t kEncoderI2cAddr = 0x36;  // Adafruit seesaw 4991 default
constexpr int kEncoderIntGpio     = 21;    // active-LOW IRQ from seesaw

// ────────────────────────────────────────────────────────────────────────────
// SPI display — ST7789V or ILI9341
// ────────────────────────────────────────────────────────────────────────────
//
// These GPIOs are shared with the on-board ES8311 audio codec I2S bus (U8).
// The codec is unused by pixfrog so the pins are free. SPI2 (HSPI) is used;
// all signals are routed through the GPIO matrix so no fixed-pin constraint.

constexpr int kDisplaySpiHost        = 1;  // SPI2 / HSPI
constexpr int kDisplayClkGpio        = 13;
constexpr int kDisplayMosiGpio       = 11;
constexpr int kDisplayCsGpio         = 12;
constexpr int kDisplayDcGpio         = 10;
constexpr int kDisplayRstGpio        = 9;
constexpr uint32_t kDisplaySpiFreqHz = 40'000'000;

// ────────────────────────────────────────────────────────────────────────────
// Miscellaneous
// ────────────────────────────────────────────────────────────────────────────

constexpr int kStatusLedGpio = 1;

// UART0 console (TXD/RXD) is on the dedicated pins GPIO 37 / GPIO 38
// printed on the header silkscreen. Configured by the IDF default
// console driver — not used directly from this board file.
constexpr int kDebugTxGpio = 37;
constexpr int kDebugRxGpio = 38;

}  // namespace pixfrog::board
