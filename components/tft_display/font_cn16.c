#include "font_cn16.h"
#include <string.h>

/*
 * 16x16 中文点阵字库查找表。
 *
 * 数据由 tools/gen_font_cn16.py 从 TTF 字体生成，输出到 font_cn16_table.h，
 * 要求按 unicode 码点升序排列（生成脚本已保证）。
 *
 * 表为空时（未生成字库），所有汉字显示为方块占位，ASCII 不受影响。
 */
#if __has_include("font_cn16_table.h")
#include "font_cn16_table.h"
// 生成头需定义:
//   typedef struct { uint32_t code; uint8_t bmp[32]; } cn16_glyph_t;
//   static const cn16_glyph_t g_cn16_table[];
//   static const uint32_t g_cn16_table_count;
#else
typedef struct { uint32_t code; uint8_t bmp[32]; } cn16_glyph_t;
static const cn16_glyph_t g_cn16_table[] = {{0, {0}}};
static const uint32_t g_cn16_table_count = 0;
#endif

bool font_cn16_lookup(uint32_t unicode, uint8_t out[32])
{
    if (g_cn16_table_count == 0) return false;

    // 二分查找（表按 code 升序）
    uint32_t lo = 0, hi = g_cn16_table_count - 1;
    while (lo <= hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (g_cn16_table[mid].code == unicode) {
            memcpy(out, g_cn16_table[mid].bmp, 32);
            return true;
        }
        if (mid == 0) break;
        if (g_cn16_table[mid].code > unicode) hi = mid - 1;
        else lo = mid + 1;
    }
    return false;
}
