#!/usr/bin/env python3
"""训练好的 80 类 MobileNetV2 -> ESP32-S3 INT8 .espdl。

输入协议：模型接受训练同款 Normalize 后的张量 ((x/255-m)/s)。
端侧 esp-dl ImagePreprocessor 用 mean=255*m, std=255*s 在 [0,255] 域做等价归一化，
归一化在卷积前完成，padding 语义与训练完全一致（不折算进首层 conv）。

用法: /home/oooa/miniforge3/envs/espdl/bin/python tools/dataset80/export_espdl.py
"""
from pathlib import Path

import torch
import yaml
from torch.utils.data import DataLoader
from torchvision import transforms
from torchvision.models import mobilenet_v2

from esp_ppq import QuantizationSettingFactory
from esp_ppq.api import espdl_quantize_torch

ROOT = Path(__file__).resolve().parents[2]
DATA = ROOT / 'models' / 'dataset80'
OUT = ROOT / 'models' / 'train80'
ESPDL = ROOT / 'espdl_files' / 'items80.espdl'
SIZE = 224
DEVICE = 'cuda'


def build_model():
    spec = yaml.safe_load((ROOT / 'models' / 'classes80.yaml').read_text())
    classes = [c for g in spec['groups'].values() for c in g]
    model = mobilenet_v2(weights=None)
    model.classifier[-1] = torch.nn.Linear(model.classifier[-1].in_features, len(classes))
    sd = torch.load(OUT / 'best.pth', map_location='cpu')
    model.load_state_dict(sd)
    model.eval().to(DEVICE)
    return model, classes


def calib_data(n=16):
    """归一化域标定样本（与设备端 ImagePreprocessor 输出同域）。"""
    from PIL import Image
    tf = transforms.Compose([
        transforms.Resize(SIZE), transforms.CenterCrop(SIZE), transforms.ToTensor(),
        transforms.Normalize([0.485, 0.456, 0.406], [0.229, 0.224, 0.225]),
    ])
    imgs = []
    src = DATA / 'val' if (DATA / 'val').exists() else DATA / 'images'
    for d in sorted(p for p in src.iterdir() if p.is_dir()):
        fs = sorted(d.glob('*'))
        if not fs:
            continue
        p = fs[len(fs) // 2]
        try:
            im = Image.open(p).convert('RGB')
            imgs.append(tf(im))
        except Exception:
            pass
        if len(imgs) >= n:
            break
    return imgs


def main():
    ESPDL.parent.mkdir(exist_ok=True)
    model, classes = build_model()
    x = torch.randn(1, 3, SIZE, SIZE, device=DEVICE)
    with torch.no_grad():
        out = model(x)
    print('sanity float out[0..5]:', out[0, :5].tolist())

    quant_setting = QuantizationSettingFactory.espdl_setting()
    calib = calib_data()
    dl = DataLoader(calib, batch_size=1, shuffle=False)
    graph = espdl_quantize_torch(
        model=model,
        espdl_export_file=str(ESPDL),
        calib_dataloader=dl,
        calib_steps=len(calib),
        input_shape=[1, 3, SIZE, SIZE],
        target='esp32s3',
        num_of_bits=8,
        collate_fn=lambda b: b.to(DEVICE),
        setting=quant_setting,
        device=DEVICE,
        error_report=True,
        skip_export=False,
        export_test_values=True,
        verbose=1,
    )
    print('exported:', ESPDL, round(ESPDL.stat().st_size / 1048576, 2), 'MB')


if __name__ == '__main__':
    main()
