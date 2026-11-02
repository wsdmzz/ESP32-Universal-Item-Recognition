#!/usr/bin/env python3
"""camcheck.py — 一帧看三件事：屏幕在画面里的位置、有没有削顶、糊不糊。

自学习闭环要的是「摄像头能看清出题图」，这三件事分别对应：
  位置/占比  -> 屏只占画面一小块时，模型拿到的是 120x90 放大的糊图，再准的分类器也没用
  削顶比例  -> 屏是自发光亮点，AEC 按整帧平均测光，桌面够亮时屏已经全白
  清晰度    -> Laplacian 方差，长曝光抖动和失焦都会把它打下去（失焦只能靠挪距离）

用法: tools/camcheck.py [--box x0,y0,x1,y1] [--save f.png]
"""
import argparse
import io
import json
import time
import urllib.request

import numpy as np
from PIL import Image

DEF = "http://192.168.8.194"


def http(url):
    with urllib.request.urlopen(url, timeout=20) as r:
        return r.read()


def seq(base):
    try:
        return json.loads(http(f"{base}/result.json")).get("seq", 0)
    except Exception:
        return 0


def grab(base, wait_fresh=True):
    s0 = seq(base)
    if wait_fresh:
        t0 = time.time()
        while time.time() - t0 < 12:
            if seq(base) != s0:
                break
            time.sleep(0.4)
    return np.asarray(Image.open(io.BytesIO(http(f"{base}/capture.jpg"))).convert("L"),
                      dtype=np.float64)


def lap_var(a):
    if a.shape[0] < 3 or a.shape[1] < 3:
        return 0.0
    lap = (a[:-2, 1:-1] + a[2:, 1:-1] + a[1:-1, :-2] + a[1:-1, 2:] - 4 * a[1:-1, 1:-1])
    return float(lap.var())


def find_box(a, thr=170):
    """用最亮的一大块连通区域近似屏幕包围盒（阈值分割 + 逐块投票）"""
    h, w = a.shape
    cols, rows = 16, 12
    mask = np.zeros((rows, cols), bool)
    for r in range(rows):
        for c in range(cols):
            blk = a[r * h // rows:(r + 1) * h // rows, c * w // cols:(c + 1) * w // cols]
            mask[r, c] = blk.mean() > thr
    if not mask.any():
        return None
    rs = np.where(mask.any(axis=1))[0]
    cs = np.where(mask.any(axis=0))[0]
    return (int(cs.min() * w / cols), int(rs.min() * h / rows),
            int((cs.max() + 1) * w / cols), int((rs.max() + 1) * h / rows))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=DEF)
    ap.add_argument("--box", default=None, help="不给就自动找亮块")
    ap.add_argument("--thr", type=float, default=170, help="自动找屏的块均值阈值")
    ap.add_argument("--save", default=None)
    a = ap.parse_args()

    base = a.host.rstrip("/")
    g = grab(base)
    h, w = g.shape

    if a.box:
        box = tuple(int(t) for t in a.box.split(","))
        auto = find_box(g, a.thr)
    else:
        box = auto = find_box(g, a.thr)
        if not box:
            box = (0, 0, w, h)

    sub = g[box[1]:box[3], box[0]:box[2]]
    clip = 100.0 * (sub >= 250).mean()
    frac = 100.0 * (box[2] - box[0]) * (box[3] - box[1]) / (w * h)

    # 固定三行输出，方便脚本里 sed -n 取；找不到亮块也要占一行，否则行号会漂
    print(f"整帧 {w}x{h}  均值={g.mean():.1f}  清晰度(全图 Laplacian var)={lap_var(g):.1f}")
    print(f"自动找屏  bbox={auto if auto else '未找到（整帧都不够亮，先查曝光/屏是否黑着）'}")
    print(f"屏幕区 bbox={box} 占画面 {frac:.1f}%  "
          f"均值={sub.mean():.1f} 中位={np.median(sub):.0f} 削顶={clip:.1f}%  "
          f"清晰度={lap_var(sub):.1f}")
    print(f"换算：屏 {box[2]-box[0]}x{box[3]-box[1]} px 对应 480x320 出题图 "
          f"-> 模型实际只看到 {(box[2]-box[0])}x{(box[3]-box[1])} 的缩略图")

    if a.save:
        im = Image.open(io.BytesIO(http(f"{base}/capture.jpg"))).convert("RGB")
        d = np.array(im)   # 要画框，必须拿可写副本
        x0, y0, x1, y1 = box
        d[y0:y0 + 2, x0:x1] = (255, 0, 0)
        d[y1 - 2:y1, x0:x1] = (255, 0, 0)
        d[y0:y1, x0:x0 + 2] = (255, 0, 0)
        d[y0:y1, x1 - 2:x1] = (255, 0, 0)
        Image.fromarray(d).resize((w * 2, h * 2), Image.LANCZOS).save(a.save)
        print(f"已存标注图 {a.save}")


if __name__ == "__main__":
    main()
