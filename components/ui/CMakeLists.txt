set(srcs
    "src/ui.cpp"
    "src/menu.cpp"
    "src/encoder_seesaw.cpp"
    "src/font_5x8.cpp"
    "src/splash.cpp"
)

set(requires driver esp_common esp_timer config_store dmx_manager lcd_cam_output freertos)

if(CONFIG_PIXFROG_DISPLAY_TFT)
    list(APPEND srcs "src/tft_st7789.cpp" "src/canvas_tft.cpp")
    list(APPEND requires esp_lcd)
else()
    list(APPEND srcs "src/oled_ssd1306.cpp" "src/canvas_oled.cpp")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES ${requires}
)
