#!/usr/bin/env python3
"""训练 127 类常见物品分类器（闭集即梦生成图，端侧拍屏识别）。

数据: models/items127/raw/*.png —— 127 张生成图（每类 1 张），唯一识别目标
采样: WeightedRandomSampler 均匀过采样 AUG_PER_EPOCH 次。
val = 生成图原图（确定性变换，目标指标 100% 拟合）。

用法: /home/oooa/miniforge3/envs/espdl/bin/python tools/items127/train_items127.py
环境变量: T80_DEVICE(cpu/cuda) T80_AMP(bf16/fp16/off) T80_BATCH T80_LR T80_EPOCHS T80_AUG_PER_EPOCH
产出: models/train_items127/{best.pth,best_ema.pth,classes127.json}
"""
import json
import os
import random
import re
import shutil
import time
from pathlib import Path

import torch
import torch.nn as nn
import yaml
from torch.utils.data import DataLoader
from torchvision import datasets, transforms
from torchvision.models import mobilenet_v2
try:   # torchvision>=0.13 新 weights API；DCU 集群是 0.10 老版，只有 pretrained=
    from torchvision.models import MobileNet_V2_Weights
    HAS_WEIGHTS = True
except ImportError:
    HAS_WEIGHTS = False

ROOT = Path(__file__).resolve().parents[2]
RAW = ROOT / 'models' / 'items127' / 'raw'
DATA = ROOT / 'models' / 'dataset_items127'
OUT = ROOT / 'models' / 'train_items127'
OUT.mkdir(parents=True, exist_ok=True)

DEVICE = os.environ.get('T80_DEVICE', 'cpu')
if DEVICE == 'cpu':
    torch.set_num_threads(int(os.environ.get('T80_THREADS', '14')))
SIZE = 224
BATCH = int(os.environ.get('T80_BATCH', '32'))  # DCU 显存有限，默认 32
EPOCHS = int(os.environ.get('T80_EPOCHS', '60'))
LR = float(os.environ.get('T80_LR', '0.01'))
WORKERS = 4 if DEVICE == 'cpu' else 6  # DCU 建议 ≤8 workers
AMP = os.environ.get('T80_AMP', 'bf16' if DEVICE == 'cpu' else 'off')
AUG_PER_EPOCH = int(os.environ.get('T80_AUG_PER_EPOCH', '2000'))


def amp():
    """AMP context manager — DCU dtk torch 1.10 无 bf16 autocast，回退 nullcontext。"""
    if AMP == 'off':
        return torch.no_grad() if False else __import__('contextlib').nullcontext()
    try:
        if AMP == 'bf16':
            return torch.amp.autocast('cuda', dtype=torch.bfloat16)
        return torch.amp.autocast('cuda', dtype=torch.float16)
    except Exception:
        return __import__('contextlib').nullcontext()


def build_model(num_classes):
    if HAS_WEIGHTS:
        model = mobilenet_v2(weights=MobileNet_V2_Weights.DEFAULT)
    else:
        model = mobilenet_v2(pretrained=True)
    model.classifier[-1] = nn.Linear(model.classifier[-1].in_features, num_classes)
    model = model.to(DEVICE)
    return model


def build():
    spec = yaml.safe_load((ROOT / 'models' / 'items127.yaml').read_text())
    classes = spec['groups']['items127']
    gen_zh = spec['gen_zh_map']
    random.seed(42)

    gens = {}
    for f in sorted(RAW.glob('*.png')):
        m = re.search(r'，(.+?)，', f.name)
        assert m, f'无法从文件名解析品种: {f.name}'
        cls = gen_zh[m.group(1)]
        gens[cls] = f
    missing = [c for c in classes if c not in gens]
    assert not missing, f'缺生成图: {missing}'

    if not (DATA / 'train').exists():
        for cls in classes:
            for split in ('train', 'val'):
                d = DATA / split / cls
                d.mkdir(parents=True, exist_ok=True)
                shutil.copy2(gens[cls], d / f'gen__{cls}.png')
        print(f'dataset built: {len(classes)} classes -> {DATA}')
    else:
        print(f'dataset already exists at {DATA}')

    return classes


