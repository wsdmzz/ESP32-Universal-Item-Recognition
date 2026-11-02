#!/usr/bin/env python3
"""eval_preproc.py — 屏幕反光能不能靠软件预处理救回来？拿真模型离线 A/B。

输入是 cap_collect.py 采下来的「端侧真正吃进去的那一帧」（models/items103/cap_test/
<类>__<序号>.jpg），所以这里跑的和板子上跑的是同一张图，只是把端侧的量化推理
换成 CPU 上的 fp32 mobilenet_v2。

流程：
  1) 先用三个 checkpoint 各跑一遍原始帧，和板子上记录下来的预测对答案，
      agreement 最高的那个就是当前 espdl 的源头（不然比了也是白比）。
  2) 在这个 checkpoint 上逐个试预处理变体，报 top-1 命中率。
  3) 再拿合成训练集 val 抽查一遍 —— 校正若把干净图也拧坏了，自学习会学歪。

用法:
  python3 tools/items103/eval_preproc.py
  python3 tools/items103/eval_preproc.py --device-labels lamp:台灯,skateboard:滑板
"""
import argparse
import json
import re
import sys
from pathlib import Path

import numpy as np
import torch
import torch.nn.functional as F
from PIL import Image, ImageFilter
from torchvision.models import mobilenet_v2

ROOT = Path(__file__).resolve().parents[2]
CAPS = ROOT / 'models' / 'items103' / 'cap_test'
VAL = ROOT / 'models' / 'dataset_items103' / 'val'
CKPTS = ['last.pth', 'best.pth', 'best_ema.pth']
SIZE = 224
MEAN = np.array([0.485, 0.456, 0.406], np.float32)
STD = np.array([0.229, 0.224, 0.225], np.float32)


def load_classes():
    """classes103.json 其实是 torch.save 的 list，不是 JSON。"""
    p = ROOT / 'models' / 'train_items103' / 'classes103.json'
    try:
        d = torch.load(p, map_location='cpu', weights_only=False)
        return list(d)
    except Exception:
        return json.loads(p.read_text())


def build(ckpt, n_cls):
    m = mobilenet_v2(weights=None)
    m.classifier[-1] = torch.nn.Linear(m.classifier[-1].in_features, n_cls)
    m.load_state_dict(torch.load(ROOT / 'models' / 'train_items103' / ckpt,
                                 map_location='cpu'))
    return m.eval()


def to_tensor(rgb_u8):
    """HxWx3 uint8 -> (非等比拉伸 224) -> ToTensor -> Normalize，与端侧协议一致。"""
    im = Image.fromarray(rgb_u8).resize((SIZE, SIZE), Image.BILINEAR)
    x = np.asarray(im, np.float32) / 255.0
    x = (x - MEAN) / STD
    return torch.from_numpy(x.transpose(2, 0, 1)).unsqueeze(0)


@torch.no_grad()
def logits(model, rgb_u8):
    return model(to_tensor(rgb_u8))[0].numpy()


# ---------------- 预处理变体：都吃 HxWx3 uint8，吐 uint8 ----------------

def pct_levels(a, lo=1.0, hi=99.0, per_channel=True):
    """量「黑底」和「白顶」：反光把黑抬起来，这两个数就是加性底 + 乘性雾。"""
    if per_channel:
        l = np.percentile(a, lo, axis=(0, 1))
        h = np.percentile(a, hi, axis=(0, 1))
    else:
        g = a.reshape(-1, 3).mean(1)
        l = np.full(3, np.percentile(g, lo), np.float32)
        h = np.full(3, np.percentile(g, hi), np.float32)
    return l.astype(np.float32), h.astype(np.float32)


def v_raw(a):
    return a


def v_stretch(a):
    """逐通道百分位拉伸：底 - 黑电平，雾 - 除以斜率。反光的主项就这两下。"""
    l, h = pct_levels(a, 1, 99)
    f = (a.astype(np.float32) - l) / np.maximum(h - l, 8.0)
    return np.clip(f * 255.0, 0, 255).astype(np.uint8)


