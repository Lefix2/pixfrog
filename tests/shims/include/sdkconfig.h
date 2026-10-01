// Host shim: the Kconfig choices the harness builds against. The LED backend
// defaults to PARLIO; a target defines it to 0 to build the LCD_CAM one.
#pragma once
#ifndef CONFIG_PIXFROG_LED_OUTPUT_PARLIO
#define CONFIG_PIXFROG_LED_OUTPUT_PARLIO 1
#endif
