#!/usr/bin/env python3
"""用实拍照片离线验证 items127 模型，复现端侧完整链路。

为什么需要这个：val acc 是用增强图算的，和实拍不是一回事。烧录一轮要十几分钟，
先用真实照片在 PC 上跑一遍，能直接预测端侧表现。

复现的端侧链路（与 components/items127/items127.cpp 严格一致）：
  任意照片 -> resize 到 320x240（摄像头 FRAMESIZE_QVGA）
           -> 非等比拉伸到 224x224（esp-dl ImagePreprocessor）
           -> /255 + ImageNet Normalize（端侧 mean/std = 255*ImageNet，等价）
           -> MobileNetV2 -> softmax -> yaml 顺序索引 -> 中文名

用法:
  python3.11 tools/items127/verify_capture.py 照片1.jpg [照片2.jpg ...]
  python3.11 tools/items127/verify_capture.py --topk 5 --ema 照片.jpg
"""
from pathlib import Path
import argparse

import torch
import yaml
from PIL import Image
from torchvision import transforms
from torchvision.models import mobilenet_v2

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'models' / 'train_items127'
SIZE = 224
CAM_W, CAM_H = 320, 240   # FRAMESIZE_QVGA，与 main.cpp 一致


def build_tf():
    # 两步 resize 精确复刻端侧：先降到摄像头分辨率，再非等比拉到模型输入
    return transforms.Compose([
        transforms.Resize((CAM_H, CAM_W)),    # -> 320x240，模拟摄像头采样
        transforms.Resize((SIZE, SIZE)),      # -> 224x224，模拟 esp-dl 非等比拉伸
        transforms.ToTensor(),
        transforms.Normalize([0.485, 0.456, 0.406], [0.229, 0.224, 0.225]),
    ])


def load_model(use_ema=False):
    spec = yaml.safe_load((ROOT / 'models' / 'items127.yaml').read_text())
    classes = spec['groups']['items127']

    model = mobilenet_v2(weights=None)
    model.classifier[-1] = torch.nn.Linear(model.classifier[-1].in_features, len(classes))

    ckpt = OUT / ('best_ema.pth' if use_ema else 'best.pth')
    sd = torch.load(ckpt, map_location='cpu')
    model.load_state_dict(sd)
    model.eval()
    return model, classes, ckpt


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('images', nargs='+', help='实拍照片路径')
    ap.add_argument('--topk', type=int, default=5)
    ap.add_argument('--ema', action='store_true', help='用 best_ema.pth 而非 best.pth')
    args = ap.parse_args()

    model, classes, ckpt = load_model(args.ema)
    tf = build_tf()
    print(f'model: {ckpt.name}  |  classes: {len(classes)}')
    print(f'链路: 原图 -> {CAM_W}x{CAM_H} -> {SIZE}x{SIZE}(非等比) -> normalize\n')

    for p in args.images:
        f = Path(p)
        if not f.exists():
            print(f'[跳过] 文件不存在: {p}')
            continue

        img = Image.open(f).convert('RGB')
        x = tf(img).unsqueeze(0)

        with torch.no_grad():
            logits = model(x)[0]
            probs = torch.softmax(logits, dim=0)

        topk = torch.topk(probs, min(args.topk, len(classes)))
        print(f'=== {f.name}  (原始 {img.size[0]}x{img.size[1]}) ===')
        for rank, (prob, idx) in enumerate(zip(topk.values.tolist(), topk.indices.tolist()), 1):
            mark = '  <-- top1' if rank == 1 else ''
            print(f'  {rank}. {prob*100:6.2f}%  [{idx:3d}] {classes[idx]}{mark}')
        print()


if __name__ == '__main__':
    main()
