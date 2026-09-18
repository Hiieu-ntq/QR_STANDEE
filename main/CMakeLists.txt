idf_component_register(SRCS "qrcodegen.c" "main.c" "qrcode.c" "internet_check.c" "wifi_manager.c" "web_server.c" "dns_server.c"
                    INCLUDE_DIRS "."
                    EMBED_FILES "web/index.html"
                    REQUIRES driver esp_lcd esp_vfs_console spi_flash nvs_flash esp_wifi esp_event esp_netif lwip esp_http_server json)
