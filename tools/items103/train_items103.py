#!/usr/bin/env python3
"""训练 103 类常见物品分类器（真实照片 + 屏幕模拟增强，端侧拍屏识别）。

数据: models/items103/raw_aug_240/<类>/*.jpg —— 103 类 x 20 增强 x 2 构图(fill/fit)
      由 tools/items103/augment_to_240.py 从 augmented/ 生成（480x320 -> 320x240）。
自学习: models/items103/real_captures/<类>/ 里的实拍帧自动混入 train（x REAL_REPEAT）。
val  = 增强图 20% 留出，零随机变换，衡量端侧几何链路拟合度。

用法: python tools/items103/train_items103.py          (本地 CPU)
      DCU: 见 items103_train.sbatch（dtk torch1.10 兼容已内建）
环境变量: T80_DEVICE T80_AMP T80_BATCH T80_LR T80_EPOCHS T80_AUG_PER_EPOCH
          T80_REAL_REPEAT T80_FINETUNE T80_THREADS T80_WEIGHT_DECAY
产出: models/train_items103/{best.pth,best_ema.pth,last.pth,classes103.json}
"""
import os
import random
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
RAW = ROOT / 'models' / 'items103' / 'raw_aug_240'
REAL = ROOT / 'models' / 'items103' / 'real_captures'
REAL_REPEAT = int(os.environ.get('T80_REAL_REPEAT', '3'))
DATA = ROOT / 'models' / 'dataset_items103'
OUT = ROOT / 'models' / 'train_items103'
OUT.mkdir(parents=True, exist_ok=True)

DEVICE = os.environ.get('T80_DEVICE', 'cpu')
if DEVICE == 'cpu':
    torch.set_num_threads(int(os.environ.get('T80_THREADS', '14')))
SIZE = 224
BATCH = int(os.environ.get('T80_BATCH', '32'))  # DCU 显存有限，默认 32
EPOCHS = int(os.environ.get('T80_EPOCHS', '60'))
LR = float(os.environ.get('T80_LR', '0.001'))
WORKERS = 0  # DCU 集群多进程 DataLoader 易卡死，用主进程加载
AMP = os.environ.get('T80_AMP', 'bf16' if DEVICE == 'cpu' else 'off')
AUG_PER_EPOCH = int(os.environ.get('T80_AUG_PER_EPOCH', '2000'))
WEIGHT_DECAY = float(os.environ.get('T80_WEIGHT_DECAY', '0.01'))


def amp():
    """AMP context manager — DCU dtk torch 1.10 无 bf16 autocast，回退 nullcontext。"""
    if AMP == 'off':
        return __import__('contextlib').nullcontext()
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
    return model.to(DEVICE)


def build():
    spec = yaml.safe_load((ROOT / 'models' / 'items103.yaml').read_text())
    classes = spec['groups']['items103']

    class_images = {}
    for cls in classes:
        d = RAW / cls
        imgs = sorted(list(d.glob('*.jpg')) + list(d.glob('*.png'))) if d.exists() else []
        assert imgs, f'缺增强图的类别: {cls}'
        class_images[cls] = imgs
    total_images = sum(len(v) for v in class_images.values())
    print(f'Total augmented images: {total_images}')

    # 每次都用当前 RAW 重建数据集，避免增强策略换了却仍训旧数据
    if DATA.exists():
        shutil.rmtree(DATA)
        print(f'cleared stale dataset: {DATA}')

    train_ratio = 0.8
    for cls in classes:
        imgs = class_images[cls][:]
        random.Random(42).shuffle(imgs)   # 固定种子：DCU/本地切分一致
        split_idx = int(len(imgs) * train_ratio)
        for split, split_imgs in [('train', imgs[:split_idx]), ('val', imgs[split_idx:])]:
            d = DATA / split / cls
            d.mkdir(parents=True, exist_ok=True)
            for img_path in split_imgs:
                shutil.copy2(img_path, d / f'{img_path.stem}{img_path.suffix}')

    # ---- 自学习闭环：用户在 Web 标注台确认/改判后的实拍帧 ----
    # 只进 train（val 保持纯增强图，继续作为几何链路是否对齐的指标）。
    real_count = 0
    if REAL.exists():
        for d in sorted(p for p in REAL.iterdir() if p.is_dir()):
            if d.name not in class_images:
                print(f'  !! 忽略未知实拍类别目录: {d.name}')
                continue
            fs = sorted(list(d.glob('*.png')) + list(d.glob('*.jpg')))
            for f in fs:
                for k in range(REAL_REPEAT):
                    shutil.copy2(f, DATA / 'train' / d.name / f'{f.stem}__real{k}{f.suffix}')
                    real_count += 1
    if real_count:
        print(f'real captures injected into train: {real_count} files '
              f'({real_count // REAL_REPEAT} unique x{REAL_REPEAT})')

    def _count(d):
        return sum(1 for p in d.rglob('*') if p.suffix.lower() in ('.jpg', '.jpeg', '.png'))

    train_count = _count(DATA / 'train')
    val_count = _count(DATA / 'val')
    print(f'Dataset built: {train_count} train + {val_count} val images -> {DATA}')
    assert train_count + val_count == total_images + real_count, (
        f'数据集不完整: {train_count}+{val_count} != RAW {total_images} + real {real_count}'
    )
    return classes


