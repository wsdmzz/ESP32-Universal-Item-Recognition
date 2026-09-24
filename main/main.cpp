// ============================================================
// ESP32-S3 端侧摄像头物体识别 (FireBeetle 2 ESP32-S3 + OV3660)
// 流程: AXP313A 摄像头供电 -> DVP 采集 QVGA RGB888
//       -> ESP-DL YOLO11n-320 int8 (COCO-80) 本地推理 -> TFT 显示
// 出题机相关组件保留在仓库中，不再由本程序调用。
// ============================================================
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "driver/i2c.h"
#include "esp_camera.h"
#include "img_converters.h"
#include "axp313a.h"
#include "tft_display.h"
#include "coco_detect.hpp"

static const char *TAG = "CAMDET";

// ===== FireBeetle 2 ESP32-S3 (V1.1) CAM DVP 引脚（官方 wiki CAM 接口表）=====
#define CAM_PIN_XCLK   45
#define CAM_PIN_PCLK    5
#define CAM_PIN_VSYNC   6
#define CAM_PIN_HREF   42
// 8bit 总线使用传感器 D2-D9: config.pin_d0..d7 依次对应 D2..D9
#define CAM_DATA_D2    39   // pin_d0
#define CAM_DATA_D3    40   // pin_d1
#define CAM_DATA_D4    41   // pin_d2
#define CAM_DATA_D5     4   // pin_d3
#define CAM_DATA_D6     7   // pin_d4
#define CAM_DATA_D7     8   // pin_d5
#define CAM_DATA_D8    46   // pin_d6
#define CAM_DATA_D9    48   // pin_d7
// I2C: AXP313A 与摄像头 SCCB 共用一组引脚 (SDA=1, SCL=2)
#define I2C_SDA_PIN     1
#define I2C_SCL_PIN     2
#define I2C_PORT        I2C_NUM_0

#define DET_MAX_OBJS    8    // 屏幕最多展示的目标数
#define SCORE_THR       0.35f

// COCO-80 类别中文名（顺序与模型输出 category 一致）
static const char *COCO_CN[80] = {
    "人", "自行车", "汽车", "摩托车", "飞机", "公共汽车", "火车", "卡车", "船", "红绿灯",
    "消防栓", "停车标志", "停车计时器", "长凳", "鸟", "猫", "狗", "马", "羊", "牛",
    "大象", "熊", "斑马", "长颈鹿", "背包", "雨伞", "手提包", "领带", "行李箱", "飞盘",
    "滑雪板", "单板滑雪板", "球", "风筝", "球棒", "棒球手套", "滑板", "冲浪板", "网球拍", "瓶子",
    "酒杯", "杯子", "叉子", "刀", "勺子", "碗", "香蕉", "苹果", "三明治", "橙子",
    "西兰花", "胡萝卜", "热狗", "比萨", "甜甜圈", "蛋糕", "椅子", "沙发", "盆栽", "床",
    "餐桌", "马桶", "电视", "笔记本电脑", "鼠标", "遥控器", "键盘", "手机", "微波炉", "烤箱",
    "烤面包机", "水槽", "冰箱", "书", "钟表", "花瓶", "剪刀", "玩具熊", "吹风机", "牙刷",
};

// ---------- 共用 I2C 总线（AXP313A + 摄像头 SCCB）----------
static esp_err_t shared_i2c_init(void)
{
    i2c_config_t conf = {};
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = I2C_SDA_PIN;
    conf.scl_io_num = I2C_SCL_PIN;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = 100000;
    esp_err_t ret = i2c_param_config(I2C_PORT, &conf);
    if (ret == ESP_OK) ret = i2c_driver_install(I2C_PORT, conf.mode, 0, 0, 0);
    return ret;
}

// 总线扫描：打印所有应答的 7 位地址（诊断用）
static void i2c_scan(void)
{
    ESP_LOGI(TAG, "I2C scan:");
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true); // 检查ACK
        i2c_master_stop(cmd);
        esp_err_t ret = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(100));
        i2c_cmd_link_delete(cmd);
        if (ret == ESP_OK) ESP_LOGI(TAG, "  found device @0x%02X", addr);
    }
}

