#ifndef MATH_GENERATOR_H
#define MATH_GENERATOR_H

#include "quiz_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化本地题库生成器（随机种子等）
 */
void quiz_generator_init(void);

/**
 * 生成一道随机学科选择题（本地规则，离线可用）
 * @param problem 输出题目
 * @param difficulty 难度
 * @return 0=成功, -1=失败
 */
int quiz_generate(quiz_problem_t *problem, difficulty_t difficulty);

/**
 * 生成指定学科的选择题
 */
int quiz_generate_subject(quiz_problem_t *problem, subject_t subject, difficulty_t difficulty);

#ifdef __cplusplus
}
#endif

#endif // MATH_GENERATOR_H
