#include "ai_client.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "AI_CLIENT";

// 通义千问 API（OpenAI 兼容模式）
#define QWEN_API_URL    "https://api.scnet.cn/api/llm/v1/chat/completions"
#define QWEN_MODEL      "Qwen3.8-Flash"

#define REQ_BUF_SIZE    (16 * 1024)
#define RESP_BUF_SIZE   (128 * 1024)
#define USER_BUF_SIZE   (8 * 1024)

static char *s_api_key = NULL;
static char *s_req_buf = NULL;    // PSRAM
static char *s_resp_buf = NULL;   // PSRAM
static char *s_user_raw = NULL;   // PSRAM，转义前
static char *s_user_json = NULL;  // PSRAM，转义后
static char *s_content = NULL;    // PSRAM，提取的 content
static bool s_initialized = false;

// ========== 内部工具 ==========

// 将任意 UTF-8 文本转义为 JSON 字符串内容（不含首尾引号）
static int json_escape(const char *src, char *dst, size_t dst_size)
{
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)src;
    for (; *p; p++) {
        const char *rep = NULL;
        char esc[8];
        switch (*p) {
            case '"':  rep = "\\\""; break;
            case '\\': rep = "\\\\"; break;
            case '\n': rep = "\\n";  break;
            case '\r': rep = "\\r";  break;
            case '\t': rep = "\\t";  break;
            default:
                if (*p < 0x20) {
                    snprintf(esc, sizeof(esc), "\\u%04x", *p);
                    rep = esc;
                }
                break;
        }
        if (rep) {
            size_t l = strlen(rep);
            if (o + l >= dst_size - 1) return -1;
            memcpy(dst + o, rep, l);
            o += l;
        } else {
            if (o + 1 >= dst_size - 1) return -1;
            dst[o++] = *p;
        }
    }
    dst[o] = '\0';
    return (int)o;
}

// 构建 OpenAI 兼容请求体；user_json 必须是已转义的用户内容（写入 s_req_buf）
static int build_request(const char *system_prompt, const char *user_json,
                         float temperature, int max_tokens, bool json_mode)
{
    int len = snprintf(s_req_buf, REQ_BUF_SIZE,
        "{"
        "\"model\": \"" QWEN_MODEL "\","
        "\"messages\": ["
        "{\"role\":\"system\",\"content\":\"%s\"},"
        "{\"role\":\"user\",\"content\":\"%s\"}"
        "],"
        "\"temperature\": %.2f,"
        "\"max_tokens\": %d%s"
        "}",
        system_prompt, user_json, temperature, max_tokens,
        json_mode ? ",\"response_format\":{\"type\":\"json_object\"}" : "");

    if (len < 0 || len >= REQ_BUF_SIZE) return -1;
    return 0;
}

