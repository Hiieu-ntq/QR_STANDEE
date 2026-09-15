idf_component_register(SRCS "qrcodegen.c" "main.c" "qrcode.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver esp_lcd esp_vfs_console)
