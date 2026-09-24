#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化并连接 WiFi（STA 模式）
 * @param ssid WiFi名称
 * @param password WiFi密码
 * @param timeout_ms 超时时间（毫秒）
 * @return 0=成功, -1=失败
 */
int wifi_manager_connect(const char *ssid, const char *password, uint32_t timeout_ms);

/**
 * 检查是否已连接
 */
bool wifi_manager_is_connected(void);

/**
 * 断开连接
 */
void wifi_manager_disconnect(void);

/**
 * 获取 IP 地址字符串
 */
const char* wifi_manager_get_ip(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_MANAGER_H
