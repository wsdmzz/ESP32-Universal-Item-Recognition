idf_component_register(
    SRCS "main.cpp" "quiz_photos.cpp"
    INCLUDE_DIRS "."
    REQUIRES axp313a tft_display esp32-camera esp-dl items103
             driver esp_timer spi_flash wifi_manager esp_http_server
             spiffs esp_hw_support
)

# 题库镜像：spiffs_data/photos/*.jpg → storage 分区(0xc10000, 3.94MB)，挂载在 /spiffs
# 103 张 480x320 共 1.72MB，占 44%，还能再塞约 130 张同规格的。
# FLASH_IN_PROJECT：idf.py flash 时一并写入；只改图片不改动代码时单独烧这一分区即可：
#   python -m esptool -p /dev/ttyACM0 -b 460800 write_flash 0xc10000 build/storage.bin
spiffs_create_partition_image(storage ../spiffs_data FLASH_IN_PROJECT)