def main():
    classes = build()
    num_classes = len(classes)
    print(f'classes ({num_classes}):', classes[:5], '...', classes[-3:])

    # ImageFolder 按字母序编号；classes 表本身即字母序（classes.txt），恒等映射，
    # 但仍显式做 remap，防止有人往 yaml 里加非字母序类别时静默错位。
    alpha_classes = sorted(classes)
    perm = [alpha_classes.index(c) for c in classes]
    inv_perm = {v: i for i, v in enumerate(perm)}

    train_ds = datasets.ImageFolder(DATA / 'train', transform=None)
    val_ds = datasets.ImageFolder(DATA / 'val', transform=None)

    def target_transform(idx):
        return inv_perm[idx]

    # 几何必须与端侧 esp-dl 完全一致：整帧非等比拉伸到 224x224。
    # 数据已预先转成 320x240（含 fill/fit 两种屏幕构图），这里只需再拉到 224。
    train_tf = transforms.Compose([
        transforms.Resize((SIZE, SIZE)),          # 非等比，与端侧一致
        transforms.RandomHorizontalFlip(),
        transforms.ColorJitter(0.4, 0.4, 0.4, 0.12),
        transforms.RandomRotation(8),
        transforms.RandomGrayscale(0.03),
        transforms.GaussianBlur(5, sigma=(0.1, 2.0)),
        transforms.ToTensor(),
        transforms.Normalize([0.485, 0.456, 0.406], [0.229, 0.224, 0.225]),
        transforms.RandomErasing(0.25, scale=(0.02, 0.2)),
    ])
    val_tf = transforms.Compose([
        transforms.Resize((SIZE, SIZE)),
        transforms.ToTensor(),
        transforms.Normalize([0.485, 0.456, 0.406], [0.229, 0.224, 0.225]),
    ])

    train_ds.transform = train_tf
    train_ds.target_transform = target_transform
    val_ds.transform = val_tf
    val_ds.target_transform = target_transform

    weights = [1.0] * len(train_ds.samples)
    sampler = torch.utils.data.WeightedRandomSampler(weights, num_samples=AUG_PER_EPOCH, replacement=True)
    train_dl = DataLoader(train_ds, batch_size=BATCH, sampler=sampler, num_workers=WORKERS, pin_memory=False)
    val_dl = DataLoader(val_ds, batch_size=BATCH, shuffle=False, num_workers=WORKERS, pin_memory=False)

    model = build_model(num_classes)
    # 自学习增量微调：从上一轮部署的权重继续训，而不是 ImageNet 预训练从头训。
    FINETUNE = os.environ.get('T80_FINETUNE', '')
    if FINETUNE:
        ckpt = Path(FINETUNE)
        if not ckpt.is_absolute():
            ckpt = OUT / ckpt
        model.load_state_dict(torch.load(ckpt, map_location='cpu'))
        model = model.to(DEVICE)
        print(f'finetune init from: {ckpt}')
    ema = {k: v.clone().detach().float() for k, v in model.state_dict().items() if v.dtype.is_floating_point}
    ema_decay = 0.999

    optimizer = torch.optim.SGD([p for p in model.parameters() if p.dtype.is_floating_point],
                                lr=LR, momentum=0.9, weight_decay=WEIGHT_DECAY)
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
            with torch.no_grad():
                for k, v in model.state_dict().items():
                    if k in ema and v.dtype.is_floating_point:
                        ema[k].mul_(ema_decay).add_(v.float(), alpha=1 - ema_decay)
            loss_sum += loss.item() * y.size(0)
            n += y.size(0)
        scheduler.step()

        model.eval()
        ok = tot = 0
        # 分类别命中统计：定位自学习阶段哪些类仍不收敛
        per_cls_ok = [0] * num_classes
        per_cls_tot = [0] * num_classes
        with torch.no_grad():
            for x, y in val_dl:
                x, y = x.to(DEVICE), y.to(DEVICE)
                with amp():
                    pred = model(x).argmax(1)
                hit = (pred == y)
                ok += hit.sum().item()
                tot += y.size(0)
                for p, t, h in zip(pred.tolist(), y.tolist(), hit.tolist()):
                    per_cls_tot[t] += 1
                    per_cls_ok[t] += int(h)
        acc = ok / tot
        worst = sorted(range(num_classes),
                       key=lambda i: per_cls_ok[i] / max(1, per_cls_tot[i]))[:5]
        worst_s = ', '.join(f'{classes[i]}({per_cls_ok[i]}/{per_cls_tot[i]})' for i in worst)
        print(f'ep{ep:02d} loss {loss_sum/n:.3f} val {acc:.4f} {time.time()-t0:.0f}s  worst: {worst_s}', flush=True)
        torch.save(model.state_dict(), OUT / 'last.pth')
        if acc > best:
            best = acc
            torch.save(model.state_dict(), OUT / 'best.pth')
            torch.save({k: v.half().float() for k, v in ema.items()}, OUT / 'best_ema.pth')
            torch.save(classes, OUT / 'classes103.json')
    print(f'best val acc: {best:.4f}  total {time.time()-t_all:.0f}s')
    print(f'=== ALL DONE, best.pth in {OUT}/')


if __name__ == '__main__':
    main()