static int do_api_request(void)
{
    esp_http_client_config_t config = {
        .url = QWEN_API_URL,
        .timeout_ms = 300000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { ESP_LOGE(TAG, "client init fail"); return -1; }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    char auth_header[160];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", s_api_key);
    esp_http_client_set_header(client, "Authorization", auth_header);

    int body_len = (int)strlen(s_req_buf);

    int64_t t0 = esp_timer_get_time();

    // 显式 open/write/read：esp_http_client_perform 会把响应体自动读进内部缓冲，
    // 之后的 read_response 返回 0，导致静默失败。手动流程可稳定分块读取。
    esp_err_t err = esp_http_client_open(client, body_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open fail: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return -1;
    }

    int wrote = esp_http_client_write(client, s_req_buf, body_len);
    if (wrote != body_len) {
        ESP_LOGE(TAG, "write %d/%d bytes", wrote, body_len);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }

    int hret = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (hret < 0 || status != 200) {
        int n = esp_http_client_read(client, s_resp_buf, RESP_BUF_SIZE - 1);
        if (n > 0) s_resp_buf[n] = '\0'; else s_resp_buf[0] = '\0';
        ESP_LOGE(TAG, "API status %d (hret %d): %s", status, hret,
                 n > 0 ? s_resp_buf : "(no body)");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -3;
    }

    int total = 0;
    int read_err = 0;
    while (total < RESP_BUF_SIZE - 1) {
        int n = esp_http_client_read(client, s_resp_buf + total,
                                     RESP_BUF_SIZE - 1 - total);
        if (n < 0) { read_err = 1; break; }
        if (n == 0) break;
        total += n;
        if (esp_http_client_is_complete_data_received(client)) break;
    }
    int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000;

    if (read_err) {
        ESP_LOGE(TAG, "read error after %lldms, %d bytes",
                 (long long)elapsed_ms, total);
    }
    if (total <= 0) {
        ESP_LOGE(TAG, "empty body, %lldms elapsed", (long long)elapsed_ms);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (total <= 0) return -1;
    s_resp_buf[total] = '\0';
    ESP_LOGI(TAG, "HTTP 200, %d bytes in %lldms", total, (long long)elapsed_ms);
    return 0;
}

// 从 ChatCompletion 响应中提取 message.content（复制出来，避免生命周期问题）
static int extract_content(char *out, size_t out_size)
{
    cJSON *root = cJSON_Parse(s_resp_buf);
    if (!root) return -1;

    int ret = -1;
    cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
        cJSON *msg = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(choices, 0), "message");
        cJSON *content = msg ? cJSON_GetObjectItemCaseSensitive(msg, "content") : NULL;
        const char *text = content ? cJSON_GetStringValue(content) : NULL;
        if (text) {
            strncpy(out, text, out_size - 1);
            out[out_size - 1] = '\0';
            ret = 0;
        }
    }
    cJSON_Delete(root);
    return ret;
}

static void fill_problem_from_json(cJSON *item, quiz_problem_t *p)
{
    memset(p, 0, sizeof(*p));
    p->from_ai = true;
    p->difficulty = DIFFICULTY_MEDIUM;

    cJSON *q = cJSON_GetObjectItemCaseSensitive(item, "question");
    if (q && cJSON_IsString(q))
        strncpy(p->question, q->valuestring, sizeof(p->question) - 1);

    // options: 字符串数组
    cJSON *opts = cJSON_GetObjectItemCaseSensitive(item, "options");
    if (opts && cJSON_IsArray(opts)) {
        int n = cJSON_GetArraySize(opts);
        if (n > QUIZ_MAX_OPTIONS) n = QUIZ_MAX_OPTIONS;
        p->option_count = (uint8_t)n;
        for (int i = 0; i < n; i++) {
            cJSON *o = cJSON_GetArrayItem(opts, i);
            if (o && cJSON_IsString(o))
                strncpy(p->options[i], o->valuestring, QUIZ_OPTION_LEN - 1);
        }
    }

    // correct: 索引或答案文本
    cJSON *c = cJSON_GetObjectItemCaseSensitive(item, "correct");
    if (!c) c = cJSON_GetObjectItemCaseSensitive(item, "answer");
    if (c) {
        if (cJSON_IsNumber(c)) {
            int idx = (int)c->valuedouble;
            if (idx < 0) idx = 0;
            if (idx >= p->option_count) idx = p->option_count - 1;
            p->correct_index = (uint8_t)idx;
        } else if (cJSON_IsString(c)) {
            // 匹配选项文本
            p->correct_index = 0;
            for (int i = 0; i < p->option_count; i++) {
                if (strcmp(p->options[i], c->valuestring) == 0) {
                    p->correct_index = i;
                    break;
                }
            }
        }
    }

    // subject 匹配
    cJSON *s = cJSON_GetObjectItemCaseSensitive(item, "subject");
    p->subject = SUBJECT_MATH;
    if (s && cJSON_IsString(s)) {
        static const char *names[] = {"数学", "语文", "英语", "科学", "历史"};
        for (int i = 0; i < SUBJECT_MAX; i++) {
            if (strstr(s->valuestring, names[i])) { p->subject = (subject_t)i; break; }
        }
    }

    // difficulty 匹配
    cJSON *d = cJSON_GetObjectItemCaseSensitive(item, "difficulty");
    if (d && cJSON_IsString(d)) {
        if (strstr(d->valuestring, "easy") || strstr(d->valuestring, "简单"))
            p->difficulty = DIFFICULTY_EASY;
        else if (strstr(d->valuestring, "hard") || strstr(d->valuestring, "困难"))
            p->difficulty = DIFFICULTY_HARD;
    }
}

