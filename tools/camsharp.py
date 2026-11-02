#!/usr/bin/env python3
"""camsharp.py — 在固定屏幕区上比「曝光/增益」组合的清晰度，找抖动模糊的下限。

点测光把屏压到不削顶之后，剩下的敌人是糊：曝光行数越长，手持/面包板上的微抖
就越会被抹成一片。缩短曝光要拿增益换，所以这里两维一起扫，量三个数：
  均值/削顶  -> 曝光对不对
  Laplacian 方差 -> 糊不糊（同一张图横向比较才有意义，所以脚本会先把出题图钉死）

用法:
  tools/camsharp.py                      # 扫内置 (aecv, agcg) 组合
  tools/camsharp.py --n slippers         # 先钉住这张出题图再扫
  tools/camsharp.py --box 57,98,179,180
  tools/camsharp.py --keep               # 留在最后一组，不恢复点测光
"""
import argparse
import json
import time
import urllib.parse
import urllib.request
from io import BytesIO

import numpy as np
from PIL import Image

DEF = "http://192.168.8.194"

# (手动曝光行数, 手动增益 0..1023)：行数往下、增益往上，看清晰度能不能换回来
DEFAULT_GRID = [(a, g) for a in (293, 160, 90, 50, 30) for g in (0, 30, 60, 120)]


def http(url):
    with urllib.request.urlopen(url, timeout=25) as r:
        return r.read()


def cam(base, **kw):
    return json.loads(http(f"{base}/cam?" + urllib.parse.urlencode(kw)))


def seq(base):
    try:
        return json.loads(http(f"{base}/result.json")).get("seq", 0)
    except Exception:
        return 0


def grab(base):
    return np.asarray(Image.open(BytesIO(http(f"{base}/capture.jpg"))).convert("L"),
                      dtype=np.float64)


def grab_fresh(base, timeout=15):
    s0 = seq(base)
    t0 = time.time()
    while time.time() - t0 < timeout:
        if seq(base) != s0:
            return grab(base)
        time.sleep(0.4)
    return grab(base)


def lap_var(a):
    if a.shape[0] < 3 or a.shape[1] < 3:
        return 0.0
    lap = a[:-2, 1:-1] + a[2:, 1:-1] + a[1:-1, :-2] + a[1:-1, 2:] - 4 * a[1:-1, 1:-1]
    return float(lap.var())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=DEF)
    ap.add_argument("--box", default="57,98,179,180")
    ap.add_argument("--n", default=None, help="先显示指定出题图，保证各档比的是同一张")
    ap.add_argument("--every", type=int, default=3600, help="扫之前把换图间隔拉长")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--grid", default=None, help='JSON: [[aecv,agcg],...]')
    a = ap.parse_args()

    base = a.host.rstrip("/")
    x0, y0, x1, y1 = (int(t) for t in a.box.split(","))
    grid = json.loads(a.grid) if a.grid else DEFAULT_GRID

    http(f"{base}/quiz?every={a.every}")
    if a.n:
        http(f"{base}/quiz?n={a.n}")
        time.sleep(2.5)
    cam(base, spot=0, crop=0)          # 手动档：别让控制器和裁剪抢测量区域
    time.sleep(1.0)

    print(f"屏幕区 {x0},{y0},{x1},{y1} = {x1-x0}x{y1-y0} px")
    print(f"{'aecv':>5s} {'agcg':>5s} {'均值':>7s} {'p90':>5s} {'削顶%':>6s} {'清晰度':>8s}  回读")
    for aecv, agcg in grid:
        st = cam(base, aecv=aecv, agc=0, agcg=agcg)
        time.sleep(1.0)
        g = grab_fresh(base)
        sub = g[y0:y1, x0:x1]
        if sub.size == 0:
            print(f"{aecv:5d} {agcg:5d}   框超出画面")
            continue
        mean = sub.mean()
        clip = 100.0 * (sub >= 250).mean()
        p90 = float(np.percentile(sub, 90))
        lv = lap_var(sub)
        rb = f"aecv={st.get('aecv')} agcg={st.get('agcg')} agc={st.get('agc')} aec={st.get('aec')}"
        print(f"{aecv:5d} {agcg:5d} {mean:7.1f} {p90:5.0f} {clip:6.1f} {lv:8.1f}  {rb}")

    if a.keep:
        print("保留最后一组（--keep）")
    else:
        cam(base, spot=1, crop=1)
        print("已恢复点测光 + 裁剪")


if __name__ == "__main__":
    main()
