#include "axp313a.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "AXP313A";

static i2c_port_t s_port;
static uint8_t s_addr;

static esp_err_t axp_write2(uint8_t reg, uint8_t val)
{
    uint8_t data[2] = { reg, val };
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(cmd, data, sizeof(data), true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(s_port, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);
    return ret;
}

esp_err_t axp313a_begin(i2c_port_t i2c_num, uint8_t addr)
{
    s_port = i2c_num;
    s_addr = addr;
    // 0x00(EXTEN power signal hold enable) <- 0x04: 保持使能，防止按键行为干扰
    esp_err_t ret = axp_write2(0x00, 0x04);
    vTaskDelay(pdMS_TO_TICKS(100));
    if (ret == ESP_OK) ESP_LOGI(TAG, "AXP313A ready @0x%02X port %d", addr, i2c_num);
    else ESP_LOGE(TAG, "AXP313A begin failed: %s", esp_err_to_name(ret));
    return ret;
}

esp_err_t axp313a_set_camera_power(float dvdd, float avdd_dovdd)
{
    // ALDO(reg 0x16) = DVDD; DLDO(reg 0x17) = AVDD/DOVDD
    // 电压档位 0.5V ~ 3.5V, 步进 100mV: val = (V - 0.5) * 10
    if (dvdd < 0.5f || avdd_dovdd < 0.5f) return ESP_ERR_INVALID_ARG;
    if (dvdd > 3.5f || avdd_dovdd > 3.5f) return ESP_ERR_INVALID_ARG;

    uint8_t aldo = (uint8_t)((dvdd - 0.5f) * 10.0f + 0.5f);
    uint8_t dldo = (uint8_t)((avdd_dovdd - 0.5f) * 10.0f + 0.5f);

    esp_err_t ret;
    ret = axp_write2(0x10, 0x19); // 使能 ALDO + DLDO (DC-DC1 保持关)
    vTaskDelay(pdMS_TO_TICKS(10));
    if (ret == ESP_OK) { ret = axp_write2(0x16, aldo); vTaskDelay(pdMS_TO_TICKS(10)); }
    if (ret == ESP_OK) { ret = axp_write2(0x17, dldo); vTaskDelay(pdMS_TO_TICKS(10)); }
    if (ret == ESP_OK)
        ESP_LOGI(TAG, "Camera power ON: DVDD=%.1fV AVDD/DOVDD=%.1fV", dvdd, avdd_dovdd);
    else
        ESP_LOGE(TAG, "set_camera_power failed: %s", esp_err_to_name(ret));
    return ret;
}

esp_err_t axp313a_disable_power(void)
{
    return axp_write2(0x10, 0x01);
}
