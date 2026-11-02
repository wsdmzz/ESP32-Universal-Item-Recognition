#!/usr/bin/env python3
"""把 augmented/ 的 480x320 屏幕模拟增强图转成 320x240（与端侧摄像头帧同尺寸）。

链路依据（用户确认）：新屏幕 480x320，但摄像头输入固定 320x240，
esp-dl 端侧把整帧 320x240 非等比拉伸到 224x224 —— 这条不变。
新旧屏幕的差别只在「屏幕内容在 4:3 摄像头帧里的构图」：
  - FILL：非等比拉满 320x240 —— 模拟旧 320x240 屏占满视野（现在就能测）。
  - FIT ：等比缩放置入 320x240、左右留边 —— 模拟新 480x320 (3:2) 屏
          被 4:3 摄像头拍摄的真实构图（屏幕到货后主用）。
每张源图同时产出 fill + fit 两份，两种屏幕链路都覆盖。

输入: augmented/<类>/*.jpg (480x320)
输出: models/items103/raw_aug_240/<类>/<原名>__fill.jpg / __fit.jpg
"""
from pathlib import Path
import random
from PIL import Image, ImageFilter
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / 'augmented'
DST = ROOT / 'models' / 'items103' / 'raw_aug_240'

TW, TH = 320, 240          # 端侧摄像头帧尺寸 (FRAMESIZE_QVGA)

random.seed(42)
np.random.seed(42)


def to_fill(img):
    """非等比拉满 320x240 —— 与端侧「整帧直接拉伸」几何完全一致。"""
    return img.resize((TW, TH), Image.BILINEAR)


def to_fit(img):
    """等比缩放居中 + 四周留边 —— 模拟 3:2 屏幕在 4:3 摄像头帧内的实拍构图。"""
    w, h = img.size
    scale = min(TW / w, TH / h) * random.uniform(0.80, 1.0)
    nw, nh = max(1, int(w * scale)), max(1, int(h * scale))
    fg = img.resize((nw, nh), Image.BILINEAR)

    mode = random.random()
    if mode < 0.45:
        # 边缘延展：最接近「屏幕内容延伸到边框外」的实拍观感
        arr = np.array(img.resize((TW, TH), Image.BILINEAR))
        canvas = Image.fromarray(arr)
    elif mode < 0.75:
        # 随机纯色背景（桌面/墙面/黑边框）
        g = random.randint(30, 220)
        jitter = lambda: max(0, min(255, g + random.randint(-25, 25)))
        canvas = Image.new('RGB', (TW, TH), (jitter(), jitter(), jitter()))
    else:
        # 模糊放大的自身作背景（屏幕余光/虚化背景）
        canvas = img.resize((TW, TH), Image.BILINEAR).filter(
            ImageFilter.GaussianBlur(radius=random.uniform(8, 20)))

    canvas.paste(fg, ((TW - nw) // 2, (TH - nh) // 2))
    return canvas


def main():
    classes = sorted(d.name for d in SRC.iterdir() if d.is_dir())
    assert len(classes) == 103, f'期望 103 类，实际 {len(classes)}'
    total = 0
    for ci, cls in enumerate(classes, 1):
        out_dir = DST / cls
        out_dir.mkdir(parents=True, exist_ok=True)
        files = sorted(list((SRC / cls).glob('*.jpg')) + list((SRC / cls).glob('*.png')))
        assert files, f'类目录为空: {cls}'
        for f in files:
            img = Image.open(f).convert('RGB')
            # JPEG 存盘顺带模拟摄像头有损压缩
            to_fill(img).save(out_dir / f'{f.stem}__fill.jpg',
                              quality=random.randint(78, 95))
            to_fit(img).save(out_dir / f'{f.stem}__fit.jpg',
                             quality=random.randint(78, 95))
            total += 2
        print(f'[{ci:3d}/103] {cls:14s} {len(files)} -> {len(files)*2}', flush=True)
    print(f'\ndone: {total} images -> {DST}')


if __name__ == '__main__':
    main()
