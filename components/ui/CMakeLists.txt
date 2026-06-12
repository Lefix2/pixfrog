set(srcs
    "src/ui.cpp"
    "src/menu.cpp"
    "src/encoder_seesaw.cpp"
    "src/font_data.cpp"
    "src/splash.cpp"
    "src/splash_anim.cpp"
)

set(requires driver esp_common esp_timer esp_app_format config_store dmx_manager fseq_player led_output freertos web_config sacn)

if(CONFIG_PIXFROG_DISPLAY_TFT)
    list(APPEND srcs "src/tft_st7789.cpp" "src/canvas_tft.cpp")
    list(APPEND requires esp_lcd)
else()
    list(APPEND srcs "src/oled_ssd1306.cpp" "src/canvas_oled.cpp" "src/font_oled.cpp"
                     "src/splash_oled.cpp")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES ${requires}
)
