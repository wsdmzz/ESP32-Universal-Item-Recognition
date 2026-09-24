#include "quiz_store.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "QUIZ_STORE";

#define QUEUE_FILE    "/data/queue.jsonl"
#define QUEUE_FILE_OLD "/data/queue.json"   // 旧版全量 JSON，加载时顺手删除
#define HISTORY_FILE  "/data/history.json"
#define HDR_BUF_SIZE  (48 * 1024)

// ---- 题库队列（内存环形数组 + 文件持久化）----
static quiz_problem_t *s_queue = NULL;   // PSRAM
static int s_head = 0;                    // 出队位置
static int s_size = 0;                    // 当前数量
static int s_dirty_pops = 0;              // 距上次持久化已出队题数（避免每5s重写大文件）

// ---- 答题记录（带题干文本，供 AI 分析）----
typedef struct {
    uint32_t problem_id;
    uint8_t subject;
    uint8_t difficulty;
    int8_t chosen_index;
    uint8_t correct_index;
    bool is_correct;
    uint32_t timestamp;
    char question[96];
} store_record_t;

static store_record_t *s_history = NULL;  // PSRAM
static int s_history_count = 0;           // 总条数（环形）
static int s_pending_start = 0;           // 未上传的起始位置（环形索引）

static uint32_t now_sec(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000ULL);
}

// ========== 持久化 ==========

static cJSON* problem_to_json(const quiz_problem_t *p)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "question", p->question);
    cJSON *opts = cJSON_AddArrayToObject(o, "options");
    for (int i = 0; i < p->option_count; i++)
        cJSON_AddItemToArray(opts, cJSON_CreateString(p->options[i]));
    cJSON_AddNumberToObject(o, "correct", p->correct_index);
    cJSON_AddNumberToObject(o, "subject", p->subject);
    cJSON_AddNumberToObject(o, "difficulty", p->difficulty);
    cJSON_AddNumberToObject(o, "id", p->id);
    cJSON_AddBoolToObject(o, "ai", p->from_ai);
    return o;
}

static bool problem_from_json(const cJSON *o, quiz_problem_t *p)
{
    memset(p, 0, sizeof(*p));
    const cJSON *q = cJSON_GetObjectItemCaseSensitive(o, "question");
    const cJSON *opts = cJSON_GetObjectItemCaseSensitive(o, "options");
    if (!q || !cJSON_IsString(q) || !opts || !cJSON_IsArray(opts)) return false;

    strncpy(p->question, q->valuestring, sizeof(p->question) - 1);

    int n = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, opts) {
        if (n >= QUIZ_MAX_OPTIONS || !cJSON_IsString(item)) break;
        strncpy(p->options[n], item->valuestring, QUIZ_OPTION_LEN - 1);
        n++;
    }
    if (n < 2) return false;
    p->option_count = n;

    const cJSON *c = cJSON_GetObjectItemCaseSensitive(o, "correct");
    p->correct_index = (c && cJSON_IsNumber(c)) ? (uint8_t)c->valueint : 0;
    if (p->correct_index >= p->option_count) p->correct_index = 0;

    const cJSON *s = cJSON_GetObjectItemCaseSensitive(o, "subject");
    p->subject = (s && cJSON_IsNumber(s) && s->valueint < SUBJECT_MAX)
                 ? (subject_t)s->valueint : SUBJECT_MATH;

    const cJSON *d = cJSON_GetObjectItemCaseSensitive(o, "difficulty");
    p->difficulty = (d && cJSON_IsNumber(d) && d->valueint < DIFFICULTY_MAX)
                    ? (difficulty_t)d->valueint : DIFFICULTY_MEDIUM;

    const cJSON *id = cJSON_GetObjectItemCaseSensitive(o, "id");
    p->id = (id && cJSON_IsNumber(id)) ? (uint32_t)id->valuedouble : 0;

    const cJSON *ai = cJSON_GetObjectItemCaseSensitive(o, "ai");
    p->from_ai = ai ? cJSON_IsTrue(ai) : true;
    return true;
}

// 单题写为一行 JSON（流式，避免为10000题构建整棵 cJSON 树）
static bool write_problem_line(FILE *f, const quiz_problem_t *p)
{
    cJSON *o = problem_to_json(p);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!s) return false;
    fputs(s, f);
    fputc('\n', f);
    free(s);
    return true;
}

// 全量重写队列文件（在补货结束/每200次出队时调用，10000题约3MB）
static bool save_queue(void)
{
    FILE *f = fopen(QUEUE_FILE, "wb");
    if (!f) { ESP_LOGE(TAG, "open %s fail", QUEUE_FILE); return false; }
    for (int i = 0; i < s_size; i++) {
        int idx = (s_head + i) % STORE_MAX_QUEUE;
        if (!write_problem_line(f, &s_queue[idx])) break;
    }
    fclose(f);
    return true;
}

