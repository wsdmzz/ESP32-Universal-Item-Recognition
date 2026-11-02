#!/usr/bin/env python3
"""camstat.py — 量「摄像头看屏幕」的曝光，判断削顶/反光有多严重。

用法:
  tools/camstat.py                 # 抓一帧，打印整体+中心区统计
  tools/camstat.py --grid          # 再打印 8x6 分块亮度表，用来定位屏幕在画面里的位置
  tools/camstat.py --out f.jpg     # 顺便把这帧存下来

为什么需要它：屏幕是自发光亮点，OV3660 的 AEC 按整帧平均测光，桌面看着还行时
屏已经全白削顶（>=250），出题图的细节就没了。削顶比例是唯一能客观比较调参好坏的数。
"""
import argparse
import io
import sys
import urllib.request

try:
    from PIL import Image
except ImportError:
    sys.exit("需要 Pillow：pip install pillow")

DEF = "http://192.168.8.194"


def grab(base):
    url = base.rstrip("/") + "/capture.jpg"
    with urllib.request.urlopen(url, timeout=15) as r:
        data = r.read()
    return Image.open(io.BytesIO(data)).convert("L"), data


def stat(g, name):
    px = list(g.getdata())
    n = len(px)
    mean = sum(px) / n
    clip = sum(1 for v in px if v >= 250) / n
    dead = sum(1 for v in px if v <= 8) / n
    lo, hi = min(px), max(px)
    # 中位数比均值抗高光，更能代表「主体到底亮成什么样」
    srt = sorted(px)
    med = srt[n // 2]
    print(f"{name:10s} {g.width}x{g.height} 均值={mean:6.1f} 中位={med:3d} "
          f"min/max={lo}/{hi} 削顶={clip*100:5.1f}% 死黑={dead*100:5.1f}%")
    return clip


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=DEF)
    ap.add_argument("--grid", action="store_true")
    ap.add_argument("--out", default=None)
    a = ap.parse_args()

    g, raw = grab(a.host)
    if a.out:
        with open(a.out, "wb") as f:
            f.write(raw)
        print(f"已存 {a.out} ({len(raw)} 字节)")

    stat(g, "整帧")
    w, h = g.size
    box = (int(w * 0.25), int(h * 0.25), int(w * 0.75), int(h * 0.75))
    stat(g.crop(box), "中心 1/2")

    if a.grid:
        cols, rows = 8, 6
        print("分块均值（亮块就是屏）：")
        sym = []
        for r in range(rows):
            line, sl = [], []
            for c in range(cols):
                b = list(g.crop((w * c // cols, h * r // rows,
                                 w * (c + 1) // cols, h * (r + 1) // rows)).getdata())
                line.append(f"{sum(b)/len(b):5.0f}")
                clip = sum(1 for v in b if v >= 250) / len(b)
                sl.append('#' if clip > 0.2 else ('+' if clip > 0.03 else '.'))
            print("  " + " ".join(line))
            sym.append("  " + " ".join(sl))
        print("分块削顶图：")
        for s in sym:
            print(s)
        print("  图例：# = 该块 >20% 像素削顶，+ = 3~20%，. = 基本干净")


if __name__ == "__main__":
    main()