// ---------- 摄像头初始化 ----------
static esp_err_t camera_init(void)
{
    camera_config_t config = {};
    config.pin_pwdn     = -1;   // PWDN 已下拉
    config.pin_reset    = -1;   // RST 已上拉到 DOVDD
    config.pin_xclk     = CAM_PIN_XCLK;
    config.pin_sccb_sda = -1;   // 复用已装好的 I2C 端口
    config.pin_sccb_scl = -1;
    config.sccb_i2c_port = I2C_PORT;

    config.pin_d7 = CAM_DATA_D9;
    config.pin_d6 = CAM_DATA_D8;
    config.pin_d5 = CAM_DATA_D7;
    config.pin_d4 = CAM_DATA_D6;
    config.pin_d3 = CAM_DATA_D5;
    config.pin_d2 = CAM_DATA_D4;
    config.pin_d1 = CAM_DATA_D3;
    config.pin_d0 = CAM_DATA_D2;

    config.pin_vsync = CAM_PIN_VSYNC;
    config.pin_href  = CAM_PIN_HREF;
    config.pin_pclk  = CAM_PIN_PCLK;

    config.xclk_freq_hz = 20000000;
    config.ledc_timer   = LEDC_TIMER_0;
    config.ledc_channel = LEDC_CHANNEL_0;

    config.pixel_format = PIXFORMAT_YUV422;   // S3 DVP 不支持 RGB888 直出，采集后转 RGB888
    config.frame_size   = FRAMESIZE_QVGA;     // 320x240, 与屏幕 1:1
    config.jpeg_quality = 12;
    config.fb_count     = 2;
    config.fb_location  = CAMERA_FB_IN_PSRAM;
    config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s", esp_err_to_name(err));
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    ESP_LOGI(TAG, "Camera sensor PID=0x%04x (%s)", s ? (unsigned)s->id.PID : 0u,
             (s && s->id.PID == (uint16_t)OV3660_PID) ? "OV3660" : "other");
    if (s) {
        s->set_vflip(s, 1);      // 摄像头模组安装方向：上下翻转
        s->set_brightness(s, 1); // 适当提亮
    }
    return ESP_OK;
}

// ---------- YUV422 -> RGB888（修正 R/B 通道顺序）----------
// esp32-camera 的 fmt2rgb888 对 YUV422 输出的是 B,G,R 顺序，而 ESP-DL
// (DL_IMAGE_PIX_TYPE_RGB888) 与本项目的 rgb565() 都按 R,G,B 读取，
// 导致红蓝互换：屏幕显示偏色、且模型输入颜色错乱使识别率下降。
// 这里按标准 BT.601 全范围整数近似自行转换，输出真正的 R,G,B。
static inline void yuv422_to_rgb888(const uint8_t *src, size_t src_len, uint8_t *dst)
{
    size_t pairs = src_len / 4;   // 每 4 字节 Y0 U Y1 V 对应 2 像素
    for (size_t i = 0; i < pairs; i++) {
        int y0 = src[i * 4 + 0];
        int u  = (int)src[i * 4 + 1] - 128;
        int y1 = src[i * 4 + 2];
        int v  = (int)src[i * 4 + 3] - 128;
        for (int p = 0; p < 2; p++) {
            int yy = p ? y1 : y0;
            int r = yy + ((1436 * v) >> 10);              // 1.402
            int g = yy - ((352 * u + 731 * v) >> 10);     // -0.344 -0.714
            int b = yy + ((1814 * u) >> 10);              // 1.772
            *dst++ = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
            *dst++ = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g));
            *dst++ = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
        }
    }
}

// 统一帧转 RGB888：YUV422 走自研正确顺序，其余格式回退库函数
static bool frame_to_rgb888(const camera_fb_t *fb, uint8_t *dst)
{
    if (fb->format == PIXFORMAT_YUV422) {
        yuv422_to_rgb888(fb->buf, fb->len, dst);
        return true;
    }
    return fmt2rgb888(fb->buf, fb->len, fb->format, dst);
}

// ---------- 识别主任务 ----------
// ---------- 共享识别结果（detect_task 写，view_task 读）----------
static SemaphoreHandle_t s_res_mux;
static tft_object_t s_res_objs[DET_MAX_OBJS];
static int s_res_count = 0;
static char s_res_status[64] = "识别引擎启动中...";

static void result_publish(const tft_object_t *objs, int count, const char *status)
{
    xSemaphoreTake(s_res_mux, portMAX_DELAY);
    s_res_count = count > DET_MAX_OBJS ? DET_MAX_OBJS : count;
    if (objs && s_res_count > 0)
        memcpy(s_res_objs, objs, sizeof(tft_object_t) * s_res_count);
    snprintf(s_res_status, sizeof(s_res_status), "%s", status);
    xSemaphoreGive(s_res_mux);
}

// ---------- 视频推流任务：采集 -> RGB565 -> 刷屏 + 叠加检测框 ----------
static void view_task(void *arg)
{
    static uint8_t *rgb888 = nullptr;    // YUV422 转 RGB888 暂存
    static uint8_t *be565  = nullptr;    // 推流帧（大端 RGB565）
    rgb888 = (uint8_t *)heap_caps_malloc(320 * 240 * 3, MALLOC_CAP_SPIRAM);
    be565  = (uint8_t *)heap_caps_malloc(320 * 240 * 2, MALLOC_CAP_SPIRAM);
    if (!rgb888 || !be565) {
        ESP_LOGE(TAG, "view buffers alloc failed");
        vTaskDelete(NULL);
        return;
    }

    tft_object_t local_objs[DET_MAX_OBJS];
    char local_status[64];

    while (1) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            vTaskDelay(pdMS_TO_TICKS(30)); // 推理任务占用帧缓冲时属正常，稍候
            continue;
        }

        bool ok = frame_to_rgb888(fb, rgb888);
        esp_camera_fb_return(fb);

        if (!ok) {
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }

        // RGB888 -> 大端 RGB565
        int npix = 320 * 240;
        for (int i = 0; i < npix; i++) {
            uint16_t c = rgb565(rgb888[i * 3], rgb888[i * 3 + 1], rgb888[i * 3 + 2]);
            be565[i * 2]     = c >> 8;
            be565[i * 2 + 1] = c & 0xFF;
        }

        // 取最新识别结果（拷贝后立即释放锁，绘制不与推理互等）
        xSemaphoreTake(s_res_mux, portMAX_DELAY);
        int n = s_res_count;
        memcpy(local_objs, s_res_objs, sizeof(local_objs));
        snprintf(local_status, sizeof(local_status), "%s", s_res_status);
        xSemaphoreGive(s_res_mux);

        tft_render_view(be565, local_objs, n, local_status);
    }
}