// 追加写（入队新题时增量落盘，开销与题数成正比）
static bool append_queue(const quiz_problem_t *problems, int count)
{
    if (count <= 0) return true;
    FILE *f = fopen(QUEUE_FILE, "ab");
    if (!f) return save_queue(); // 文件缺失则全量重建
    for (int i = 0; i < count; i++) write_problem_line(f, &problems[i]);
    fclose(f);
    return true;
}

static bool load_queue(void)
{
    remove(QUEUE_FILE_OLD); // 旧格式作废

    FILE *f = fopen(QUEUE_FILE, "rb");
    if (!f) return true; // 首次运行，空队列

    s_head = 0; s_size = 0;
    char line[1024];
    int skipped = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        // 无换行说明该行超长：丢弃剩余部分保持行对齐
        if (len > 0 && line[len - 1] != '\n') {
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n') {}
            skipped++;
            continue;
        }
        if (len <= 1) continue;
        cJSON *o = cJSON_Parse(line);
        if (!o) { skipped++; continue; }
        if (s_size < STORE_MAX_QUEUE && problem_from_json(o, &s_queue[s_size]))
            s_size++;
        else
            skipped++;
        cJSON_Delete(o);
    }
    fclose(f);
    ESP_LOGI(TAG, "Queue loaded: %d problems (%d skipped)", s_size, skipped);
    return true;
}

static bool save_history(void)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < s_history_count; i++) {
        const store_record_t *r = &s_history[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", r->problem_id);
        cJSON_AddNumberToObject(o, "subject", r->subject);
        cJSON_AddStringToObject(o, "question", r->question);
        cJSON_AddNumberToObject(o, "difficulty", r->difficulty);
        cJSON_AddNumberToObject(o, "chosen", r->chosen_index);
        cJSON_AddNumberToObject(o, "correct", r->correct_index);
        cJSON_AddBoolToObject(o, "is_correct", r->is_correct);
        cJSON_AddNumberToObject(o, "ts", r->timestamp);
        cJSON_AddItemToArray(arr, o);
    }
    char *str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!str) return false;

    FILE *f = fopen(HISTORY_FILE, "wb");
    if (!f) { free(str); return false; }
    fwrite(str, 1, strlen(str), f);
    fclose(f);
    free(str);
    return true;
}

static bool load_history(void)
{
    FILE *f = fopen(HISTORY_FILE, "rb");
    if (!f) return true;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 64 * 1024) { fclose(f); return true; }

    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return false; }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);

    cJSON *arr = cJSON_Parse(buf);
    free(buf);
    if (!arr || !cJSON_IsArray(arr)) { if (arr) cJSON_Delete(arr); return true; }

    s_history_count = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        if (s_history_count >= STORE_MAX_HISTORY) break;
        store_record_t *r = &s_history[s_history_count];
        memset(r, 0, sizeof(*r));
        const cJSON *v;
        v = cJSON_GetObjectItemCaseSensitive(item, "id");
        if (v) r->problem_id = (uint32_t)v->valuedouble;
        v = cJSON_GetObjectItemCaseSensitive(item, "question");
        if (v && cJSON_IsString(v)) strncpy(r->question, v->valuestring, sizeof(r->question) - 1);
        v = cJSON_GetObjectItemCaseSensitive(item, "difficulty");
        if (v) r->difficulty = (uint8_t)v->valueint;
        v = cJSON_GetObjectItemCaseSensitive(item, "subject");
        if (v && cJSON_IsNumber(v)) r->subject = (uint8_t)v->valueint;
        v = cJSON_GetObjectItemCaseSensitive(item, "chosen");
        if (v) r->chosen_index = (int8_t)v->valueint;
        v = cJSON_GetObjectItemCaseSensitive(item, "correct");
        if (v) r->correct_index = (uint8_t)v->valueint;
        v = cJSON_GetObjectItemCaseSensitive(item, "is_correct");
        if (v) r->is_correct = cJSON_IsTrue(v);
        v = cJSON_GetObjectItemCaseSensitive(item, "ts");
        if (v) r->timestamp = (uint32_t)v->valuedouble;
        s_history_count++;
    }
    cJSON_Delete(arr);
    s_pending_start = s_history_count; // 重启后旧记录视为已上传
    ESP_LOGI(TAG, "History loaded: %d records", s_history_count);
    return true;
}

// ========== 公共 API ==========

