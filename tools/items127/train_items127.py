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
# 320x240 与端侧摄像头帧同尺寸，训练/量化/端侧走同一条几何链路（见 resize_to_320.py）
RAW = ROOT / 'models' / 'items127' / 'raw_aug_320'
# 自学习闭环：Web 端确认/改判后的实拍帧，按类别子目录存放（label_server.py 写入）
REAL = ROOT / 'models' / 'items127' / 'real_captures'
REAL_REPEAT = int(os.environ.get('T80_REAL_REPEAT', '3'))
DATA = ROOT / 'models' / 'dataset_items127'
OUT = ROOT / 'models' / 'train_items127'
OUT.mkdir(parents=True, exist_ok=True)

DEVICE = os.environ.get('T80_DEVICE', 'cpu')
if DEVICE == 'cpu':
    torch.set_num_threads(int(os.environ.get('T80_THREADS', '14')))
SIZE = 224
BATCH = int(os.environ.get('T80_BATCH', '32'))  # DCU 显存有限，默认 32
EPOCHS = int(os.environ.get('T80_EPOCHS', '60'))
LR = float(os.environ.get('T80_LR', '0.001'))  # 降低学习率避免过拟合（原 0.01）
WORKERS = 0  # DCU 集群多进程 DataLoader 易卡死，用主进程加载
AMP = os.environ.get('T80_AMP', 'bf16' if DEVICE == 'cpu' else 'off')
AUG_PER_EPOCH = int(os.environ.get('T80_AUG_PER_EPOCH', '2000'))
WEIGHT_DECAY = float(os.environ.get('T80_WEIGHT_DECAY', '0.01'))  # 增加正则化


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

    # 收集所有增强图片，按品种分组（raw_aug_320 是 jpg，兼容旧的 png）
    class_images = {cls: [] for cls in classes}
    files = sorted(list(RAW.glob('*.jpg')) + list(RAW.glob('*.png')))
    for f in files:
        # 文件名格式：品种名_vX_suffix.png 或 品种名_orig.png
        # 提取品种名（文件名中第一个下划线之前的部分）
        stem = f.stem
        # 尝试匹配：从 YAML 的 gen_zh_map 中查找
        matched_cls = None
        for cls_name in classes:
            if stem.startswith(cls_name):
                matched_cls = cls_name
                break
        
        if matched_cls:
            class_images[matched_cls].append(f)
        else:
            # 尝试从文件名解析（兼容旧格式）
            m = re.search(r'，(.+?)，', f.name)
            if m:
                cls = gen_zh.get(m.group(1))
                if cls and cls in class_images:
                    class_images[cls].append(f)

    # 缺类直接失败，不要静默训出一个类别数不对的模型
    missing = [c for c in classes if not class_images[c]]
    assert not missing, f'缺增强图的类别({len(missing)}): {missing[:5]}'
    
    total_images = sum(len(imgs) for imgs in class_images.values())
    print(f'Total augmented images: {total_images}')

    # 每次都用当前 RAW 重建数据集，避免增强策略换了却仍训旧数据
    # （历史坑：原先 `if not (DATA/'train').exists()` 会短路，导致屏幕模拟增强
    #   的 1397 张图没进数据集，训练白跑一轮）
    if DATA.exists():
        shutil.rmtree(DATA)
        print(f'cleared stale dataset: {DATA}')

    train_ratio = 0.8  # 80% 训练，20% 验证
    for cls in classes:
        imgs = class_images[cls]
        if not imgs:
            continue

        # 打乱顺序
        random.shuffle(imgs)
        split_idx = int(len(imgs) * train_ratio)
        train_imgs = imgs[:split_idx]
        val_imgs = imgs[split_idx:]

        # 复制到 train 目录
        for split, split_imgs in [('train', train_imgs), ('val', val_imgs)]:
            d = DATA / split / cls
            d.mkdir(parents=True, exist_ok=True)
            for img_path in split_imgs:
                # 保留原扩展名（raw_aug_320 是 jpg），避免扩展名与实际编码不符
                shutil.copy2(img_path, d / f'{img_path.stem}{img_path.suffix}')

    # ---- 自学习闭环：用户在 Web 端确认/改判过的实拍帧 ----
    # 只进 train（val 保持纯生成图，继续作为几何链路是否对齐的指标）。
    # 每张重复 REAL_REPEAT 份，让少量实拍样本在 WeightedRandomSampler 下有足够话语权。
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

    # 断言：数据集规模必须与 RAW+实拍 一致，否则说明标注/复制漏了
    assert train_count + val_count == total_images + real_count, (
        f'数据集不完整: {train_count}+{val_count} != RAW {total_images} + real {real_count}'
    )

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

    # 几何必须与端侧 esp-dl 完全一致：整帧非等比拉伸到 224x224。
    # 原先用 RandomResizedCrop / Resize(256)+CenterCrop(224) 是等比语义，
    # 而端侧把 320x240 直接拉成 224x224（物体横向压扁 25%），两者形变不同，
    # 导致圆形物体（橙子/樱桃/脸盆）在端侧认不出。数据已预先拉成 320x240，
    # 这里只需再拉到 224，即与端侧同一条链路。
    train_tf = transforms.Compose([
        transforms.Resize((SIZE, SIZE)),          # 非等比，与端侧一致
        transforms.RandomHorizontalFlip(),        # 以下均为像素级增强，不改几何比例
        transforms.ColorJitter(0.4, 0.4, 0.4, 0.12),
        transforms.RandomRotation(8),             # 小角度，模拟手持轻微倾斜
        transforms.RandomGrayscale(0.03),
        transforms.GaussianBlur(5, sigma=(0.1, 2.0)),
        transforms.ToTensor(),
        transforms.Normalize([0.485, 0.456, 0.406], [0.229, 0.224, 0.225]),
        transforms.RandomErasing(0.25, scale=(0.02, 0.2)),
    ])
    # val 严格复刻端侧推理链路：零随机，只有非等比 resize + 归一化
    val_tf = transforms.Compose([
        transforms.Resize((SIZE, SIZE)),
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
    train_dl = DataLoader(train_ds, batch_size=BATCH, sampler=sampler, num_workers=WORKERS, pin_memory=False)
    val_dl = DataLoader(val_ds, batch_size=BATCH, shuffle=False, num_workers=WORKERS, pin_memory=False)

    model = build_model(num_classes)
    # 自学习增量微调：从上一轮部署的权重继续训，而不是 ImageNet 预训练从头训。
    # 用法: T80_FINETUNE=last.pth T80_LR=0.0002 T80_EPOCHS=8 python3.11 tools/items127/train_items127.py
    FINETUNE = os.environ.get('T80_FINETUNE', '')
    if FINETUNE:
        ckpt = Path(FINETUNE)
        if not ckpt.is_absolute():
            ckpt = OUT / ckpt
        model.load_state_dict(torch.load(ckpt, map_location='cpu'))
        model = model.to(DEVICE)
        print(f'finetune init from: {ckpt}')
    # EMA: only track float parameters (skip buffers like num_batches_tracked in BatchNorm)
    ema = {k: v.clone().detach().float() for k, v in model.state_dict().items() if v.dtype.is_floating_point}
    ema_decay = 0.999

    optimizer = torch.optim.SGD([p for p in model.parameters() if p.dtype.is_floating_point], lr=LR, momentum=0.9, weight_decay=WEIGHT_DECAY)
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
