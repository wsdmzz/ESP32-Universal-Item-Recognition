#!/usr/bin/env python3
"""训练 19 类固定水果分类器（替换 items80，端侧拍屏识别即梦生成图）。

数据: models/fruit19/raw/*.png —— 19 张生成图（每类 1 张），主要识别目标（高权重）
      + dataset80 真实水果照片补充泛化（低权重，映射见 fruit19.yaml real_map）
采样: WeightedRandomSampler，生成图 GEN_W=30 : 真实图 1，每轮过采样 AUG_PER_EPOCH 次。
val = 生成图原图（确定性变换，gen acc 为目标指标）+ 真实图 held-out（real acc 参考）。

用法: /home/oooa/miniforge3/envs/espdl/bin/python tools/fruit19/train_fruit19.py
环境变量: T80_DEVICE(cpu/cuda) T80_AMP(bf16/fp16/off) T80_BATCH T80_LR T80_EPOCHS
          T80_REAL_DIR(真实图根目录, 默认 models/dataset80/images) T80_AUG_PER_EPOCH
产出: models/train_fruit19/{best.pth,best_ema.pth,classes19.json}
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
RAW = ROOT / 'models' / 'fruit19' / 'raw'
REAL = Path(os.environ.get('T80_REAL_DIR', str(ROOT / 'models' / 'dataset80' / 'images')))
DATA = ROOT / 'models' / 'dataset_fruit19'
OUT = ROOT / 'models' / 'train_fruit19'
OUT.mkdir(parents=True, exist_ok=True)

DEVICE = os.environ.get('T80_DEVICE', 'cpu')
if DEVICE == 'cpu':
    torch.set_num_threads(int(os.environ.get('T80_THREADS', '14')))
SIZE = 224
BATCH = int(os.environ.get('T80_BATCH', '64'))
EPOCHS = int(os.environ.get('T80_EPOCHS', '40'))
LR = float(os.environ.get('T80_LR', '0.01'))
WORKERS = 5 if DEVICE == 'cpu' else 10
AMP = os.environ.get('T80_AMP', 'bf16' if DEVICE == 'cpu' else 'off')
GEN_W = float(os.environ.get('T80_GEN_W', '30'))   # 生成图采样权重（主要目标域）
REAL_TRAIN = 40   # 每类真实图训练上限
REAL_VAL = 8      # 每类真实图验证上限


def build():
    spec = yaml.safe_load((ROOT / 'models' / 'fruit19.yaml').read_text())
    classes = spec['groups']['fruit']
    gen_zh = spec['gen_zh_map']
    real_map = spec.get('real_map', {})
    random.seed(42)

    gens = {}
    for f in sorted(RAW.glob('*.png')):
        m = re.search(r'新鲜(.+?)，', f.name)
        assert m, f'无法从文件名解析品种: {f.name}'
        cls = gen_zh[m.group(1)]
        gens[cls] = f
    missing = [c for c in classes if c not in gens]
    assert not missing, f'缺生成图: {missing}'

    if not (DATA / 'train').exists():
        n_real = 0
        for cls in classes:
            for split in ('train', 'val'):
                d = DATA / split / cls
                d.mkdir(parents=True, exist_ok=True)
                shutil.copy2(gens[cls], d / f'gen__{cls}.png')
            src = REAL / real_map[cls] if cls in real_map else None
            if src and src.is_dir():
                fs = sorted(p for p in src.iterdir()
                            if p.suffix.lower() in ('.jpg', '.jpeg', '.png'))
                random.shuffle(fs)
                for f in fs[:REAL_TRAIN]:
                    (DATA / 'train' / cls / f'real__{f.name}').symlink_to(f.resolve())
                for f in fs[REAL_TRAIN:REAL_TRAIN + REAL_VAL]:
                    (DATA / 'val' / cls / f'real__{f.name}').symlink_to(f.resolve())
                n_real += min(len(fs), REAL_TRAIN + REAL_VAL)
        print(f'real photos linked: {n_real} (from {REAL})')
    return classes


def main():
    classes = build()
    (OUT / 'classes19.json').write_text(json.dumps(classes, ensure_ascii=False))
    print('classes:', len(classes))

    train_tf = transforms.Compose([
        transforms.RandomResizedCrop(SIZE, scale=(0.35, 1.0), ratio=(0.6, 1.7)),
        transforms.RandomHorizontalFlip(),
        transforms.ColorJitter(0.4, 0.4, 0.4, 0.12),
        transforms.RandomRotation(20),
        transforms.RandomGrayscale(p=0.03),
        transforms.GaussianBlur(5, sigma=(0.1, 2.5)),
        transforms.ToTensor(),
        transforms.Normalize([0.485, 0.456, 0.406], [0.229, 0.224, 0.225]),
        transforms.RandomErasing(p=0.25, scale=(0.02, 0.2)),
    ])
    val_tf = transforms.Compose([
        transforms.Resize(256), transforms.CenterCrop(SIZE),
        transforms.ToTensor(),
        transforms.Normalize([0.485, 0.456, 0.406], [0.229, 0.224, 0.225]),
    ])

    def safe_loader(path):
        from PIL import Image
        try:
            with Image.open(path) as im:
                return im.convert('RGB')
        except Exception:
            return Image.new('RGB', (SIZE, SIZE), (128, 128, 128))

    train_ds = datasets.ImageFolder(DATA / 'train', train_tf, loader=safe_loader)
    val_ds = datasets.ImageFolder(DATA / 'val', val_tf, loader=safe_loader)
    print('train imgs:', len(train_ds), 'val imgs:', len(val_ds))

    # ImageFolder 字母序 -> yaml 类序重映射（与端侧 fruit19_classes.hpp 一致）
    alpha = train_ds.classes
    assert set(alpha) == set(classes), f'dirs != yaml: {set(alpha) ^ set(classes)}'
    assert val_ds.classes == alpha
    perm = [classes.index(c) for c in alpha]
    train_ds.target_transform = lambda t: perm[t]
    val_ds.target_transform = lambda t: perm[t]

    # 采样权重: 生成图 GEN_W（主要目标）, 真实图 1（辅助泛化）；每轮过采样
    AUG_PER_EPOCH = int(os.environ.get('T80_AUG_PER_EPOCH', '1600'))
    weights = [GEN_W if Path(p).name.startswith('gen__') else 1.0
               for p, _ in train_ds.samples]
    from torch.utils.data import WeightedRandomSampler
    sampler = WeightedRandomSampler(weights, num_samples=AUG_PER_EPOCH, replacement=True)
    n_gen_train = sum(1 for p, _ in train_ds.samples if Path(p).name.startswith('gen__'))
    print(f'train: {n_gen_train} gen + {len(train_ds) - n_gen_train} real, '
          f'{AUG_PER_EPOCH} draws/epoch (gen weight {GEN_W})')
    gen_val_set = {i for i, (p, _) in enumerate(val_ds.samples)
                   if Path(p).name.startswith('gen__')}
    print(f'val: {len(gen_val_set)} gen (目标指标) + {len(val_ds) - len(gen_val_set)} real (参考)')

    model = (mobilenet_v2(weights=MobileNet_V2_Weights.IMAGENET1K_V1) if HAS_WEIGHTS
             else mobilenet_v2(pretrained=True))
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
    train_dl = DataLoader(train_ds, BATCH, sampler=sampler, num_workers=WORKERS,
                          pin_memory=pin, drop_last=True)
    val_dl = DataLoader(val_ds, 128, False, num_workers=4, pin_memory=pin)

    from contextlib import nullcontext
    def amp():
        if AMP == 'bf16':
            return torch.autocast(DEVICE, dtype=torch.bfloat16)
        if AMP == 'fp16':
            return torch.autocast(DEVICE, dtype=torch.float16)
        return nullcontext()

    best = 0.0
    best_r = 0.0
    for ep in range(EPOCHS):
        model.train()
        t0 = time.time()
        loss_sum = n = 0
        for x, y in train_dl:
            x, y = x.to(DEVICE), y.to(DEVICE)
            opt.zero_grad(set_to_none=True)
            with amp():
                out = model(x)
                loss = nn.functional.cross_entropy(out, y, label_smoothing=0.1)
            loss.backward()
            opt.step()
            update_ema(model)
            loss_sum += loss.item() * x.size(0)
            n += x.size(0)
        sched.step()

        model.eval()
        ok = tot = gok = gtot = rok = rtot = 0
        with torch.no_grad():
            base = 0
            for x, y in val_dl:
                x, y = x.to(DEVICE), y.to(DEVICE)
                with amp():
                    pred = model(x).argmax(1)
                hit = (pred == y)
                ok += hit.sum().item(); tot += y.size(0)
                for j in range(y.size(0)):
                    if base + j in gen_val_set:
                        gtot += 1; gok += bool(hit[j])
                    else:
                        rtot += 1; rok += bool(hit[j])
                base += y.size(0)
        acc = ok / tot
        gacc = gok / max(gtot, 1)
        racc = rok / max(rtot, 1)
        print(f'ep{ep:02d} loss {loss_sum/n:.3f} val {acc:.4f} gen19 {gacc:.4f} real {racc:.4f} {time.time()-t0:.0f}s', flush=True)
        torch.save(model.state_dict(), OUT / 'last.pth')
        # best 以生成图（部署目标域）准确率为准，同分再比真实图
        if (gacc, racc) > (best, best_r):
            best, best_r = gacc, racc
            torch.save(model.state_dict(), OUT / 'best.pth')
            torch.save({k: v.half().float() for k, v in ema.items()}, OUT / 'best_ema.pth')
    print('best gen19 acc:', best, 'real acc at best:', best_r)


if __name__ == '__main__':
    main()
