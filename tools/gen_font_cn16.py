#!/usr/bin/env python3
"""
生成 16x16 中文点阵字库头文件 font_cn16_table.h

用法:
    pip install pillow
    python3 tools/gen_font_cn16.py --font /path/to/wqy-microhei.ttc \
        --output components/tft_display/font_cn16_table.h

    # 没有现成字体时可用系统任意中文 TTF/TTC，例如:
    #   文泉驿微米黑 / Noto Sans CJK SC / 思源黑体

字模格式:
    每个码点 32 字节（16 行 x 每行 2 字节，高位在前，像素=1 表示点亮）
    表按 unicode 升序排列，供固件二分查找。

覆盖字符集: ASCII 可见字符(0x21-0x7E) + 3500 常用汉字 + 常用中文标点。
"""

import argparse
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    print("需要 Pillow: pip install pillow", file=sys.stderr)
    sys.exit(1)

# 现代汉语常用字表前 3500 字太长的话这里内置一份精简字符集
COMMON_CN_PUNCT = "、，。！？；：、“”‘’（）—…·《》【】"
ASCII_RANGE = [ord(c) for c in
               "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
               "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~ "]

def load_charset(args):
    codes = set(ASCII_RANGE)
    for ch in COMMON_CN_PUNCT:
        codes.add(ord(ch))
    if args.charset_file:
        with open(args.charset_file, "r", encoding="utf-8") as f:
            for line in f:
                for ch in line.strip():
                    if ord(ch) >= 0x80:
                        codes.add(ord(ch))
    else:
        # 内置精简常用字（教育场景高频 500 字，可自行扩充或指定 --charset-file）
        builtin = (
            "的一是在不了有和人这中大为上个国我以要他时来用们生到作地于出就分对成会"
            "可主发年动同工也能下过子说产种面而方后多定行学法所民得经十三之进着等部"
            "度家电力里如水化高自二理起小物现实加量都两体制机当使点从业本去把性好应"
            "开它合还因由其些然前外天政四日那社义事平形相全表间样与关各重新线内数正"
            "心反你明看原又么利比或但质气第向道命此变条只没结解问意建月公无系军很情"
            "者最立代想已通并提直题党程展五果料象员革位入常文总次品式活设及管特件长"
            "求老头基资边流路级少图山统接知较将组见计别她手角期根论运农指几九区强放"
            "决西被干做必战先回则任取据处队南给色光门即保治北造百规热领七海口东导器"
            "压志世金增争济阶油思术极交受联什认六共权收证改清己美再采转更单风切打白"
            "教速花带安场身车例真务具万每目至达走积示议声报斗完类八离华名确才科张信"
            "马节话米整空元况今集温传土许步群广石记需段研界拉林律叫且究观越织装影算"
            "低持音众书布复容儿须际商非验连断深难近矿千周委素技备半办青省列习响约支"
        )
        for ch in builtin:
            codes.add(ord(ch))
    return sorted(codes)

def render_glyph(font, code):
    """渲染单个字符为 16x16 双字节行点阵：先大画布取墨迹包围盒，再居中贴入 16x16 单元格"""
    bmp = bytearray(32)
    ch = chr(code)
    img = Image.new("L", (48, 48), 0)
    d = ImageDraw.Draw(img)
    try:
        d.text((16, 16), ch, fill=255, font=font)
    except Exception:
        return bmp
    bbox = img.getbbox()
    if not bbox:
        return bmp
    ink = img.crop(bbox)
    w, h = ink.size
    if w > 16 or h > 16:  # 极少数超宽字模，等比缩回
        s = min(16 / w, 16 / h)
        ink = ink.resize((max(1, int(w * s)), max(1, int(h * s))), Image.LANCZOS)
        w, h = ink.size
    ox = (16 - w) // 2
    oy = (16 - h) // 2
    px = ink.load()
    for row in range(16):
        bits = 0
        r = row - oy
        if 0 <= r < h:
            for col in range(16):
                c = col - ox
                if 0 <= c < w and px[c, r] >= 90:
                    bits |= 0x8000 >> col
        bmp[row * 2] = (bits >> 8) & 0xFF
        bmp[row * 2 + 1] = bits & 0xFF
    return bmp

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", required=True, help="TTF/TTC 字体路径")
    ap.add_argument("--size", type=int, default=16)
    ap.add_argument("--output", required=True)
    ap.add_argument("--charset-file", help="额外字符集txt文件（每行任意汉字）")
    args = ap.parse_args()

    try:
        font = ImageFont.truetype(args.font, args.size)
    except Exception as e:
        print(f"字体加载失败: {e}", file=sys.stderr)
        sys.exit(1)

    codes = load_charset(args)
    entries = []
    for code in codes:
        if code == ord(" "):
            continue  # 空格无需字模
        bmp = render_glyph(font, code)
        if all(b == 0 for b in bmp):
            continue  # 跳过无法渲染的
        entries.append((code, bmp))

    with open(args.output, "w", encoding="utf-8") as f:
        f.write("/* 自动生成 - gen_font_cn16.py，请勿手工编辑 */\n")
        f.write("#ifndef FONT_CN16_TABLE_H\n#define FONT_CN16_TABLE_H\n\n")
        f.write("typedef struct { unsigned int code; unsigned char bmp[32]; } cn16_glyph_t;\n\n")
        f.write(f"static const cn16_glyph_t g_cn16_table[] = {{\n")
        for code, bmp in entries:
            hexs = ",".join(f"0x{b:02X}" for b in bmp)
            f.write(f"    {{0x{code:04X}, {{{hexs}}}}},\n")
        f.write("};\n\n")
        f.write(f"static const unsigned int g_cn16_table_count = {len(entries)};\n\n")
        f.write("#endif\n")

    print(f"生成完成: {len(entries)} 字模 -> {args.output}")
    print(f"预计 Flash 占用: {len(entries)*36} 字节")

if __name__ == "__main__":
    main()
