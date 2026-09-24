#ifndef QUIZ_TYPES_H
#define QUIZ_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 学科类型
 */
typedef enum {
    SUBJECT_MATH = 0,      // 数学
    SUBJECT_CHINESE,       // 语文
    SUBJECT_ENGLISH,       // 英语
    SUBJECT_SCIENCE,       // 科学/自然
    SUBJECT_HISTORY,       // 历史/常识
    SUBJECT_MAX
} subject_t;

/**
 * 题目难度级别
 */
typedef enum {
    DIFFICULTY_EASY = 0,
    DIFFICULTY_MEDIUM,
    DIFFICULTY_HARD,
    DIFFICULTY_MAX
} difficulty_t;

#define QUIZ_MAX_OPTIONS   4     // 每题最多4个选项
#define QUIZ_OPTION_LEN    48    // 单个选项最大长度

/**
 * 题目结构体（选择题格式）
 */
typedef struct {
    char question[192];                 // 题目内容
    char options[QUIZ_MAX_OPTIONS][QUIZ_OPTION_LEN]; // A/B/C/D 选项
    uint8_t option_count;               // 实际选项数量（2-4）
    uint8_t correct_index;              // 正确选项下标（0=A）
    subject_t subject;                  // 学科
    difficulty_t difficulty;            // 难度
    uint32_t id;                        // 题目编号
    bool from_ai;                       // true=AI生成, false=本地种子题库
} quiz_problem_t;

/**
 * 答题记录
 */
typedef struct {
    uint32_t problem_id;                // 题目编号
    subject_t subject;                  // 学科
    uint8_t difficulty;                 // 难度
    int8_t chosen_index;                // 用户选择的选项（-1=跳过）
    uint8_t correct_index;              // 正确答案
    bool is_correct;                    // 是否答对
    uint32_t timestamp;                 // 时间戳（秒）
} quiz_answer_record_t;

/**
 * 获取学科名称字符串（中文）
 */
const char* quiz_subject_name(subject_t subject);

#ifdef __cplusplus
}
#endif

#endif // QUIZ_TYPES_H
