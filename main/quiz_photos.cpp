// quiz_photos.cpp — 出题图库：从 flash(storage/SPIFFS) 里随机挑一张 480x320 JPEG 刷到 4 寸屏
//
// 图库在编译期由 spiffs_create_partition_image(storage ../spiffs_data) 打成镜像烧进
// 0xc10000 的 storage 分区，挂载在 /spiffs，实际路径 /spiffs/photos/<name>.jpg。
// 图片不进内部 RAM：文件名表和解码缓冲全在 PSRAM，读一张才分配一张的大小（复用）。
#include "quiz_photos.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_spiffs.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "tft_big.h"

static const char *TAG = "QUIZ";
static const char *QUIZ_BASE = "/spiffs";
static const char *QUIZ_DIR  = "/spiffs/photos";
static const int   NAME_LEN  = 24;      // 目录里的文件名都 <=12 字符，留足余量

static char *s_names;                   // PSRAM: count * NAME_LEN
static int   s_count;
static int   s_interval_s = 120;        // 2 分钟换一张
static char  s_last[NAME_LEN];
static int   s_shown;
static SemaphoreHandle_t s_lock;
static uint8_t *s_file;                 // PSRAM: 读文件用，按需长大
static size_t  s_file_cap;

static int cmp_name(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

int quiz_photos_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();

    uint32_t before = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    esp_vfs_spiffs_conf_t cfg = {
        .base_path = QUIZ_BASE,
        .partition_label = "storage",
        .max_files = 2,                 // SPIFFS 每多一个 fd 都要吃内部 RAM，这里只顺序读文件
        .format_if_mount_failed = false,  // 镜像是编译期烧进去的，挂载失败绝不能格式化掉图库
    };
    esp_err_t err = esp_vfs_spiffs_register(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "挂载 /spiffs 失败: %s（storage 分区没烧图库？）", esp_err_to_name(err));
        return -1;
    }
    uint32_t after = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "挂载 /spiffs 完成，内部 RAM 占用 %u 字节（挂载后剩 %u）",
             (unsigned)(before > after ? before - after : 0), (unsigned)after);

    size_t total = 0, used = 0;
    if (esp_spiffs_info("storage", &total, &used) == ESP_OK)
        ESP_LOGI(TAG, "storage 分区 %u 字节，已用 %u（%.1f%%）",
                 (unsigned)total, (unsigned)used, (double)used * 100.0 / (double)total);

    DIR *d = opendir(QUIZ_DIR);
    if (!d) {
        ESP_LOGE(TAG, "打不开 %s", QUIZ_DIR);
        return -1;
    }
    // 先数一遍再装表：目录项名字长度不定，用固定槽位最省事
    int n = 0;
    for (struct dirent *e = readdir(d); e; e = readdir(d)) {
        size_t l = strlen(e->d_name);
        if (l > 4 && !strcmp(e->d_name + l - 4, ".jpg")) n++;
    }
    if (n == 0) { closedir(d); ESP_LOGE(TAG, "%s 里没有 jpg", QUIZ_DIR); return -1; }

    s_names = (char *)heap_caps_malloc((size_t)n * NAME_LEN, MALLOC_CAP_SPIRAM);
    if (!s_names) { closedir(d); ESP_LOGE(TAG, "PSRAM 不够放文件名表"); return -1; }
    rewinddir(d);
    int i = 0;
    for (struct dirent *e = readdir(d); e; e = readdir(d)) {
        size_t l = strlen(e->d_name);
        if (l > 4 && l < NAME_LEN && !strcmp(e->d_name + l - 4, ".jpg")) {
            memcpy(s_names + (size_t)i * NAME_LEN, e->d_name, l + 1);
            i++;
        }
    }
    closedir(d);
    s_count = i;
    qsort(s_names, (size_t)s_count, (size_t)NAME_LEN, cmp_name);
    ESP_LOGI(TAG, "图库就绪：%d 张，间隔 %d 秒，首张 %s",
             s_count, s_interval_s, s_names);
    return s_count;
}

int quiz_photos_count(void) { return s_count; }
int quiz_photos_interval(void) { return s_interval_s; }
const char *quiz_photos_last(void) { return s_last; }
const char *quiz_photos_name(int idx)
{
    if (!s_names || idx < 0 || idx >= s_count) return "";
    return s_names + (size_t)idx * NAME_LEN;
}

int quiz_photos_set_interval(int sec)
{
    if (sec < 2) sec = 2;
    if (sec > 3600) sec = 3600;
    s_interval_s = sec;
    ESP_LOGI(TAG, "换图间隔 -> %d 秒", s_interval_s);
    return s_interval_s;
}

