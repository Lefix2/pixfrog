// Host shim for the ESP-IDF driver/i2c_master.h header.
//
// ui_internal.h includes <driver/i2c_master.h> for the i2c_master_bus_handle_t
// type used in the OLED/encoder init signatures. The emulator never touches I2C,
// so we only need the handle type to exist. Defined as an opaque pointer.

#pragma once

typedef void* i2c_master_bus_handle_t;
