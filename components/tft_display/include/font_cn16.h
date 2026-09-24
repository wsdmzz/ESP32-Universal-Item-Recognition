#ifndef FONT_CN16_H
#define FONT_CN16_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 查询 16x16 中文字模。
 * @param unicode 码点
 * @param out 输出 32 字节点阵（每行2字节高位在前，共16行）
 * @return true=找到字模
 *
 * 字模表在 font_cn16_table.h 中，由 tools/gen_font_cn16.py 从 TTF 字体
 * 渲染生成（覆盖约3500常用汉字 + 标点）。
 */
bool font_cn16_lookup(uint32_t unicode, uint8_t out[32]);

#ifdef __cplusplus
}
#endif

#endif // FONT_CN16_H
