# oledsplash

Bakes the **static OLED splash frog** (`components/ui/src/splash_oled.cpp`) — a
single small 1bpp frog logo, area-downsampled from the settled frame of the TFT
splash animation (`splash_anim.cpp`). One frame, so the OLED build doesn't pull
the full ~76 KB TFT strip. `splash.cpp` (OLED branch) blits it above the
"pixfrog" wordmark.

The asset is **generated**, not hand-edited. The frame data lives behind
`CONFIG_PIXFROG_DISPLAY_TFT` and `ui_internal.h` pulls an IDF header, so the
generator links against `splash_anim.cpp` with that define and a stubbed header:

```bash
mkdir -p /tmp/s/driver
printf 'typedef void* i2c_master_bus_handle_t;\ntypedef void* i2c_master_dev_handle_t;\n' \
    > /tmp/s/driver/i2c_master.h
cd tools/oledsplash
g++ -std=c++17 -DCONFIG_PIXFROG_DISPLAY_TFT -I/tmp/s -I../../components/ui/src \
    gen.cpp ../../components/ui/src/splash_anim.cpp -o gen
./gen [destW=64] [destH=45] > ../../components/ui/src/splash_oled.cpp
```

Output: `frog_oled_{w,h,data}()` over `kFrogOled` (1bpp MSB-first, stride
`(w+7)/8`). `splash_oled.cpp` is in `.clang-format-ignore`. To re-derive the frog
from the SVG instead, regenerate `splash_anim.cpp` via `tools/splashgen` first.