def v_stretch_g(a):
    """共享一套比例的拉伸（保住色相，不让蓝底被单独拉成青）。"""
    l, h = pct_levels(a, 1, 99, per_channel=False)
    f = (a.astype(np.float32) - l) / np.maximum(h - l, 8.0)
    return np.clip(f * 255.0, 0, 255).astype(np.uint8)


def _wb(a, ref):
    g = a.reshape(-1, 3).mean(0)
    gain = ref / np.maximum(g, 1.0)
    gain /= gain.mean()                       # 只改色偏，不改整体亮度
    return np.clip(a.astype(np.float32) * gain, 0, 255).astype(np.uint8)


def v_gw(a):
    """灰世界白平衡：三通道均值拉平。实测白区 B/R=1.14，就是它该管的。"""
    return _wb(a, a.reshape(-1, 3).mean())


def v_wp(a):
    """白补丁：拿 99.5 百分位当白，逐通道归一。"""
    hi = np.percentile(a, 99.5, axis=(0, 1))
    return _wb(a, hi.mean())


def _clahe(a, clip=3.0, grid=8):
    import cv2
    lab = cv2.cvtColor(a, cv2.COLOR_RGB2LAB)
    eq = cv2.createCLAHE(clipLimit=clip, tileGridSize=(grid, grid))
    lab[..., 0] = eq.apply(lab[..., 0])
    return cv2.cvtColor(lab, cv2.COLOR_LAB2RGB)


def v_clahe(a):
    """局部直方图均衡：反光底是斜的（边缘更亮），全局减法去不干净。"""
    return _clahe(a)


def v_unsharp(a):
    """反锐化：光学把 12 屏像素的线抹平了，锐化只能救回一部分。"""
    im = Image.fromarray(a)
    bl = im.filter(ImageFilter.GaussianBlur(1.2))
    d = np.asarray(im, np.float32) - np.asarray(bl, np.float32)
    return np.clip(np.asarray(im, np.float32) + 1.2 * d, 0, 255).astype(np.uint8)


def v_stretch_wp(a):
    return v_stretch(v_wp(a))


def v_wp_stretch(a):
    return v_stretch_g(v_wp(a))


def v_clahe_wp(a):
    return _clahe(v_wp(a))


def v_cunstretched(a):
    return v_unsharp(v_stretch(a))


VARIANTS = [
    ('raw',           v_raw),
    ('stretch',       v_stretch),
    ('stretch_g',     v_stretch_g),
    ('gw',            v_gw),
    ('wp',            v_wp),
    ('clahe',         v_clahe),
    ('unsharp',       v_unsharp),
    ('wp+stretch',    v_stretch_wp),
    ('wp+stretch_g',  v_wp_stretch),
    ('wp+clahe',      v_clahe_wp),
    ('stretch+sharp', v_cunstretched),
]


def load_cn2en():
    """从端侧类表头文件里取 中文->英文 的对应，用来把板子的判决和类索引对上。"""
    h = (ROOT / 'components' / 'items103' / 'include' / 'items103_classes.hpp')
    txt = h.read_text(encoding='utf-8')

    def arr(name):
        body = txt.split(f'{name}[103] = {{', 1)[1].split('};', 1)[0]
        return [s.strip().strip('"') for s in body.split(',') if s.strip()]

    cn, en = arr('ITEMS103_CN'), arr('ITEMS103_EN')
    return dict(zip(cn, en))


def load_device_labels(log_path):
    """cap_collect 每采一张会打印「<类> -> ... 模型=<中文> N%」，那是端侧 INT8 的判决。"""
    p = Path(log_path)
    if not p.exists():
        return {}
    cn2en = load_cn2en()
    out = {}
    for ln in p.read_text(encoding='utf-8').splitlines():
        m = re.match(r'(\w+)\s+->.*模型=(\S+) (\d+)%', ln)
        if m and m.group(2) in cn2en:
            out[m.group(1)] = cn2en[m.group(2)]
    return out


def read_caps():
    out = []
    for p in sorted(CAPS.glob('*.jpg')):
        stem = p.stem.split('__')[0]
        im = Image.open(p).convert('RGB')
        out.append((stem, p, np.asarray(im, np.uint8)))
    return out