// ---------- 识别任务：周期性推理，更新共享结果 ----------
static void detect_task(void *arg)
{
    COCODetect *detect = new COCODetect(COCODetect::YOLO11N_320_S8_V1, false);
    detect->set_score_thr(SCORE_THR);
    ESP_LOGI(TAG, "Model loaded");

    static tft_object_t objs[DET_MAX_OBJS];
    uint8_t *rgb_buf = (uint8_t *)heap_caps_malloc(320 * 240 * 3, MALLOC_CAP_SPIRAM);
    if (!rgb_buf) {
        ESP_LOGE(TAG, "rgb_buf alloc failed");
        result_publish(NULL, 0, "内存不足");
        vTaskDelete(NULL);
        return;
    }

    char status[64];
    while (1) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGW(TAG, "Camera capture failed");
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (!frame_to_rgb888(fb, rgb_buf)) {
            ESP_LOGE(TAG, "fmt2rgb888 failed");
            esp_camera_fb_return(fb);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        int fw = fb->width, fh = fb->height;
        esp_camera_fb_return(fb);   // 尽早归还，推流任务继续使用

        dl::image::img_t img = {};
        img.data     = rgb_buf;
        img.width    = fw;
        img.height   = fh;
        img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888;

        int64_t t0 = esp_timer_get_time();
        auto &results = detect->run(img);
        int64_t dt_us = esp_timer_get_time() - t0;

        int n = 0;
        // 图像坐标 320x240 与屏幕 1:1，直接作为整屏叠加框坐标
        for (const auto &res : results) {
            if (n >= DET_MAX_OBJS) break;
            if (res.box.size() != 4) continue;
            int x1 = res.box[0] * 320 / fw;
            int y1 = res.box[1] * 240 / fh;
            int x2 = res.box[2] * 320 / fw;
            int y2 = res.box[3] * 240 / fh;
            objs[n].label   = (res.category >= 0 && res.category < 80)
                                  ? COCO_CN[res.category] : "未知";
            objs[n].percent = (int)(res.score * 100.0f + 0.5f);
            objs[n].x = x1;
            objs[n].y = y1;
            objs[n].w = x2 - x1;
            objs[n].h = y2 - y1;
            ESP_LOGI(TAG, "obj: %s %d%% box[%d,%d,%d,%d]",
                     objs[n].label, objs[n].percent, x1, y1, x2, y2);
            n++;
        }

        snprintf(status, sizeof(status), "识别到 %d 个物体 | 耗时 %.1fs",
                 n, dt_us / 1e6);
        result_publish(objs, n, status);

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ---------- 入口 ----------
extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32-S3 端侧物体识别 ===");

    s_res_mux = xSemaphoreCreateMutex();

    // 1) 共用 I2C + AXP313A 摄像头供电
    ESP_ERROR_CHECK(shared_i2c_init());
    i2c_scan();
    if (axp313a_begin(I2C_PORT, AXP313A_I2C_ADDR) != ESP_OK)
        ESP_LOGE(TAG, "AXP313A not responding - camera may have no power");
    if (axp313a_set_camera_power(1.5f, 2.8f) != ESP_OK) // OV3660: DVDD/AVDD
        ESP_LOGW(TAG, "Camera power set failed");
    vTaskDelay(pdMS_TO_TICKS(200)); // 等电源稳定

    // 2) 屏幕
    if (tft_display_init() != 0) {
        ESP_LOGE(TAG, "Display init failed! Halting.");
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    tft_show_status("摄像头识别", "正在初始化 OV3660...");

    // 3) 摄像头
    if (camera_init() != ESP_OK) {
        tft_show_status("摄像头初始化失败", "请检查 CAM 排线是否插好");
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // 4) 加载模型（推流启动前先显示加载提示，避免黑屏等待）
    tft_show_status("加载识别模型", "ESP-DL YOLO11n INT8 -> PSRAM");

    // 5) 双任务：推流(高优先级) + 周期推理
    xTaskCreate(view_task,   "view_task",   8192,  NULL, 5, NULL);
    xTaskCreate(detect_task, "detect_task", 16384, NULL, 3, NULL);
}
