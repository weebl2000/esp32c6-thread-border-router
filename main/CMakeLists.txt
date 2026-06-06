# Overlay for $IDF_PATH/examples/openthread/ot_br/main/CMakeLists.txt
# Mirrors the v5.5.x stock example's PRIV_REQUIRES and adds esp_http_server,
# which the OTBR REST API in esp_ot_br.c depends on. (The v5.5 example lists
# explicit requires, so esp_http_server is no longer pulled in transitively.)
idf_component_register(SRCS "esp_ot_br.c"
                       PRIV_REQUIRES esp_psram openthread esp_coex esp_wifi nvs_flash esp_http_server
                       INCLUDE_DIRS ".")