def run(model, frames, variant_fn, classes):
    """返回 (命中标签的张数, top1 置信度均值, 每张的 top1 名)。"""
    hit = 0
    conf = []
    preds = []
    for stem, path, arr in frames:
        lg = logits(model, variant_fn(arr))
        t = torch.softmax(torch.from_numpy(lg), dim=0).numpy()
        i = int(t.argmax())
        conf.append(t[i])
        preds.append((stem, path, classes[i], t[i] * 100))
        if stem in classes[i].lower() or classes[i] == stem:
            hit += 1
    return hit, float(np.mean(conf)) if conf else 0.0, preds


def pick_ckpt(frames, classes, want=None):
    """三个 ckpt 各跑原始帧，谁和板子/谁自己更贴合这批实拍就用谁。"""
    best = None
    for ck in CKPTS:
        model = build(ck, len(classes))
        hit, conf, preds = run(model, frames, v_raw, classes)
        agree = None
        if want:
            agree = sum(1 for s, _, n, _ in preds if want.get(s) == n)
        print(f'  {ck:14s} top1={hit}/{len(frames)}  置信={conf*100:.1f}%'
              + (f'  与端侧一致={agree}/{len(frames)}' if want is not None else ''))
        score = (agree if agree is not None else hit, conf)
        if best is None or score > best[0]:
            best = (score, ck, model)
    return best[1], best[2]


def val_check(model, classes, n=120):
    """干净合成图上校正是否无害（抽查 n 张）。"""
    dirs = sorted(d for d in VAL.iterdir() if d.is_dir())
    imgs = []
    for d in dirs:
        for p in sorted(d.glob('*'))[:2]:
            if d.name in classes:
                imgs.append((d.name, p, np.asarray(Image.open(p).convert('RGB'), np.uint8)))
        if len(imgs) >= n:
            break
    imgs = imgs[:n]
    if not imgs:
        return None
    r = []
    for name, fn in VARIANTS[:9]:
        hit, _, _ = run(model, imgs, fn, classes)
        r.append((name, hit, len(imgs)))
    return r


def main():
    global CAPS
    ap = argparse.ArgumentParser()
    ap.add_argument('--caps', default=str(CAPS))
    ap.add_argument('--detail', action='store_true', help='打印每张的 top1')
    ap.add_argument('--no-val', action='store_true')
    ap.add_argument('--log', default='/tmp/capcol.log',
                    help='cap_collect 的输出，用来拿端侧 INT8 判决挑 checkpoint')
    a = ap.parse_args()

    CAPS = Path(a.caps)
    classes = load_classes()
    frames = read_caps()
    if not frames:
        print(f'no captures under {CAPS} — 先跑 cap_collect.py')
        return 1
    print(f'实拍帧 {len(frames)} 张，类表 {len(classes)} 项')

    want = load_device_labels(a.log)
    if want:
        print('端侧 INT8 判决 ' + str(len(want)) + ' 条（用它挑 checkpoint）')
    ck, model = pick_ckpt(frames, classes, want)
    print(f'\n采用 checkpoint: {ck}\n')

    print(f'{"变体":15s} {"命中":>8s} {"均置信":>8s}')
    base = None
    for name, fn in VARIANTS:
        hit, conf, preds = run(model, frames, fn, classes)
        if base is None:
            base = hit
        mark = '' if name == 'raw' else f'  {hit-base:+d}'
        print(f'{name:15s} {hit:3d}/{len(frames)}  {conf*100:6.1f}%{mark}')
        if a.detail:
            for s, _, n, p in preds:
                flag = 'ok ' if (n == s or s in n) else '   '
                print(f'    {flag}{s:12s} -> {n} {p:.0f}%')

    if not a.no_val:
        r = val_check(model, classes)
        if r:
            print(f'\n干净合成图抽查（{r[0][2]} 张，校正不该伤这里）:')
            for name, hit, tot in r:
                print(f'  {name:15s} {hit:3d}/{tot}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
