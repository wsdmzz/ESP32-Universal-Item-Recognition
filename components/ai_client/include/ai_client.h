#ifndef AI_CLIENT_H
#define AI_CLIENT_H

#include "quiz_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * AI 分析结果结构体
 */
typedef struct {
    char summary[512];          // 总体评价
    char weak_points[512];      // 薄弱点
    char strengths[256];        // 优势
    char suggestion[256];       // 学习建议
    bool valid;                 // 是否有效
} ai_analysis_t;

/**
 * 初始化 AI 客户端
 * @param api_key 通义千问 API Key
 * @return 0=成功
 */
int ai_client_init(const char *api_key);

/**
 * 请求 AI 批量生成选择题（用于预取入库）
 * @param problems 输出题目数组
 * @param max_count 数组容量
 * @param out_count 实际生成数量
 * @param subject 学科（SUBJECT_MAX 表示随机混合）
 * @param hot_topic_hint 可选：热点/个性化提示（如"结合近期学生易错的分数运算"）
 * @return 0=成功, -1=网络错误, -2=解析错误, -3=API错误
 */
int ai_generate_quiz_batch(quiz_problem_t *problems, int max_count, int *out_count,
                           subject_t subject, const char *hot_topic_hint);

/**
 * 请求 AI 生成单道选择题（内部调用批量接口取第一题）
 */
int ai_generate_quiz(quiz_problem_t *problem, subject_t subject, difficulty_t difficulty);

/**
 * 请求 AI 分析答题记录
 * @param records_json JSON格式的答题记录数组
 * @param analysis 输出分析结果
 * @return 0=成功
 */
int ai_analyze_performance(const char *records_json, ai_analysis_t *analysis);

/**
 * 检查 AI 客户端是否可用
 */
bool ai_client_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif // AI_CLIENT_H