int quiz_store_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/data",
        .partition_label = "storage",
        .max_files = 8,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
        return -1;
    }

    s_queue = heap_caps_malloc(sizeof(quiz_problem_t) * STORE_MAX_QUEUE,
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_history = heap_caps_malloc(sizeof(store_record_t) * STORE_MAX_HISTORY,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_queue || !s_history) {
        ESP_LOGE(TAG, "PSRAM alloc failed");
        return -1;
    }

    load_queue();
    load_history();
    return 0;
}

// 队列中是否已有相同题干（AI 跨批次会出重复题，入队前去重）
static bool queue_contains(const char *question)
{
    for (int i = 0; i < s_size; i++) {
        int idx = (s_head + i) % STORE_MAX_QUEUE;
        if (strcmp(s_queue[idx].question, question) == 0) return true;
    }
    return false;
}

int quiz_store_enqueue(const quiz_problem_t *problems, int count)
{
    int added = 0, dups = 0;
    FILE *f = fopen(QUEUE_FILE, "ab"); // 增量追加，避免每次重写3MB
    for (int i = 0; i < count; i++) {
        if (s_size >= STORE_MAX_QUEUE) {
            ESP_LOGW(TAG, "Queue full, dropped %d problems", count - i);
            break;
        }
        if (queue_contains(problems[i].question)) { dups++; continue; }
        int idx = (s_head + s_size) % STORE_MAX_QUEUE;
        s_queue[idx] = problems[i];
        if (f) write_problem_line(f, &problems[i]);
        s_size++;
        added++;
    }
    if (f) fclose(f);
    if (dups > 0) ESP_LOGI(TAG, "Dropped %d duplicate problems", dups);
    if (added > 0 && !f) save_queue(); // 追加失败则全量重建
    if (added > 0) s_dirty_pops = 0;
    return added;
}

void quiz_store_flush(void)
{
    s_dirty_pops = 0;
    save_queue();
}

bool quiz_store_dequeue(quiz_problem_t *problem)
{
    if (s_size <= 0) return false;
    *problem = s_queue[s_head];
    s_head = (s_head + 1) % STORE_MAX_QUEUE;
    s_size--;
    // 出队只动内存游标；每 200 题才全量重写一次，控制 flash 磨损
    if (++s_dirty_pops >= 200) {
        s_dirty_pops = 0;
        save_queue();
    }
    return true;
}

int quiz_store_queue_count(void)
{
    return s_size;
}

int quiz_store_record_answer(const quiz_problem_t *problem, int8_t chosen_index)
{
    if (!problem) return -1;

    // 环形满时淘汰最旧（同时调整 pending_start）
    if (s_history_count >= STORE_MAX_HISTORY) {
        memmove(&s_history[0], &s_history[1], sizeof(store_record_t) * (STORE_MAX_HISTORY - 1));
        s_history_count = STORE_MAX_HISTORY - 1;
        if (s_pending_start > 0) s_pending_start--;
    }

    store_record_t *r = &s_history[s_history_count++];
    memset(r, 0, sizeof(*r));
    r->problem_id = problem->id;
    r->subject = problem->subject;
    r->difficulty = problem->difficulty;
    r->chosen_index = chosen_index;
    r->correct_index = problem->correct_index;
    r->is_correct = (chosen_index == (int8_t)problem->correct_index);
    r->timestamp = now_sec();
    strncpy(r->question, problem->question, sizeof(r->question) - 1);

    save_history();
    ESP_LOGI(TAG, "Record #%u [%s] chosen=%c correct=%c => %s (pending=%d)",
             (unsigned)r->problem_id, quiz_subject_name((subject_t)r->subject),
             chosen_index >= 0 ? 'A' + chosen_index : '-',
             'A' + r->correct_index,
             r->is_correct ? "OK" : "MISS",
             s_history_count - s_pending_start);
    return 0;
}

int quiz_store_export_history_json(char *buf, size_t buf_size)
{
    cJSON *arr = cJSON_CreateArray();
    int n = 0;
    for (int i = s_pending_start; i < s_history_count; i++) {
        const store_record_t *r = &s_history[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "subject", quiz_subject_name((subject_t)r->subject));
        cJSON_AddStringToObject(o, "question", r->question);
        cJSON_AddNumberToObject(o, "chosen", r->chosen_index);
        cJSON_AddNumberToObject(o, "correct", r->correct_index);
        cJSON_AddBoolToObject(o, "is_correct", r->is_correct);
        cJSON_AddItemToArray(arr, o);
        n++;
    }
    char *str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!str) return -1;
    strncpy(buf, str, buf_size - 1);
    buf[buf_size - 1] = '\0';
    free(str);
    return n;
}

void quiz_store_history_uploaded(void)
{
    s_pending_start = s_history_count;
}

int quiz_store_pending_records(void)
{
    return s_history_count - s_pending_start;
}