def main():
    classes = build()
    num_classes = len(classes)
    print(f'classes ({num_classes}):', classes[:5], '...', classes[-3:])

    # Build class index map (alphabetical → yaml order)
    alpha_classes = sorted(classes)
    perm = [alpha_classes.index(c) for c in classes]  # yaml_order[i] -> alpha_index
    inv_perm = {v: i for i, v in enumerate(perm)}      # alpha_index -> yaml_order

    train_ds = datasets.ImageFolder(DATA / 'train', transform=None)
    val_ds = datasets.ImageFolder(DATA / 'val', transform=None)

    # Target transform: ImageFolder gives alphabetical index → remap to yaml order
    def target_transform(idx):
        return inv_perm[idx]

    # Strong train aug (same as fruit19)
    train_tf = transforms.Compose([
        transforms.RandomResizedCrop(SIZE, scale=(0.35, 1.0), ratio=(0.6, 1.7)),
        transforms.RandomHorizontalFlip(),
        transforms.ColorJitter(0.4, 0.4, 0.4, 0.12),
        transforms.RandomRotation(20),
        transforms.RandomGrayscale(0.03),
        transforms.GaussianBlur(5, sigma=(0.1, 2.5)),
        transforms.ToTensor(),
        transforms.Normalize([0.485, 0.456, 0.406], [0.229, 0.224, 0.225]),
        transforms.RandomErasing(0.25, scale=(0.02, 0.2)),
    ])
    val_tf = transforms.Compose([
        transforms.Resize(256),
        transforms.CenterCrop(SIZE),
        transforms.ToTensor(),
        transforms.Normalize([0.485, 0.456, 0.406], [0.229, 0.224, 0.225]),
    ])

    train_ds.transform = train_tf
    train_ds.target_transform = target_transform
    val_ds.transform = val_tf
    val_ds.target_transform = target_transform

    # Uniform sampler with oversampling
    weights = [1.0] * len(train_ds.samples)
    sampler = torch.utils.data.WeightedRandomSampler(weights, num_samples=AUG_PER_EPOCH, replacement=True)
    train_dl = DataLoader(train_ds, batch_size=BATCH, sampler=sampler, num_workers=WORKERS, pin_memory=True)
    val_dl = DataLoader(val_ds, batch_size=BATCH, shuffle=False, num_workers=WORKERS, pin_memory=True)

    model = build_model(num_classes)
    # EMA: only track float parameters (skip buffers like num_batches_tracked in BatchNorm)
    ema = {k: v.clone().detach().float() for k, v in model.state_dict().items() if v.dtype.is_floating_point}
    ema_decay = 0.999

    optimizer = torch.optim.SGD([p for p in model.parameters() if p.dtype.is_floating_point], lr=LR, momentum=0.9, weight_decay=1e-4)
    scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=EPOCHS)
    criterion = nn.CrossEntropyLoss()

    best = 0.0
    t_all = time.time()

    for ep in range(1, EPOCHS + 1):
        t0 = time.time()
        model.train()
        loss_sum = 0.0
        n = 0
        for x, y in train_dl:
            x, y = x.to(DEVICE), y.to(DEVICE)
            optimizer.zero_grad()
            with amp():
                pred = model(x)
                loss = criterion(pred, y)
            loss.backward()
            optimizer.step()
            # EMA update (only float params)
            with torch.no_grad():
                for k, v in model.state_dict().items():
                    if k in ema and v.dtype.is_floating_point:
                        ema[k].mul_(ema_decay).add_(v.float(), alpha=1 - ema_decay)
            loss_sum += loss.item() * y.size(0)
            n += y.size(0)
        scheduler.step()

        # Val
        model.eval()
        ok = tot = 0
        with torch.no_grad():
            for x, y in val_dl:
                x, y = x.to(DEVICE), y.to(DEVICE)
                with amp():
                    pred = model(x).argmax(1)
                hit = (pred == y)
                ok += hit.sum().item()
                tot += y.size(0)
        acc = ok / tot
        print(f'ep{ep:02d} loss {loss_sum/n:.3f} val {acc:.4f} {time.time()-t0:.0f}s', flush=True)
        torch.save(model.state_dict(), OUT / 'last.pth')
        if acc > best:
            best = acc
            torch.save(model.state_dict(), OUT / 'best.pth')
            torch.save({k: v.half().float() for k, v in ema.items()}, OUT / 'best_ema.pth')
            torch.save(classes, OUT / 'classes127.json')
    print(f'best val acc: {best:.4f}  total {time.time()-t_all:.0f}s')
    print(f'=== ALL DONE, best.pth in {OUT}/')


if __name__ == '__main__':
    main()
