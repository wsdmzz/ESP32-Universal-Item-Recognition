#!/usr/bin/env python3
"""训练 80 类 MobileNetV2 分类器（torchvision -> 后续 esp-ppq 量化）。

用法: /home/oooa/miniforge3/envs/espdl/bin/python tools/dataset80/train80.py
产出: models/train80/best.pth + models/train80/model.pth (state_dict)
"""
import json
import os
import random
import time
from pathlib import Path

import torch
import torch.nn as nn
import yaml
from torch.utils.data import DataLoader
from torchvision import datasets, transforms
from torchvision.models import mobilenet_v2, MobileNet_V2_Weights

ROOT = Path(__file__).resolve().parents[2]
DATA = ROOT / 'models' / 'dataset80'
OUT = ROOT / 'models' / 'train80'
OUT.mkdir(parents=True, exist_ok=True)

# GPU 被 llama-server 常驻占用 -> 默认 CPU 训练（20 核），设 T80_DEVICE=cuda 可切回
DEVICE = os.environ.get('T80_DEVICE', 'cpu')
if DEVICE == 'cpu':
    torch.set_num_threads(int(os.environ.get('T80_THREADS', '14')))

IMAGES = DATA / 'images'
SIZE = 224
BATCH = 128 if DEVICE == 'cpu' else 16
EPOCHS = 30
LR = 0.02 if DEVICE == 'cpu' else 0.01   # batch128 线性缩放，小数据集保守取 0.02
WORKERS = 5 if DEVICE == 'cpu' else 10


def build():
    spec = yaml.safe_load((ROOT / 'models' / 'classes80.yaml').read_text())
    classes = [c for g in spec['groups'].values() for c in g]
    random.seed(42)
    for split, frac in (('train', 0.9), ('val', 0.1)):
        d = DATA / split
        if d.exists():
            continue
        (d).mkdir(parents=True)
        for cls in classes:
            files = sorted((IMAGES / cls).glob('*'))
            files = [f for f in files if f.suffix.lower() in ('.jpg', '.jpeg', '.png')]
            random.shuffle(files)
            n = len(files)
            if n == 0:
                raise RuntimeError(f'class {cls} has 0 images; fill dataset first')
            nval = min(120, max(10, n // 5))   # 每类 10-120 张做验证集
            cut = n - nval
            pick = files[:cut][:500] if split == 'train' else files[cut:][:nval]
            (d / cls).mkdir(exist_ok=True)
            for f in pick:
                link = d / cls / f.name
                if not link.exists():
                    link.symlink_to(f.resolve())
    return classes


def main():
    classes = build()
    (OUT / 'classes80.json').write_text(json.dumps(classes, ensure_ascii=False, indent=1))
    print('classes:', len(classes))

    train_tf = transforms.Compose([
        transforms.RandomResizedCrop(SIZE, scale=(0.5, 1.0)),
        transforms.RandomHorizontalFlip(),
        transforms.ColorJitter(0.3, 0.3, 0.3, 0.1),
        transforms.RandomRotation(15),
        transforms.RandomGrayscale(p=0.05),
        transforms.ToTensor(),
        transforms.Normalize([0.485, 0.456, 0.406], [0.229, 0.224, 0.225]),
        transforms.RandomErasing(p=0.2),
    ]
    )
    val_tf = transforms.Compose([
        transforms.Resize(256), transforms.CenterCrop(SIZE),
        transforms.ToTensor(),
        transforms.Normalize([0.485, 0.456, 0.406], [0.229, 0.224, 0.225]),
    ])
    def safe_loader(path):
        # Commons/公开数据集个别文件可能损坏，回退为中性灰图防训练崩溃
        from PIL import Image
        try:
            with Image.open(path) as im:
                return im.convert('RGB')
        except Exception:
            return Image.new('RGB', (SIZE, SIZE), (128, 128, 128))

    train_ds = datasets.ImageFolder(DATA / 'train', train_tf, loader=safe_loader)
    val_ds = datasets.ImageFolder(DATA / 'val', val_tf, loader=safe_loader)
    print('train imgs:', len(train_ds), 'val imgs:', len(val_ds))

    # ImageFolder 的类别按字母序编号，而设备端/导出统一用 classes80.yaml 分组序。
    # 通过 target_transform 把"字母序索引"重映射到"yaml 分组序索引"，保证整链路一致
    # （__getitem__ 应用 target_transform，比直接改 .targets 可靠）。
    alpha = train_ds.classes            # 排序后的目录名（字母序）
    missing = [c for c in alpha if c not in classes]
    assert not missing, f'dirs not in yaml: {missing}'
    assert val_ds.classes == alpha, 'train/val dir order mismatch'
    perm = [classes.index(c) for c in alpha]   # 字母序 idx -> yaml 分组 idx
    train_ds.target_transform = lambda t: perm[t]
    val_ds.target_transform = lambda t: perm[t]
    # 纯映射校验，不加载图像（防损坏缩略图使训练启动即崩）
    assert len(perm) == len(classes) and sorted(perm) == list(range(len(classes)))
    assert all(perm[t] < len(classes) for _, t in train_ds.samples)

    model = mobilenet_v2(weights=MobileNet_V2_Weights.IMAGENET1K_V1)
    model.classifier[-1] = nn.Linear(model.classifier[-1].in_features, len(classes))
    model = model.to(DEVICE)

    ema = {k: v.clone().float() for k, v in model.state_dict().items()}

    def update_ema(m):
        with torch.no_grad():
            for k, v in m.state_dict().items():
                if v.dtype.is_floating_point:
                    ema[k].mul_(0.999).add_(v.float(), alpha=0.001)
                else:
                    ema[k] = v.clone().float()

    opt = torch.optim.SGD(model.parameters(), LR, momentum=0.9, weight_decay=1e-4)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, EPOCHS)
    pin = DEVICE == 'cuda'
    train_dl = DataLoader(train_ds, BATCH, shuffle=True, num_workers=WORKERS,
                          pin_memory=pin, drop_last=True)
    val_dl = DataLoader(val_ds, 256, False, num_workers=4, pin_memory=pin)

    best = 0.0
    for ep in range(EPOCHS):
        model.train()
        t0 = time.time()
        loss_sum = n = 0
        for x, y in train_dl:
            x, y = x.to(DEVICE), y.to(DEVICE)
            opt.zero_grad(set_to_none=True)
            with torch.autocast(DEVICE, dtype=torch.bfloat16):
                out = model(x)
                loss = nn.functional.cross_entropy(out, y, label_smoothing=0.1)
            loss.backward()
            opt.step()
            update_ema(model)
            loss_sum += loss.item() * x.size(0)
            n += x.size(0)
        sched.step()

        model.eval()
        ok = tot = 0
        with torch.no_grad():
            for x, y in val_dl:
                x, y = x.to(DEVICE), y.to(DEVICE)
                with torch.autocast(DEVICE, dtype=torch.bfloat16):
                    ok += (model(x).argmax(1) == y).sum().item()
                tot += y.size(0)
        acc = ok / tot
        print(f'ep{ep:02d} loss {loss_sum/n:.3f} val {acc:.4f} {time.time()-t0:.0f}s', flush=True)
        torch.save(model.state_dict(), OUT / 'last.pth')
        if acc > best:
            best = acc
            torch.save(model.state_dict(), OUT / 'best.pth')
            # EMA 权重单独保存（转 onnx 时用）
            torch.save({k: v.half().float() for k, v in ema.items()}, OUT / 'best_ema.pth')
    print('best val acc:', best)


if __name__ == '__main__':
    main()
