---
name: build
description: Build the firmware (esp32p4) in the espressif/idf:v5.5 docker image — TFT default or OLED CI overlay
---

TFT (default):
```bash
docker run --rm -v "$PWD":/project -w /project -u "$(id -u):$(id -g)" -e HOME=/tmp espressif/idf:v5.5 idf.py build
```

OLED overlay (separate build dir + sdkconfig, else the overlay is ignored):
```bash
docker run --rm -v "$PWD":/project -w /project -u "$(id -u):$(id -g)" -e HOME=/tmp espressif/idf:v5.5 \
    idf.py -B build.oled -D SDKCONFIG=build.oled/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci.oled" build
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
If sdkconfig.defaults changed, `rm -f sdkconfig` (and build.oled/sdkconfig, build.lcdcam/sdkconfig) first — defaults only apply when sdkconfig doesn't exist.

`sdkconfig.defaults` pins target esp32p4 (no `set-target` needed), hex PSRAM @ 80 MHz, 360 MHz CPU, no tickless idle, lwIP UDP tuning, and the partition table — change them only deliberately via `menuconfig`. **Kconfig drops unknown symbols silently** — after touching the defaults, check the boot log (`esp_psram: Speed: 80MHz`, `cpu freq: 360000000 Hz`) or `grep` the generated sdkconfig.
