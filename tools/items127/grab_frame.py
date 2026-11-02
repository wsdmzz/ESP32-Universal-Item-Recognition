#!/usr/bin/env python3.11
# -*- coding: utf-8 -*-
"""
从端侧 HTTP 抓帧服务拉取「模型真正看到的那一帧」。

端侧 /frame.rgb 返回的就是 classify(rgb_buf, fw, fh) 的输入缓冲原始字节
（RGB888, 320x240, 无任何再处理），所以这里存下来的 PNG 与设备推理输入
逐像素一致 —— 离线验证不再有任何猜测成分。

用法:
    python3.11 tools/items127/grab_frame.py --ip 192.168.1.23
    python3.11 tools/items127/grab_frame.py --ip 192.168.1.23 --count 10 --interval 3
    python3.11 tools/items127/grab_frame.py --ip 192.168.1.23 --dedup   # 只在 seq 变化时存

抓完直接用 verify_capture.py 离线跑:
    python3.11 tools/items127/verify_capture.py models/items127/captures/*.png --topk 5
"""
import argparse
import json
import re
import sys
import time
import urllib.request
import urllib.parse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CAM_W, CAM_H = 320, 240        # FRAMESIZE_QVGA，与 main.cpp 一致

_SAFE = re.compile(r'[^\w\u4e00-\u9fff-]+')


def slug(name: str, maxlen: int = 24) -> str:
    name = (name or 'unknown').strip()
    name = _SAFE.sub('_', name).strip('_')
    return name[:maxlen] or 'unknown'


def fetch(url: str, timeout: float = 25.0):
    req = urllib.request.Request(url, headers={'User-Agent': 'grab_frame/1.0'})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read(), dict(r.headers)


def get_result(base: str):
    try:
        raw, _ = fetch(f'{base}/result.json', timeout=8)
        return json.loads(raw.decode('utf-8', 'replace'))
    except Exception as e:
        return {'error': str(e)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ip', required=True, help='端侧 IP，见 TFT 屏幕或串口日志')
    ap.add_argument('--port', type=int, default=80)
    ap.add_argument('--count', type=int, default=1, help='抓帧次数')
    ap.add_argument('--interval', type=float, default=2.0, help='两次抓帧间隔秒')
    ap.add_argument('--out', default=str(ROOT / 'models/items127/captures'))
    ap.add_argument('--dedup', action='store_true',
                    help='按 X-Frame-Seq 去重，帧号没变就不重复存')
    ap.add_argument('--jpg', action='store_true', help='同时存一张 /capture.jpg 便于肉眼查看')
    ap.add_argument('--label', default='', help='手工标注真值（写进 manifest，不参与识别）')
    args = ap.parse_args()

    try:
        from PIL import Image
    except ImportError:
        sys.exit('需要 Pillow: pip install pillow')

    base = f'http://{args.ip}:{args.port}'
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    manifest_path = out / 'manifest.jsonl'

    print(f'端侧: {base}')
    res = get_result(base)
    print(f'result.json: {json.dumps(res, ensure_ascii=False)}')
    if 'error' in res:
        sys.exit('连不上端侧 HTTP 服务，确认 WiFi 已连接、IP 正确、防火墙放行')
    if not res.get('ok', False):
        print('提示: 端侧还没跑完第一帧推理，稍后重试')

    seen = set()
    saved = 0
    for i in range(args.count):
        try:
            raw, hdr = fetch(f'{base}/frame.rgb')
        except Exception as e:
            print(f'[{i+1}/{args.count}] 抓帧失败: {e}')
            time.sleep(args.interval)
            continue

        seq = int(hdr.get('X-Frame-Seq', '-1') or -1)
        w = int(hdr.get('X-Frame-Width', '0') or 0)
        h = int(hdr.get('X-Frame-Height', '0') or 0)
        pct = hdr.get('X-Frame-Pct', '')
        label = urllib.parse.unquote(hdr.get('X-Frame-Label', '') or '')

        if w != CAM_W or h != CAM_H:
            print(f'  !! 尺寸异常 {w}x{h}（期望 {CAM_W}x{CAM_H}），端侧帧格式可能改了')
        if len(raw) != w * h * 3:
            print(f'  !! 字节数不符: {len(raw)} != {w}*{h}*3={w*h*3}，跳过')
            time.sleep(args.interval)
            continue

        if args.dedup and seq in seen:
            print(f'[{i+1}/{args.count}] seq={seq} 未变化，跳过')
            time.sleep(args.interval)
            continue
        seen.add(seq)

        stamp = time.strftime('%Y%m%d_%H%M%S')
        name = f'cap_{stamp}_s{seq}_{slug(label)}.png'
        path = out / name
        Image.frombytes('RGB', (w, h), raw).save(path)
        saved += 1
        print(f'[{i+1}/{args.count}] seq={seq} {w}x{h} 端侧判定="{label}" {pct}% -> {path.name}')

        rec = {
            'file': name, 'ts': stamp, 'seq': seq, 'w': w, 'h': h,
            'device_label': label, 'device_pct': pct,
            'truth': args.label or None,
        }
        with manifest_path.open('a', encoding='utf-8') as f:
            f.write(json.dumps(rec, ensure_ascii=False) + '\n')

        if args.jpg:
            try:
                jpg, _ = fetch(f'{base}/capture.jpg')
                (out / (path.stem + '.jpg')).write_bytes(jpg)
            except Exception as e:
                print(f'  capture.jpg 失败: {e}')

        if i + 1 < args.count:
            time.sleep(args.interval)

    print(f'\n完成，共保存 {saved} 帧到 {out}')
    if saved:
        print(f'离线验证: python3.11 tools/items127/verify_capture.py '
              f'{" ".join(str(p) for p in sorted(out.glob("cap_*.png"))[-saved:])} --topk 5')


if __name__ == '__main__':
    main()
