#!/usr/bin/env python3
"""cap_sheet.py — 把「摄像头拍到的帧」和「题库原图」并排拼成一张对照表。

肉眼看一眼胜过读十行统计：反光、色偏、糊、取景框跑偏，在这张图里一目了然。
左列是端侧真正喂给模型的裁剪帧（cap_collect 存的），右列是它本该长成的样子。

用法:
  python3 tools/items103/cap_sheet.py [cap_test 目录] [输出 png]
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[2]
GALLERY = ROOT / 'spiffs_data' / 'photos'
CELL = (300, 200)          # 每格显示尺寸（原样放大，不做插值美化）


def main():
    d = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / 'models/items103/cap_test')
    out = Path(sys.argv[2] if len(sys.argv) > 2 else '/tmp/cap_sheet.png')
    caps = sorted(d.glob('*.jpg'))
    if not caps:
        print(f'{d} 下没有帧，先跑 cap_collect.py')
        return 1
    W, H = CELL
    sheet = Image.new('RGB', (W * 2 + 12, (H + 22) * len(caps) + 6), (24, 24, 28))
    dr = ImageDraw.Draw(sheet)
    for i, p in enumerate(caps):
        stem = p.stem.split('__')[0]
        y = 3 + i * (H + 22)
        cap = Image.open(p).convert('RGB').resize((W, H), Image.NEAREST)
        sheet.paste(cap, (3, y))
        g = GALLERY / f'{stem}.jpg'
        if g.exists():
            src = Image.open(g).convert('RGB').resize((W, H), Image.LANCZOS)
        else:
            src = Image.new('RGB', (W, H), (60, 0, 0))
        sheet.paste(src, (W + 9, y))
        a = np.asarray(cap, np.float32)
        lo, hi = np.percentile(a.mean(2), [2, 98])
        dr.text((6, y + H + 4),
                f'{stem}  黑底{lo:.0f} 白顶{hi:.0f} 对比{hi/max(lo,1):.1f}:1',
                fill=(230, 230, 230))
    dr.text((6, 0), '', fill=(255, 255, 255))
    sheet.save(out)
    print(f'左=实拍帧 右=原图  ->  {out}  ({len(caps)} 行)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
