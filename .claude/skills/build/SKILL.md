---
name: build
description: Build the firmware (esp32p4) in the espressif/idf:v5.5 docker image — OLED default or TFT CI overlay
---

OLED (default):
```bash
docker run --rm -v "$PWD":/project -w /project -u "$(id -u):$(id -g)" -e HOME=/tmp espressif/idf:v5.5 idf.py build
```

TFT overlay (separate build dir + sdkconfig, else the overlay is ignored):
```bash
docker run --rm -v "$PWD":/project -w /project -u "$(id -u):$(id -g)" -e HOME=/tmp espressif/idf:v5.5 \
    idf.py -B build.tft -D SDKCONFIG=build.tft/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci.tft" build
```

Inside the devcontainer, drop the docker wrapper: `idf.py build`.

ninja "Permission denied" in build/ (root-owned files after a flash):
```bash
docker run --rm -v "$PWD":/project espressif/idf:v5.5 chown -R "$(id -u):$(id -g)" /project/build
```
If sdkconfig.defaults changed, `rm -f sdkconfig` (and build.tft/sdkconfig) first — defaults only apply when sdkconfig doesn't exist.

`sdkconfig.defaults` pins target esp32p4 (no `set-target` needed), octal PSRAM, 400 MHz CPU, no tickless idle, lwIP UDP tuning, and the partition table — change them only deliberately via `menuconfig`.
