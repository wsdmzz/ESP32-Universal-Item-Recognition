idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES axp313a tft_display esp32-camera esp-dl fruit19
             driver esp_timer spi_flash
)

# 题库镜像（出题机遗留功能，摄像头应用不读取；如需精简可注释掉）
# spiffs_create_partition_image(storage ../spiffs_data)
