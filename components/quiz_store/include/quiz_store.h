#ifndef QUIZ_STORE_H
#define QUIZ_STORE_H

#include "quiz_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STORE_MAX_QUEUE     10000 // 题库队列最大缓存题数（约4MB PSRAM）
#define STORE_MAX_HISTORY   100  // 答题记录最大条数

/**
 * 初始化存储（挂载 SPIFFS、加载题库队列）
 * @return 0=成功
 */
int quiz_store_init(void);

/**
 * 强制把当前题库队列落盘（补货循环结束后调用）
 */
void quiz_store_flush(void);

/**
 * 批量入队 AI 生成的题目（超出容量时丢弃最新的）
 * @return 实际入队数量
 */
int quiz_store_enqueue(const quiz_problem_t *problems, int count);

/**
 * 从题库队列取出一题（FIFO）
 * @return true=成功取到, false=队列空
 */
bool quiz_store_dequeue(quiz_problem_t *problem);

/**
 * 当前队列剩余题数
 */
int quiz_store_queue_count(void);

/**
 * 记录一次答题
 */
int quiz_store_record_answer(const quiz_problem_t *problem, int8_t chosen_index);

/**
 * 导出最近未上传的答题记录为 JSON 数组字符串（供 AI 分析）
 * @param buf 输出缓冲
 * @param buf_size 缓冲大小
 * @return 导出的记录条数, -1=失败
 */
int quiz_store_export_history_json(char *buf, size_t buf_size);

/**
 * 标记历史记录已上传（导出并成功分析后调用，清空已导出部分）
 */
void quiz_store_history_uploaded(void);

/**
 * 待上传记录条数
 */
int quiz_store_pending_records(void);

#ifdef __cplusplus
}
#endif

#endif // QUIZ_STORE_H
