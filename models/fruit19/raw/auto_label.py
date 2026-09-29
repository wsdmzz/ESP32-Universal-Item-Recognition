# -*- coding: utf-8 -*-
"""
水果图片自动标注脚本
使用 OpenCV GrabCut 前景分割 -> 检测框 -> 生成 X-AnyLabeling 兼容 JSON
"""
import os
import sys
import json
import glob
import time
import numpy as np
import cv2
from PIL import Image

FRUITS = [
    "水蜜桃", "哈密瓜", "火龙果", "猕猴桃",
    "葡萄", "菠萝", "草莓", "蓝莓", "樱桃", "龙眼",
    "荔枝", "西瓜", "柠檬", "橘子", "木瓜", "柚子",
    "李子", "橙子", "梨",
]

MIN_AREA_RATIO = 0.005
GRABCUT_ITERS = 5


def find_initial_rect(img):
    h, w = img.shape[:2]
    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    blur = cv2.GaussianBlur(gray, (5, 5), 0)
    edges = cv2.Canny(blur, 30, 100)
    kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (21, 21))
    edges = cv2.dilate(edges, kernel)
    contours, _ = cv2.findContours(edges, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    if contours:
        largest = max(contours, key=cv2.contourArea)
        x, y, bw, bh = cv2.boundingRect(largest)
        pad = 15
        x = max(0, x - pad)
        y = max(0, y - pad)
        bw = min(w - x, bw + 2 * pad)
        bh = min(h - y, bh + 2 * pad)
        return (x, y, bw, bh)
    return (int(w * 0.1), int(h * 0.1), int(w * 0.8), int(h * 0.8))


def grabcut_mask(img):
    h, w = img.shape[:2]
    rect = find_initial_rect(img)
    mask = np.zeros((h, w), np.uint8)
    bgd = np.zeros((1, 65), np.float64)
    fgd = np.zeros((1, 65), np.float64)
    cv2.grabCut(img, mask, rect, bgd, fgd, GRABCUT_ITERS, cv2.GC_INIT_WITH_RECT)
    fg = np.where((mask == cv2.GC_FGD) | (mask == cv2.GC_PR_FGD), 255, 0).astype(np.uint8)
    fg_ratio = np.count_nonzero(fg) / (h * w)
    if fg_ratio < 0.03 or fg_ratio > 0.97:
        gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
        blur = cv2.GaussianBlur(gray, (5, 5), 0)
        for flag in [cv2.THRESH_BINARY_INV, cv2.THRESH_BINARY]:
            _, binary = cv2.threshold(blur, 0, 255, flag + cv2.THRESH_OTSU)
            r = np.count_nonzero(binary) / (h * w)
            if 0.05 < r < 0.85:
                fg = binary
                break
    kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (5, 5))
    fg = cv2.morphologyEx(fg, cv2.MORPH_OPEN, kernel)
    fg = cv2.morphologyEx(fg, cv2.MORPH_CLOSE, kernel)
    return fg


def mask_to_boxes(mask, img_area):
    num, labels, stats, _ = cv2.connectedComponentsWithStats(mask, connectivity=8)
    boxes = []
    min_area = img_area * MIN_AREA_RATIO
    for i in range(1, num):
        x, y, w, h, area = stats[i]
        if area < min_area:
            continue
        boxes.append([int(x), int(y), int(x + w), int(y + h)])
    boxes.sort(key=lambda b: (b[1], b[0]))
    return boxes


def extract_fruit(filename):
    for fruit in FRUITS:
        if fruit in filename:
            return fruit
    return "unknown"


def cv2_imread(path):
    return cv2.imdecode(np.fromfile(path, dtype=np.uint8), cv2.IMREAD_COLOR)


def cv2_imwrite(path, img):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    cv2.imencode(".png", img)[1].tofile(path)


def save_mask_preview(img_path, mask):
    base = os.path.splitext(os.path.basename(img_path))[0]
    preview_dir = os.path.join(os.path.dirname(img_path), "mask_preview")
    cv2_imwrite(os.path.join(preview_dir, base + "_mask.png"), mask)


def generate_json(img_path, boxes, label, img_size):
    w, h = img_size
    shapes = []
    for box in boxes:
        shapes.append({
            "label": label,
            "score": None,
            "points": [[box[0], box[1]], [box[2], box[3]]],
            "group_id": None,
            "description": "",
            "difficult": False,
            "shape_type": "rectangle",
            "flags": {},
            "attributes": {},
            "kie_linking": {},
        })
    data = {
        "version": "x-anylabeling",
        "flags": {},
        "shapes": shapes,
        "imagePath": os.path.basename(img_path),
        "imageData": None,
        "imageHeight": h,
        "imageWidth": w,
    }
    json_path = os.path.splitext(img_path)[0] + ".json"
    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
    return json_path


def main():
    work_dir = os.path.dirname(os.path.abspath(__file__))
    os.chdir(work_dir)

    images = sorted(
        f for f in os.listdir(work_dir)
        if f.lower().endswith(".png")
    )
    print(f"[数据] 找到 {len(images)} 张图片\n")

    results = []
    total_start = time.time()
    for idx, fname in enumerate(images, 1):
        img_path = os.path.join(work_dir, fname)
        fruit = extract_fruit(fname)
        img = cv2_imread(img_path)
        if img is None:
            print(f"[{idx:2d}/{len(images)}] 读取失败: {fname}")
            continue
        h, w = img.shape[:2]
        img_area = w * h

        t0 = time.time()
        mask = grabcut_mask(img)
        boxes = mask_to_boxes(mask, img_area)
        infer_ms = (time.time() - t0) * 1000

        save_mask_preview(img_path, mask)
        json_path = generate_json(img_path, boxes, fruit, (w, h))

        results.append((fname, fruit, len(boxes), infer_ms))
        print(f"[{idx:2d}/{len(images)}] {fruit:6s} | {len(boxes)} 框 | {infer_ms:6.0f}ms | {fname[:50]}")

    elapsed = time.time() - total_start
    total_boxes = sum(r[2] for r in results)
    fruits_found = sorted(set(r[1] for r in results))

    print(f"\n{'='*60}")
    print(f"完成: {len(results)} 张图片, {total_boxes} 个检测框, 耗时 {elapsed:.1f}s")
    print(f"水果类别 ({len(fruits_found)}): {', '.join(fruits_found)}")
    print(f"标注 JSON 已生成 (与图片同名 .json), mask 预览在 mask_preview/")
    print(f"可用 X-AnyLabeling 打开本目录查看/编辑标注")


if __name__ == "__main__":
    main()