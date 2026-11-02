idf_component_register(
    SRCS "main.cpp" "quiz_photos.cpp"
    INCLUDE_DIRS "."
    REQUIRES axp313a tft_display esp32-camera esp-dl items103
             driver esp_timer spi_flash wifi_manager esp_http_server
             spiffs esp_hw_support
)

# 题库镜像：spiffs_data/photos/*.jpg → storage 分区(0xc10000, 3.94MB)，挂载在 /spiffs
# 103 张 480x320 源文件合计 1,800,172 字节；运行时 esp_spiffs_info 报
# 「storage 分区 3787841 字节，已用 1840081（48.6%）」（差值是 SPIFFS 页开销）。
# 还能再塞约 120 张同规格的。
# FLASH_IN_PROJECT：idf.py flash 时一并写入；只改图片不改动代码时单独烧这一分区即可：
#   python -m esptool -p /dev/ttyACM0 -b 460800 write_flash 0xc10000 build/storage.bin
spiffs_create_partition_image(storage ../spiffs_data FLASH_IN_PROJECT)
