#ifndef TFT_DISPLAY_H
#define TFT_DISPLAY_H

#include <stdint.h>
#include <stdbool.h>
#include "quiz_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化 TFT 显示屏（ST7789 via GDI）
 * @return 0=成功, -1=失败
 */
int tft_display_init(void);

/**
 * 清屏
 */
void tft_clear(uint16_t color);

/**
 * 显示一道选择题（学科标签 + 题干 + A/B/C/D 选项）
 * @param problem 题目
 * @param mode_line 底部状态行（可为NULL），如 "AI出题" / "离线题库" / "WiFi: OK"
 */
void tft_show_quiz(const quiz_problem_t *problem, const char *mode_line);

/**
 * 显示作答反馈：高亮正确/错误选项
 * @param problem 题目
 * @param chosen 用户选择（-1=未作答）
 */
void tft_show_feedback(const quiz_problem_t *problem, int8_t chosen);

/**
 * 显示加载/状态画面
 */
void tft_show_status(const char *title, const char *detail);

/**
 * 显示 AI 学习分析报告（滚动分页简化为四段）
 */
void tft_show_analysis(const char *summary, const char *weak_points,
                       const char *strengths, const char *suggestion);

/**
 * 物体识别结果（摄像头检测应用使用）
 */
typedef struct {
    const char *label;   // 中文类别名
    int percent;         // 置信度 0-100
    int x, y, w, h;      // 已在屏幕坐标系内的检测框
} tft_object_t;

/**
 * 显示一帧目标检测结果：检测框 + 类别标签 + 列表 + 状态行
 * @param objs        结果数组（可为 NULL）
 * @param count       结果数量
 * @param status_line 底部状态行（可为 NULL）
 */
void tft_show_objects(const tft_object_t *objs, int count, const char *status_line);

/**
 * 实时推流画面：整帧 RGB565(大端字节序, 320x240) + 检测框叠加 + 底部状态行
 * @param frame_be565  320*240*2 字节，每像素先高字节
 * @param objs/count   检测框（坐标为整屏 320x240）
 * @param status_line  底部状态行（可为 NULL）
 */
void tft_render_view(const uint8_t *frame_be565, const tft_object_t *objs,
                     int count, const char *status_line);

/**
 * RGB565 颜色转换（运行时）
 */
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

/**
 * RGB565 编译期常量表达式（可用于 static 初始化）
 */
#define RGB565C(r, g, b)  ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

// 预定义颜色
#define COLOR_BLACK   RGB565C(0, 0, 0)
#define COLOR_WHITE   RGB565C(255, 255, 255)
#define COLOR_RED     RGB565C(255, 60, 60)
#define COLOR_GREEN   RGB565C(60, 220, 60)
#define COLOR_BG      RGB565C(20, 20, 35)
#define COLOR_TEXT    RGB565C(240, 240, 240)
#define COLOR_TAG     RGB565C(100, 200, 255)
#define COLOR_HINT    RGB565C(130, 130, 140)
#define COLOR_OK      RGB565C(100, 255, 100)
#define COLOR_BAD     RGB565C(255, 120, 120)
#define COLOR_SEL     RGB565C(255, 200, 80)

#ifdef __cplusplus
}
#endif

#endif // TFT_DISPLAY_H
