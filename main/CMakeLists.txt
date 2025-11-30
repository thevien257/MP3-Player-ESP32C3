# idf_component_register(
#     SRCS "main.c"
#     INCLUDE_DIRS "."
#     REQUIRES u8g2 u8g2-hal-esp-idf driver
# )

idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver esp_timer fatfs sdmmc u8g2 u8g2-hal-esp-idf chmorgan__esp-libhelix-mp3)