static uint32_t s_ai_id = 0;

// ========== 公共 API ==========

int ai_client_init(const char *api_key)
{
    if (!api_key || strlen(api_key) == 0) return -1;

    s_api_key = heap_caps_malloc(128, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_req_buf = heap_caps_malloc(REQ_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_resp_buf = heap_caps_malloc(RESP_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_user_raw = heap_caps_malloc(USER_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_user_json = heap_caps_malloc(USER_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_content = heap_caps_malloc(RESP_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_api_key || !s_req_buf || !s_resp_buf || !s_user_raw || !s_user_json || !s_content) {
        ESP_LOGE(TAG, "PSRAM alloc failed");
        return -1;
    }
    strncpy(s_api_key, api_key, 127);
    s_api_key[127] = '\0';

    s_initialized = true;
    ESP_LOGI(TAG, "AI client ready (model: %s)", QWEN_MODEL);
    return 0;
}

bool ai_client_is_connected(void)
{
    return s_initialized;
}

int ai_generate_quiz_batch(quiz_problem_t *problems, int max_count, int *out_count,
                           subject_t subject, const char *hot_topic_hint)
{
    if (!s_initialized || !problems || max_count <= 0 || !out_count) return -1;
    *out_count = 0;

    const char *subject_names[] = {"数学", "语文", "英语", "科学", "历史"};
    char subject_desc[256];
    if (subject < SUBJECT_MAX)
        snprintf(subject_desc, sizeof(subject_desc), "%s", subject_names[subject]);
    else
        snprintf(subject_desc, sizeof(subject_desc), "%s",
                 "按分布混合：数学约10%、英语约10%、语文约20%、"
                 "科学约30%（以物理通识为主，如力/光/电/热/能量）、"
                 "历史约30%（中国史与世界史常识，如朝代/四大发明/重大事件与年代）");

    int off = snprintf(s_user_raw, USER_BUF_SIZE,
        "出%d道适合初中生及以上水平的选择题（可含高中基础题），学科要求：%s。要求："
        "1.每题4个选项，有且仅有一个正确答案；"
        "2.题目多样不重复，难度以初中为主、可含高中基础（中等与困难搭配）；"
        "3.多为知识性、通识性题目，数学计算题只占少数；"
        "4.只返回JSON对象，格式："
        "{\"questions\":[{\"question\":\"题干\",\"options\":[\"选1\",\"选2\",\"选3\",\"选4\"],"
        "\"correct\":0,\"subject\":\"数学\",\"difficulty\":\"medium\"}]}。"
        "correct是正确选项的下标(0-3)。",
        max_count, subject_desc);

    if (hot_topic_hint && strlen(hot_topic_hint) > 0) {
        snprintf(s_user_raw + off, USER_BUF_SIZE - off,
                 "额外要求：%s", hot_topic_hint);
    }

    if (json_escape(s_user_raw, s_user_json, USER_BUF_SIZE) < 0) {
        ESP_LOGE(TAG, "prompt escape/overflow"); return -1;
    }

    if (build_request("你是中小学出题AI，只输出JSON，不输出其他内容。",
                      s_user_json, 0.9, 8000, true) != 0) {
        ESP_LOGE(TAG, "request build/overflow"); return -1;
    }

    ESP_LOGI(TAG, "Requesting %d quizzes...", max_count);
    int ret = do_api_request();
    if (ret != 0) return ret;

    if (extract_content(s_content, RESP_BUF_SIZE) != 0) {
        ESP_LOGE(TAG, "no message.content in response: %.120s", s_resp_buf);
        return -2;
    }

    cJSON *root = cJSON_Parse(s_content);
    if (!root) {
        ESP_LOGE(TAG, "Parse fail: %.200s", s_content);
        return -2;
    }

    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "questions");
    // 兼容直接返回数组的情况
    if (!arr && cJSON_IsArray(root)) arr = root;

    int n = 0;
    if (arr && cJSON_IsArray(arr)) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, arr) {
            if (n >= max_count) break;
            quiz_problem_t *p = &problems[n];
            fill_problem_from_json(item, p);
            // 有效性校验：必须有题干、选项数>=2
            size_t real_opts = 0;
            for (int i = 0; i < QUIZ_MAX_OPTIONS; i++)
                if (p->options[i][0]) real_opts++;
            if (real_opts < 2 || strlen(p->question) == 0 || strlen(p->options[0]) == 0) {
                continue;
            }
            p->option_count = (uint8_t)real_opts;
            if (p->correct_index >= p->option_count) p->correct_index = 0;
            p->id = ++s_ai_id;
            ESP_LOGI(TAG, "AI quiz [%s] %s (ans=%c)",
                     quiz_subject_name(p->subject), p->question, 'A' + p->correct_index);
            n++;
        }
    }
    cJSON_Delete(root);

    *out_count = n;
    return (n > 0) ? 0 : -2;
}

int ai_generate_quiz(quiz_problem_t *problem, subject_t subject, difficulty_t difficulty)
{
    (void)difficulty; // AI 自行把控难度搭配
    quiz_problem_t tmp[1];
    int count = 0;
    int ret = ai_generate_quiz_batch(tmp, 1, &count, subject, NULL);
    if (ret == 0 && count > 0) {
        *problem = tmp[0];
        return 0;
    }
    return ret != 0 ? ret : -2;
}

int ai_analyze_performance(const char *records_json, ai_analysis_t *analysis)
{
    if (!s_initialized || !records_json || !analysis) return -1;
    memset(analysis, 0, sizeof(*analysis));

    int off = snprintf(s_user_raw, USER_BUF_SIZE,
        "以下是学生答题记录JSON数组，字段：subject(学科)、question(题目)、"
        "chosen(所选选项)、correct(正确答案下标)、is_correct(是否答对)。"
        "请分析薄弱点与优势，只返回JSON："
        "{\"summary\":\"总体评价(50字内)\",\"weak_points\":\"薄弱点(80字内)\","
        "\"strengths\":\"优势(60字内)\",\"suggestion\":\"建议(60字内)\"}\n\n"
        "记录：\n");
    strncat(s_user_raw, records_json, USER_BUF_SIZE - off - 1);

    if (json_escape(s_user_raw, s_user_json, USER_BUF_SIZE) < 0) return -1;

    if (build_request("你是教育分析AI，只输出JSON。", s_user_json, 0.3, 600, true) != 0) return -1;

    ESP_LOGI(TAG, "Requesting analysis...");
    int ret = do_api_request();
    if (ret != 0) return ret;

    if (extract_content(s_content, RESP_BUF_SIZE) != 0) return -2;

    cJSON *root = cJSON_Parse(s_content);
    if (!root) return -2;

    cJSON *sum  = cJSON_GetObjectItemCaseSensitive(root, "summary");
    cJSON *weak = cJSON_GetObjectItemCaseSensitive(root, "weak_points");
    cJSON *str  = cJSON_GetObjectItemCaseSensitive(root, "strengths");
    cJSON *sug  = cJSON_GetObjectItemCaseSensitive(root, "suggestion");

    if (sum && cJSON_IsString(sum))
        strncpy(analysis->summary, sum->valuestring, sizeof(analysis->summary) - 1);
    if (weak && cJSON_IsString(weak))
        strncpy(analysis->weak_points, weak->valuestring, sizeof(analysis->weak_points) - 1);
    if (str && cJSON_IsString(str))
        strncpy(analysis->strengths, str->valuestring, sizeof(analysis->strengths) - 1);
    if (sug && cJSON_IsString(sug))
        strncpy(analysis->suggestion, sug->valuestring, sizeof(analysis->suggestion) - 1);

    analysis->valid = (strlen(analysis->summary) > 0);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Analysis: %s", analysis->summary);
    return analysis->valid ? 0 : -2;
}
