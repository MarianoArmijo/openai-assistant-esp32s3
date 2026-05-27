if(IDF_TARGET STREQUAL linux)
  idf_component_register(
    SRCS "main.cpp"
    REQUIRES esp_event esp_log)
else()
  idf_component_register(
    SRCS "main.cpp" "wifi.cpp" "media.cpp" "websocket.cpp" "wake_word.cpp" "memory.cpp"
    REQUIRES driver esp_wifi nvs_flash esp_psram esp_websocket_client json mbedtls esp-sr spiffs)
endif()
