#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "sdkconfig.h"

#include "quiz_types.h"
#include "math_generator.h"
#include "tft_display.h"
#include "wifi_manager.h"
#include "ai_client.h"
#include "quiz_store.h"

static const char *TAG = "MAIN";

#define QUEUE_LOW_WATER     400    // 题库低于此数量触发 AI 补货
#define QUEUE_TARGET        10000  // 补货目标题数
#define AI_STACK_SIZE       8192
#define QUIZ_STACK_SIZE     8192

// ================= 可替换的答题输入接口 =================
// 翻页改为"答题门控"：quiz_task 无限等待作答结果，答完才取下一题。
// 输入方式待定：将来接入按键/触摸/App 后，在其事件回调里调用
// quiz_input_submit(选项下标) 即可；或整体覆盖 quiz_input_wait 强符号。
static SemaphoreHandle_t s_answer_sem;
static volatile int s_answer_index = -1;

// 供输入设备/驱动调用：提交所选选项下标(0-3)，唤醒出题主循环
void quiz_input_submit(int option_index)
{
    s_answer_index = option_index;
    if (s_answer_sem) xSemaphoreGive(s_answer_sem);
}

__attribute__((weak)) int quiz_input_wait(const quiz_problem_t *problem, uint32_t timeout_ms)
{
    (void)problem;
    (void)timeout_ms; // 答题门控：不再按时间翻页
    xSemaphoreTake(s_answer_sem, portMAX_DELAY); // 等待作答
    int idx = s_answer_index;
    s_answer_index = -1;
    return idx;
}

// ================= 全局状态 =================
static SemaphoreHandle_t s_refill_sem;       // 通知 AI 任务补货
static volatile bool s_ai_available = false; // API Key 有效且初始化成功
static char s_hot_hint[128] = {0};          // 最近一次分析得出的针对性提示
static volatile bool s_show_analysis_pending = false;
static ai_analysis_t s_last_analysis;       // 最近一次完整分析结果

static bool api_key_configured(void)
{
    return strlen(CONFIG_QWEN_API_KEY) > 10;
}

// ================= AI 后台任务：补货 + 分析 =================
static void ai_task(void *arg)
{
    quiz_problem_t *batch = malloc(sizeof(quiz_problem_t) * CONFIG_QUIZ_BATCH_SIZE);
    char *hist_buf = malloc(24 * 1024);
    if (!batch || !hist_buf) {
        ESP_LOGE(TAG, "ai_task alloc failed");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        // 等待补货/分析通知（最长 60s 兜底轮询一次）
        xSemaphoreTake(s_refill_sem, pdMS_TO_TICKS(60000));

        if (!s_ai_available || !wifi_manager_is_connected()) {
            continue;
        }

        // 1) 题库补货：低于低水位则连续补到目标题数
        while (quiz_store_queue_count() < QUEUE_TARGET) {
            int before = quiz_store_queue_count();
            int got = 0;
            int ret = ai_generate_quiz_batch(batch, CONFIG_QUIZ_BATCH_SIZE, &got,
                                             SUBJECT_MAX, /*随机混合，可换指定学科*/
                                             s_hot_hint[0] ? s_hot_hint : NULL);
            if (ret != 0 || got <= 0) {
                ESP_LOGW(TAG, "AI refill failed: %d (queue=%d)", ret, before);
                break; // 网络/额度异常，等下一轮通知或60s轮询再试
            }
            int added = quiz_store_enqueue(batch, got);
            ESP_LOGI(TAG, "AI refilled %d problems (queue=%d/%d)",
                     added, quiz_store_queue_count(), QUEUE_TARGET);
            if (added == 0) break; // 全是重复题，稍后分析出新提示再试
        }
        quiz_store_flush(); // 补货会话结束（成功或中断）都落盘一次

        // 2) 达到阈值则做学习分析
        if (quiz_store_pending_records() >= CONFIG_QUIZ_ANALYSIS_EVERY && hist_buf) {
            int n = quiz_store_export_history_json(hist_buf, 24 * 1024);
            if (n > 0) {
                static ai_analysis_t analysis;
                int aret = ai_analyze_performance(hist_buf, &analysis);
                if (aret == 0 && analysis.valid) {
                    quiz_store_history_uploaded();
                    snprintf(s_hot_hint, sizeof(s_hot_hint),
                             "该学生薄弱点：%.75s。请多安排相关练习。", analysis.weak_points);
                    s_last_analysis = analysis;
                    s_show_analysis_pending = true;
                    ESP_LOGI(TAG, "Analysis ready, hint: %s", s_hot_hint);
                }
            }
        }
    }
}

