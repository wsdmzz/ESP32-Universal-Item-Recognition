#!/usr/bin/env python3
"""cap_collect.py — 从板子上采「端侧真正喂给模型的那一帧」，按真类名存下来。

自学习闭环要的是实拍域样本。这里的关键是标签可信，所以踩过的坑都堵上：

  1) 出题和推理是两条时间线：/quiz?n=X 只是把 X 送上屏，而那一轮推理可能早就
     起飞了、吃的是上一张。按「我出了什么题」命名就会整批错位一格
     （14 张里只有 1 张对得上，把帧和原图并排看才确认）。现在改成让板子自己
     交代出处：/result.json 的 photo 字段 = 那一帧过推理时屏上显示的那张图，
     等它等于目标名才收帧。
  2) 也别想用像素相关性代替出处：反光把画面抬了一层很亮的底，两张完全不同的
     出题图原始相关能有 0.92，判不出「屏换没换」。
  3) 两个采集进程同时跑会互相改屏（本次就是这么坏的）。加一把文件锁，
     同一时刻只允许一个采集者。

存的是 /capture.jpg，也就是模型实际吃进去的那一张，之后离线比
「加不加预处理」用的就是同一帧。

用法:
  tools/items103/cap_collect.py --photos skateboard,slippers,pig
  tools/items103/cap_collect.py --n 16 --out models/items103/cap_test
"""
import argparse
import fcntl
import io
import json
import os
import random
import time
import urllib.request
from pathlib import Path

import numpy as np
from PIL import Image

DEF = "http://192.168.8.194"
LOCK = Path.home() / '.cap_collect.lock'


def http(url, timeout=25):
    return urllib.request.urlopen(urllib.request.Request(url), timeout=timeout).read()


def grab(base):
    jpg = http(f"{base}/capture.jpg")
    return jpg, np.asarray(Image.open(io.BytesIO(jpg)).convert('RGB'), np.uint8)


def frame_of(base, name, tmo=120):
    """等「最近一次推理吃的那一帧，屏上就是 name」。

    板子的 /result.json 会回 `photo` —— 那一帧过推理时屏上显示的出题图。
    有它就不必拿像素猜了：出题和推理是两条时间线，靠「我出了题所以帧里就是它」
    会整体错一格（早期 14 张只对上 1 张就是这么坏的）；而反光又把两张完全不同的
    图抹到原始像素相关 0.92，用图像相关性判「屏换没换」也靠不住。
    """
    t0 = time.time()
    while time.time() - t0 < tmo:
        r = json.loads(http(f"{base}/result.json"))
        if Path(str(r.get("photo", ""))).stem == name and r.get("ok"):
            return r
        time.sleep(1.0)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=DEF)
    ap.add_argument("--photos", default=None, help="逗号分隔的出题名（不带 .jpg）")
    ap.add_argument("--n", type=int, default=12, help="没给 --photos 时随机抽几张")
    ap.add_argument("--out", default="models/items103/cap_test")
    ap.add_argument("--reps", type=int, default=1, help="每张连采几帧（看抖动）")
    ap.add_argument("--tries", type=int, default=3, help="等不到就重试几次")
    a = ap.parse_args()

    lk = open(LOCK, 'w')
    try:
        fcntl.flock(lk, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        print('另一个 cap_collect 正在采集（锁被占），先停掉它再跑。')
        return 2
    lk.write(str(os.getpid())); lk.flush()

    base = a.host.rstrip("/")
    root = Path(a.out)
    root.mkdir(parents=True, exist_ok=True)

    names = ([p.strip() for p in a.photos.split(",") if p.strip()] if a.photos
             else random.sample(json.loads(http(f"{base}/quiz?list=1"))["photos"], a.n))
    names = [Path(n).stem for n in names]

    http(f"{base}/quiz?every=3600")            # 采图期间别让换图任务抢屏
    # 板子一重启这些开关全回默认值（今天就被坑过一次：采到的是 320x240 全帧）。
    # 采集前先把「点测光 + 只喂屏幕区」钉回去。
    http(f"{base}/cam?spot=1&crop=1&adetect=1&target=200")
    # 找屏要几帧才投票锁定；等它真把画面裁小了再开工，不然采到的是全帧。
    t0 = time.time()
    while time.time() - t0 < 150:
        r = json.loads(http(f"{base}/result.json"))
        if r.get("width", 320) < 300:
            break
        time.sleep(2.0)
    r = json.loads(http(f"{base}/result.json"))
    print(f"取景 {r['width']}x{r['height']}（屏框锁定后应为 ~128x84）")
    ok = fail = 0
    for name in names:
        jpg_a = arr_a = res = None
        for t in range(a.tries):
            try:
                http(f"{base}/quiz?n={name}")
            except Exception as e:
                print(f"{name}: 出题失败 {e}")
                break
            res = frame_of(base, name)
            if res is None:
                print(f"{name}: 等不到「屏上就是这张」的那一帧，重试")
                continue
            jpg_a, arr_a = grab(base)
            back = json.loads(http(f"{base}/result.json"))
            if Path(str(back.get("photo", ""))).stem != name:
                print(f"{name}: 取帧的这几秒屏被换走了，重试")
                jpg_a = arr_a = None
                continue
            break
        if jpg_a is None:
            fail += 1
            continue
        for k in range(a.reps):
            out = root / f"{name}__{k}.jpg"
            if k == 0:
                out.write_bytes(jpg_a)
            else:                       # 同一张图再采一帧，看曝光抖动
                if frame_of(base, name):
                    out.write_bytes(grab(base)[0])
        print(f"{name:14s} -> {arr_a.shape[1]}x{arr_a.shape[0]} #{res['seq']} "
              f"模型={res['label']} {res['percent']}%")
        ok += 1
    http(f"{base}/quiz?every=120")             # 采完把 2 分钟刷新还回去
    print(f"共 {ok} 张（失败 {fail}），存到 {root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
