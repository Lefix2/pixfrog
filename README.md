# pixfrog

> ArtNet → LED driver haute performance pour ESP32-P4. 8 canaux × 2 signaux (DATA + CLOCK), supportant protocoles 1-fil (WS2815, WS2812B, SK6812…) et clockés (APA102, SK9822, LPD8806).

## Caractéristiques

- ESP32-P4 dual-core RISC-V 400 MHz, 32 Mo PSRAM octal
- Ethernet 10/100M via PHY externe (IP101GRI)
- 8 canaux LED parallèles via LCD_CAM 16-bit
- Réception ArtNet UDP (jusqu'à 48 univers)
- Interface locale : OLED I2C SSD1306 + encodeur seesaw
- Configuration 100 % locale, persistance NVS

## Build

Prérequis : ESP-IDF v5.3+ avec support ESP32-P4 (`idf.py set-target esp32p4`).

```bash
idf.py set-target esp32p4
idf.py menuconfig          # optionnel, valeurs par défaut OK
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## Documentation

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — topologie tâches, frame lifecycle, budget mémoire
- [docs/HARDWARE.md](docs/HARDWARE.md) — pinout, level shifters, câblage encodeur/OLED
- [docs/PROTOCOLS.md](docs/PROTOCOLS.md) — timings LED, formule PCLK, encodage DMA

## Statut

Bootstrap initial — structure, documentation, et squelettes de modules. Implémentation en cours.

## Licence

À définir.
