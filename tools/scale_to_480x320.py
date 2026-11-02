#!/usr/bin/env python3
"""把 1280x800 图片等比缩放到 480x320（不形变、不裁掉内容）。

策略：fit-inside —— 按比例缩放到刚好装进 480x320 的框内，再居中贴到 480x320
画布上，上下（或左右）用该图边框均值填充，和 AI 生成图的浅灰背景无缝衔接。

用法：
    python3 tools/scale_to_480x320.py [src_dir] [dst_dir]
默认 src = 生活常见品图片/photos_1280x800_raw
     dst = 生活常见品图片/photos_480x320
"""
import os
import sys
import pathlib

from PIL import Image

W, H = 480, 320
ROOT = pathlib.Path(__file__).resolve().parents[1]
EXTS = ('.jpg', '.jpeg', '.png', '.bmp', '.webp')


def border_color(im):
    """取四条边 1 像素宽的均值作为留白填充色。"""
    rgb = im.convert('RGB')
    w, h = rgb.size
    px = rgb.load()
    rs = gs = bs = n = 0
    for x in range(w):
        r, g, b = px[x, 0]
        rs += r; gs += g; bs += b
        r, g, b = px[x, h - 1]
        rs += r; gs += g; bs += b; n += 2
    for y in range(h):
        r, g, b = px[0, y]
        rs += r; gs += g; bs += b
        r, g, b = px[w - 1, y]
        rs += r; gs += g; bs += b; n += 2
    return (rs // n, gs // n, bs // n)


def scale_one(src, dst):
    with Image.open(src) as im:
        im = im.convert('RGB')
        sw, sh = im.size
        s = min(W / sw, H / sh)
        nw, nh = round(sw * s), round(sh * s)
        if nw > W:
            nw = W
        if nh > H:
            nh = H
        fill = border_color(im)
        small = im.resize((nw, nh), Image.LANCZOS)
        canvas = Image.new('RGB', (W, H), fill)
        canvas.paste(small, ((W - nw) // 2, (H - nh) // 2))
        name = os.path.splitext(os.path.basename(src))[0] + '.jpg'
        out = os.path.join(dst, name)
        canvas.save(out, quality=92, optimize=True, subsampling=0)
        return nw, nh, out


def main():
    src_dir = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / '生活常见品图片' / 'photos_1280x800_raw')
    dst_dir = sys.argv[2] if len(sys.argv) > 2 else str(ROOT / '生活常见品图片' / 'photos_480x320')
    os.makedirs(dst_dir, exist_ok=True)
    files = sorted(f for f in os.listdir(src_dir) if f.lower().endswith(EXTS))
    if not files:
        print(f'!! {src_dir} 里没有图片')
        return 1
    pads = set()
    for i, f in enumerate(files, 1):
        nw, nh, out = scale_one(os.path.join(src_dir, f), dst_dir)
        pads.add((W - nw) // 2 or 0)
        print(f'[{i:3d}/{len(files)}] {f:38s} -> 内容 {nw}x{nh} / 画布 {W}x{H}')
    print(f'\n完成：{len(files)} 张 -> {dst_dir}')
    print(f'统一输出 {W}x{H}，等比缩放无形变，居中留白')
    return 0


if __name__ == '__main__':
    sys.exit(main())
