#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "WIFI";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static EventGroupHandle_t s_wifi_event_group;
static bool s_connected = false;
static char s_ip_str[16] = {0};

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGW(TAG, "Disconnected, retrying...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", s_ip_str);
        s_connected = true;
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
    }
}

int wifi_manager_connect(const char *ssid, const char *password, uint32_t timeout_ms)
{
    // NVS 必须先初始化（WiFi 校准数据依赖），损坏时擦除重建
    esp_err_t nret = nvs_flash_init();
    if (nret == ESP_ERR_NVS_NO_FREE_PAGES || nret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nret = nvs_flash_init();
    }
    if (nret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(nret));
        return -1;
    }

    s_wifi_event_group = xEventGroupCreate();

    if (esp_netif_init() != ESP_OK) {
        ESP_LOGE(TAG, "netif init failed");
        return -1;
    }
    esp_err_t lret = esp_event_loop_create_default();
    if (lret != ESP_OK && lret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop create failed");
        return -1;
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed");
        return -1;
    }

    // 注册事件处理
    esp_event_handler_instance_t handler_instance;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, &handler_instance));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, &handler_instance));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;

    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK ||
        esp_wifi_set_config(WIFI_IF_STA, &wifi_config) != ESP_OK ||
        esp_wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "wifi set/start failed");
        return -1;
    }

    ESP_LOGI(TAG, "Connecting to %s...", ssid);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to %s, IP: %s", ssid, s_ip_str);
        return 0;
    }

    ESP_LOGE(TAG, "Failed to connect within %u ms", (unsigned)timeout_ms);
    return -1;
}

bool wifi_manager_is_connected(void)
{
    return s_connected;
}

void wifi_manager_disconnect(void)
{
    esp_wifi_stop();
    s_connected = false;
}

const char* wifi_manager_get_ip(void)
{
    return s_connected ? s_ip_str : "0.0.0.0";
}
