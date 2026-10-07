idf_component_register(
    SRCS "src/net.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_netif esp_eth esp_event lwip esp_hw_support config_store
)
