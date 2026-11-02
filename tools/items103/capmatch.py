#!/usr/bin/env python3
"""capmatch.py — 给「摄像头拍到的屏幕帧」找它是题库里的哪一张，不靠出题顺序。

为什么要它：/quiz?n=X 只是把 X 送上屏，端侧那一轮推理可能正在进行中、吃的是上一张；
按「我出了什么题」来命名采到的帧就会整体错位（实测一批 14 张只有 1 张对得上，
肉眼比对确认画面是「上一题」）。所以名字要让画面内容自己说话。

做法：把实拍帧和 103 张原图都转成「降采样 + 百分位摊平 + 高通 + 单位化」的特征图，
再在 ±SHIFT 像素的平移范围内搜最大相关。
  - 百分位摊平：反光是抬黑底的加性雾（实测黑底 88、白顶 212，对比度只剩 2.4:1），
    不先摊开的话高通出来的基本是噪声。
  - 高通：减掉一次模糊，把剩下的电平/色偏也一起去掉，只留构图。
  - 平移搜索：找屏框每帧会差一两像素，不搜平移相关值会掉到噪声里。

用法:
  python3 tools/items103/capmatch.py [目录]        # 核对目录里每张帧说是哪张
"""
import sys
from pathlib import Path

import cv2
import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
GALLERY = ROOT / 'spiffs_data' / 'photos'
SIG = (96, 63)          # 特征图尺寸（96/63 = 1.524 ≈ 屏幕框宽高比）
SHIFT = 5              # 平移搜索半径（特征图像素）


def signature(rgb):
    """HxWx3 uint8 -> 单位化高通图。"""
    g = np.asarray(Image.fromarray(rgb).convert('L').resize(SIG, Image.BILINEAR),
                   np.float32)
    lo, hi = np.percentile(g, [1, 99])
    g = np.clip((g - lo) / max(hi - lo, 8.0), 0, 1) * 255.0
    hp = g - cv2.GaussianBlur(g, (0, 0), 3.0)
    hp -= hp.mean()
    return (hp / (hp.std() + 1e-6)).astype(np.float32)


class Gallery:
    def __init__(self, path=GALLERY):
        self.names = [p.stem for p in sorted(path.glob('*.jpg'))]
        self.sigs = [signature(np.asarray(Image.open(p).convert('RGB'), np.uint8))
                     for p in sorted(path.glob('*.jpg'))]
        # 原图裁掉边缘 SHIFT，实拍帧则补边（屏框可能把画面切掉一点）
        self.cores = [s[SHIFT:-SHIFT, SHIFT:-SHIFT] for s in self.sigs]
        self.n = self.cores[0].size

    def match(self, rgb, top=3):
        s = np.pad(signature(rgb), SHIFT, mode='edge')
        h, w = SIG[1] - 2 * SHIFT, SIG[0] - 2 * SHIFT
        out = []
        for name, c in zip(self.names, self.cores):
            best = -2.0
            for dy in range(2 * SHIFT + 1):
                for dx in range(2 * SHIFT + 1):
                    v = float((c * s[dy:dy + h, dx:dx + w]).sum()) / self.n
                    if v > best:
                        best = v
            out.append((name, best))
        out.sort(key=lambda t: -t[1])
        return out[:top]


def main():
    d = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / 'models/items103/cap_test')
    gal = Gallery()
    print(f'题库 {len(gal.names)} 张，核对 {d}')
    for p in sorted(d.glob('*.jpg')):
        arr = np.asarray(Image.open(p).convert('RGB'), np.uint8)
        t = gal.match(arr)
        stem = p.stem.split('__')[0]
        flag = 'ok ' if t[0][0] == stem else '!= '
        print(f'{flag}{p.name:22s} 文件说={stem:12s} 画面说={t[0][0]:12s}'
              f'{t[0][1]:+.3f} (次 {t[1][0]} {t[1][1]:+.3f})')


if __name__ == '__main__':
    main()
