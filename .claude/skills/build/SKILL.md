---
name: build
description: Build the firmware (esp32p4) in the espressif/idf:v5.5 docker image — NV3007 default, st7789/oled CI overlays
---

Default build — **NV3007** bar panel + PARLIO LED backend:
```bash
docker run --rm -v "$PWD":/project -w /project -u "$(id -u):$(id -g)" -e HOME=/tmp espressif/idf:v5.5 idf.py build
```

OLED overlay (separate build dir + sdkconfig, else the overlay is ignored):
```bash
docker run --rm -v "$PWD":/project -w /project -u "$(id -u):$(id -g)" -e HOME=/tmp espressif/idf:v5.5 \
    idf.py -B build.oled -D SDKCONFIG=build.oled/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci.oled" build
```

ST7789 panel overlay (separate build dir + sdkconfig, else the overlay is
ignored). The default build is **NV3007** — `PIXFROG_TFT_PANEL` defaults to it in
`components/ui/Kconfig`. Picking the wrong panel is not a build error: it
flashes, the backlight lights, and the matrix is driven with the other
controller's init. Check the boot log — `TFT: NV3007 428x142 ready (landscape)`
vs `TFT: ST7789 320x240 ready`. Flash a variant from its own dir, e.g.
`idf.py -B build.st7789 -p /dev/ttyACM0 flash`.
```bash
docker run --rm -v "$PWD":/project -w /project -u "$(id -u):$(id -g)" -e HOME=/tmp espressif/idf:v5.5 \
    idf.py -B build.st7789 -D SDKCONFIG=build.st7789/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci.st7789" build
```

LCD_CAM LED-output backend overlay (`sdkconfig.ci.lcdcam` — the default build
uses PARLIO; flash with `idf.py -B build.lcdcam -p /dev/ttyACM0 flash`):
```bash
docker run --rm -v "$PWD":/project -w /project -u "$(id -u):$(id -g)" -e HOME=/tmp espressif/idf:v5.5 \
    idf.py -B build.lcdcam -D SDKCONFIG=build.lcdcam/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci.lcdcam" build
```

Inside the devcontainer, drop the docker wrapper: `idf.py build`.

ninja "Permission denied" in build/ (root-owned files after a flash):
```bash
docker run --rm -v "$PWD":/project espressif/idf:v5.5 chown -R "$(id -u):$(id -g)" /project/build
```
`version`/ABOUT still showing an old commit after a flash? cmake did not
reconfigure (a `build/` carried over from another checkout or machine), so
`esp_app_desc.c` kept its stale `PROJECT_VER` and `__DATE__` while everything
else rebuilt — the ELF is current, only the version string lies. `idf.py
reconfigure` then `build` refreshes it. Cross-check with the boot log's
`ELF file SHA256` against `sha256sum build/pixfrog.elf`.

If sdkconfig.defaults changed — or you are switching panel — `rm -f sdkconfig` (and build.oled/sdkconfig, build.st7789/sdkconfig, build.lcdcam/sdkconfig) first — defaults only apply when sdkconfig doesn't exist.

`sdkconfig.defaults` pins target esp32p4 (no `set-target` needed), hex PSRAM @ 80 MHz, 360 MHz CPU, no tickless idle, lwIP UDP tuning, and the partition table — change them only deliberately via `menuconfig`. **Kconfig drops unknown symbols silently** — after touching the defaults, check the boot log (`esp_psram: Speed: 80MHz`, `cpu freq: 360000000 Hz`) or `grep` the generated sdkconfig.
