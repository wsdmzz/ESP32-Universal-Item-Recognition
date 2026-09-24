// AXP313A 电源管理芯片驱动（FireBeetle 2 ESP32-S3 V1.1 板载）
// 基于 DFRobot 官方 esp_idf 驱动整理：全局变量静态化、增加返回值。
// 使用旧版 driver/i2c.h 接口（与 esp32-camera SCCB 共用同一 I2C 端口）。
// 调用方须先自行安装 I2C 驱动，再 axp313a_begin()。
#pragma once

#include <stdint.h>
#include "stdbool.h"
#include "esp_err.h"
#include "driver/i2c.h"

// DFRobot 官方库默认地址（7位），驱动内部左移1位写
#define AXP313A_I2C_ADDR 0x36

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化：记录端口/地址并做基础配置（PL PWR KEY 使能寄存器）
 */
esp_err_t axp313a_begin(i2c_port_t i2c_num, uint8_t addr);

/**
 * @brief 使能摄像头三路电源
 * @param dvdd       ALDO 输出，OV3660 DVDD 典型 1.5V
 * @param avdd_dovdd DLDO 输出，OV3660 AVDD/DOVDD 典型 2.8V
 */
esp_err_t axp313a_set_camera_power(float dvdd, float avdd_dovdd);

/**
 * @brief 关闭 ALDO/DLDO 输出
 */
esp_err_t axp313a_disable_power(void);

#ifdef __cplusplus
}
#endif
