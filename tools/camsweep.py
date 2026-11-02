#!/usr/bin/env python3
"""camsweep.py — 扫 OV3660 曝光/增益组合，找「屏幕不削顶」的那一档。

屏幕是自发光亮点，AEC 按整帧平均测光 → 桌面够亮时屏已经全白，出题图细节没了。
这里按 /cam 端点逐组下发参数，等 AEC 稳定后连采几帧，只量屏幕那块区域的
均值和削顶比例，用数字挑参数，不靠眼睛猜。

用法:
  tools/camsweep.py                      # 跑内置的候选表
  tools/camsweep.py --box 80,80,200,200  # 指定屏幕在画面里的包围盒
  tools/camsweep.py --list my.json       # 用自己的候选表 [{"desc":..,"cam":{...}}, ...]
  tools/camsweep.py --keep               # 结束后把最后一组参数留在传感器上
"""
import argparse
import json
import time
import urllib.parse
import urllib.request
from io import BytesIO

from PIL import Image

DEF = "http://192.168.8.194"

# 内置候选：先试自动测光压暗，再试手动曝光/手动增益
DEFAULT_CANDS = [
    {"desc": "基线(全自动)",              "cam": {}},
    {"desc": "ae_level=-1",              "cam": {"ae": -1}},
    {"desc": "ae_level=-2",              "cam": {"ae": -2}},
    {"desc": "ae=-2 + bri=-2",           "cam": {"ae": -2, "bri": -2}},
    {"desc": "手动 agcg=0, aecv=700",    "cam": {"agc": 0, "agcg": 0, "aec": 0, "aecv": 700}},
    {"desc": "手动 aecv=500",            "cam": {"agc": 0, "agcg": 0, "aec": 0, "aecv": 500}},
    {"desc": "手动 aecv=350",            "cam": {"agc": 0, "agcg": 0, "aec": 0, "aecv": 350}},
    {"desc": "手动 aecv=250",            "cam": {"agc": 0, "agcg": 0, "aec": 0, "aecv": 250}},
    {"desc": "手动 aecv=150",            "cam": {"agc": 0, "agcg": 0, "aec": 0, "aecv": 150}},
    {"desc": "手动 aecv=80",             "cam": {"agc": 0, "agcg": 0, "aec": 0, "aecv": 80}},
    {"desc": "手动 aecv=250 + con=+2",   "cam": {"agc": 0, "agcg": 0, "aec": 0, "aecv": 250, "con": 2}},
]


def http(url):
    with urllib.request.urlopen(url, timeout=20) as r:
        return r.read()


def apply_cam(base, params):
    if not params:
        return {}
    q = urllib.parse.urlencode(params)
    return json.loads(http(f"{base}/cam?{q}"))


def grab(base):
    return Image.open(BytesIO(http(f"{base}/capture.jpg"))).convert("L")


def seq(base):
    """capture.jpg 发的是 detect_task 缓存的那一帧，约 4~5 秒才换一张；
    不盯 seq 的话连采三次其实是同一帧，白等。"""
    try:
        return json.loads(http(f"{base}/result.json")).get("seq", 0)
    except Exception:
        return 0


def grab_fresh(base, timeout=12):
    s0 = seq(base)
    t0 = time.time()
    while time.time() - t0 < timeout:
        s1 = seq(base)
        if s1 != s0:
            return grab(base)
        time.sleep(0.4)
    return grab(base)          # 超时也要给个结果，别整轮扫不下去


def measure(base, box, shots=2):
    """返回 (均值, 削顶%, 中位) —— 只量包围盒，取多次采样的最好一次"""
    best = None
    for i in range(shots):
        g = grab_fresh(base)
        w, h = g.size
        b = list(g.crop((box[0] * w // 320, box[1] * h // 240,
                         box[2] * w // 320, box[3] * h // 240)).getdata())
        mean = sum(b) / len(b)
        clip = 100.0 * sum(1 for v in b if v >= 250) / len(b)
        srt = sorted(b)
        med = srt[len(srt) // 2]
        cur = (mean, clip, med)
        if best is None or clip < best[1]:
            best = cur
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=DEF)
    ap.add_argument("--box", default="80,80,200,200", help="屏幕包围盒 x0,y0,x1,y1（按 320x240）")
    ap.add_argument("--list", default=None)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--shots", type=int, default=2, help="每组采几帧新帧（取削顶最低的一次）")
    a = ap.parse_args()

    base = a.host.rstrip("/")
    box = tuple(int(t) for t in a.box.split(","))
    cands = json.load(open(a.list)) if a.list else DEFAULT_CANDS

    print(f"{'候选':26s} {'均值':>6s} {'削顶%':>7s} {'中位':>5s}  传感器回读")
    for c in cands:
        st = apply_cam(base, c.get("cam", {}))
        time.sleep(1.2)                      # 等 AEC/AGC 收敛
        mean, clip, med = measure(base, box, shots=a.shots)
        flag = "  <== 干净" if clip < 1.0 and 100 < mean < 200 else ""
        rb = (f"aec={st.get('aec')} aecv={st.get('aecv')} agc={st.get('agc')} "
              f"agcg={st.get('agcg')} bri={st.get('bri')} con={st.get('con')}") if st else "-"
        print(f"{c['desc']:26s} {mean:6.1f} {clip:7.1f} {med:5d}  {rb}{flag}")

    if not a.keep:
        apply_cam(base, {"aec": 1, "agc": 1, "ae": 0, "bri": 1, "con": 0})
        print("已恢复自动曝光/自动增益（要保留最后一组用 --keep）")


if __name__ == "__main__":
    main()