// name=NULL/"rand"/"" -> 随机；否则按文件名（可省 .jpg 后缀）
int quiz_photos_show(const char *name, char *info, size_t sz)
{
    if (info && sz) info[0] = 0;
    if (s_count <= 0) { if (info) snprintf(info, sz, "图库未挂载"); return -1; }

    char pick[NAME_LEN];
    if (!name || !*name || !strcmp(name, "rand")) {
        int k = (int)(esp_random() % (uint32_t)s_count);
        strncpy(pick, s_names + (size_t)k * NAME_LEN, sizeof(pick) - 1);
        pick[sizeof(pick) - 1] = 0;
    } else {
        size_t l = strlen(name);
        if (l >= sizeof(pick)) return -2;
        strcpy(pick, name);
        if (strlen(pick) + 4 < sizeof(pick) && strcmp(pick + strlen(pick) - 4, ".jpg"))
            strcat(pick, ".jpg");
    }

    char path[64];
    snprintf(path, sizeof(path), "%s/%s", QUIZ_DIR, pick);

    // 先量大小再进锁：刷屏一次要 2~3 秒（1MHz 时更久），fopen/fseek 没必要占着锁
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0 || st.st_size > 2 * 1024 * 1024) {
        if (info) snprintf(info, sz, "打不开或大小异常 %s", pick);
        return -4;
    }
    const size_t len = (size_t)st.st_size;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    int rc = -3;
    FILE *f = fopen(path, "rb");
    if (!f) {
        xSemaphoreGive(s_lock);
        if (info) snprintf(info, sz, "打不开 %s", pick);
        return -4;
    }
    rewind(f);
    if (len > s_file_cap) {
        free(s_file);
        s_file = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
        s_file_cap = s_file ? len : 0;
        if (!s_file) {
            fclose(f);
            xSemaphoreGive(s_lock);
            if (info) snprintf(info, sz, "PSRAM 不够(%u)", (unsigned)len);
            return -3;
        }
    }
    size_t got = fread(s_file, 1, len, f);
    fclose(f);
    if (got != len) {
        xSemaphoreGive(s_lock);
        if (info) snprintf(info, sz, "只读到 %u/%u", (unsigned)got, (unsigned)len);
        return -3;
    }

    char jinfo[128];
    int jr = tft_big_show_jpeg(s_file, len, jinfo, sizeof(jinfo));
    if (jr == 0) {
        strncpy(s_last, pick, sizeof(s_last) - 1);
        s_last[sizeof(s_last) - 1] = 0;
        s_shown++;
        rc = 0;
        if (info) snprintf(info, sz, "%s | %s", pick, jinfo);
        ESP_LOGI(TAG, "#%d 出题 %s | %s", s_shown, pick, jinfo);
    } else {
        if (info) snprintf(info, sz, "%s 解码失败 rc=%d %s", pick, jr, jinfo);
        ESP_LOGW(TAG, "%s 上屏失败 rc=%d", pick, jr);
    }
    xSemaphoreGive(s_lock);
    return rc;
}

// 1 秒一跳的循环，这样 /quiz?every= 改间隔能立刻生效
static void quiz_task(void *arg)
{
    char info[192];
    int since = 1 << 30;
    while (true) {
        if (since >= s_interval_s) {
            since = 0;
            quiz_photos_show(nullptr, info, sizeof(info));
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        since++;
    }
}

// 换图任务的栈只能从内部 RAM 拿（这个任务要 fopen/fread 读 SPIFFS，也就是走 flash，
// flash 驱动会临时关 cache，栈放 PSRAM 会在关 cache 期间踩空，所以绝不能用
// xTaskCreateWithCaps + MALLOC_CAP_SPIRAM）。开机流程里越晚建任务，内部 RAM 的
// 最大连续块越小（摄像头 GDMA、WiFi/httpd、模型加载都要吃），所以给一个降档梯子，
// 保证即使排在最后也起得来。
static const int kStackLadder[] = { 6144, 5120, 4096 };

void quiz_photos_start_task(void)
{
    if (s_count <= 0) { ESP_LOGW(TAG, "图库为空，不起换图任务"); return; }

    TaskHandle_t h = nullptr;
    int stack = 0;
    for (int cand : kStackLadder) {
        if (xTaskCreate(quiz_task, "quiz_task", cand, nullptr, 2, &h) == pdPASS) {
            stack = cand;
            break;
        }
        ESP_LOGW(TAG, "quiz_task %d 字节栈起不来，降档重试（内部空闲 %u / 最大块 %u）",
                 cand,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }
    if (!h) {
        ESP_LOGE(TAG, "quiz_task 创建失败：内部 RAM 不足（空闲 %u / 最大块 %u）",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return;
    }
    ESP_LOGI(TAG, "quiz_task 已启动：每 %d 秒随机换一张（栈 %d 字节）", s_interval_s, stack);
}