// ================= 出题主循环任务 =================
static void quiz_task(void *arg)
{
    quiz_problem_t problem;

    while (1) {
        // 分析报告展示（答题间隙插入一次）
        if (s_show_analysis_pending) {
            s_show_analysis_pending = false;
            tft_show_analysis(s_last_analysis.summary, s_last_analysis.weak_points,
                              s_last_analysis.strengths, s_last_analysis.suggestion);
            vTaskDelay(pdMS_TO_TICKS(8000));
        }

        // 取题优先级：本地题库队列 -> 在线直连生成 -> 离线规则生成
        bool have = quiz_store_dequeue(&problem);
        if (!have && s_ai_available && wifi_manager_is_connected()) {
            tft_show_status("AI 生成中...", "题库已空，正在向云端请求新题");
            int got = 0;
            int ret = ai_generate_quiz_batch(&problem, 1, &got, SUBJECT_MAX,
                                             s_hot_hint[0] ? s_hot_hint : NULL);
            have = (ret == 0 && got > 0);
        }
        if (!have) {
            // 离线兜底：本地规则题库
            static const difficulty_t w[] = {
                DIFFICULTY_EASY, DIFFICULTY_MEDIUM, DIFFICULTY_MEDIUM,
                DIFFICULTY_MEDIUM, DIFFICULTY_HARD
            }; // 近似 20/60/20 加权
            difficulty_t diff = w[esp_random() % (sizeof(w) / sizeof(w[0]))];
            have = (quiz_generate(&problem, diff) == 0);
            if (!have) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }

        // 队列水位低 -> 通知 AI 任务后台补货（不阻塞显示）
        if (quiz_store_queue_count() < QUEUE_LOW_WATER && s_ai_available) {
            xSemaphoreGive(s_refill_sem);
        }

        // 状态行：仅显示题库余量
        char mode[64];
        snprintf(mode, sizeof(mode), "题库:%d", quiz_store_queue_count());
        tft_show_quiz(&problem, mode);

        // 等待作答：答完才刷新下一题（输入方式待定，见 quiz_input_submit）
        int chosen = quiz_input_wait(&problem, 0);
        if (chosen >= 0) {
            quiz_store_record_answer(&problem, (int8_t)chosen);
            tft_show_feedback(&problem, (int8_t)chosen);
            vTaskDelay(pdMS_TO_TICKS(2500));
        }
    }
}

// ================= 入口 =================
void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32-S3 AI Quiz Device ===");

    quiz_generator_init();

    if (quiz_store_init() != 0)
        ESP_LOGE(TAG, "quiz_store init failed, queue persistence off");

    if (tft_display_init() != 0) {
        ESP_LOGE(TAG, "Display init failed! Halting.");
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    tft_show_status("AI 出题机启动中", NULL);

    // WiFi（失败不致命：走离线题库模式）
    if (strlen(CONFIG_WIFI_SSID) > 0 && strcmp(CONFIG_WIFI_SSID, "my_wifi") != 0) {
        int wret = wifi_manager_connect(CONFIG_WIFI_SSID, CONFIG_WIFI_PASSWORD, 15000);
        if (wret != 0)
            ESP_LOGW(TAG, "WiFi connect failed, offline mode");
    } else {
        ESP_LOGW(TAG, "WiFi not configured (menuconfig), offline mode");
    }

    // AI 客户端（API Key 未配置则跳过，纯离线运行）
    if (api_key_configured()) {
        if (ai_client_init(CONFIG_QWEN_API_KEY) == 0)
            s_ai_available = true;
    } else {
        ESP_LOGW(TAG, "QWEN_API_KEY not set (idf.py menuconfig -> Quiz AI 配置)");
    }
    ESP_LOGI(TAG, "AI: %s, WiFi: %s, 题库缓存: %d 题",
             s_ai_available ? "ON" : "OFF",
             wifi_manager_is_connected() ? "ON" : "OFF",
             quiz_store_queue_count());

    s_refill_sem = xSemaphoreCreateBinary();
    s_answer_sem = xSemaphoreCreateBinary();

    xTaskCreate(ai_task,   "ai_task",   AI_STACK_SIZE,   NULL, 3, NULL);
    xTaskCreate(quiz_task, "quiz_task", QUIZ_STACK_SIZE, NULL, 4, NULL);
}
