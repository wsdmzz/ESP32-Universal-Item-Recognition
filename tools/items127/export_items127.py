#!/usr/bin/env python3
"""训练好的 127 类常见物品 MobileNetV2 -> ESP32-S3 INT8 .espdl。

输入协议: 模型接受归一化域输入（ToTensor[0,1] -> ImageNet Normalize），
端侧 ImagePreprocessor mean/std = 255*ImageNet 统计量，与之精确对齐。

几何协议（重要）: 端侧把整帧 320x240 非等比拉伸到 224x224。训练数据已预先
拉成 320x240（tools/items127/resize_to_320.py），校准集用 Resize((224,224))，
三处几何链路一致。切勿改回 Resize(256)+CenterCrop —— 那是等比语义，会与端侧形变错位。

校准集 = dataset_items127/val（381 张 320x240 屏幕模拟增强图，闭集目标域）。

用法: /home/oooa/miniforge3/envs/espdl/bin/python tools/items127/export_items127.py
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
DATA = ROOT / 'models' / 'dataset_items127' / 'val'   # 127 张生成图
OUT = ROOT / 'models' / 'train_items127'
ESPDL = ROOT / 'espdl_files' / 'items127.espdl'
SIZE = 224
DEVICE = 'cpu'  # Use CPU for quantization (CUDA not required)


def build_model(ckpt='last.pth'):
    # 2026-10-23 实测：val acc 从 ep21 就饱和到 1.0，best.pth 停在 ep21；
    # 而 last.pth(ep60) val loss 0.089 << best.pth 0.235，实拍樱桃置信度
    # 70-79% vs 41-55%。故默认导出 last.pth。best_ema.pth 已确认是废权重。
    spec = yaml.safe_load((ROOT / 'models' / 'items127.yaml').read_text())
    classes = spec['groups']['items127']
    model = mobilenet_v2(weights=None)
    model.classifier[-1] = torch.nn.Linear(model.classifier[-1].in_features, len(classes))
    sd = torch.load(OUT / ckpt, map_location='cpu')
    model.load_state_dict(sd)
    model.eval().to(DEVICE)
    return model, classes


def calib_data():
    from PIL import Image
    # 必须与训练 val_tf / 端侧 esp-dl 完全一致：整帧非等比拉伸到 224x224。
    # 数据集已预先拉成 320x240（见 resize_to_320.py），此处再拉到 224 即等价端侧。
    # 原先的 Resize(256)+CenterCrop(224) 是等比语义，与端侧形变不同，会让
    # INT8 校准统计量偏离端侧真实输入分布。
    tf = transforms.Compose([transforms.Resize((SIZE, SIZE)),
                             transforms.ToTensor(),
                             transforms.Normalize([0.485, 0.456, 0.406],
                                                  [0.229, 0.224, 0.225])])
    imgs = []
    for d in sorted(p for p in DATA.iterdir() if p.is_dir()):
        for p in sorted(d.glob('*')):
            try:
                im = Image.open(p).convert('RGB')
                imgs.append(tf(im).float())
            except Exception:
                pass
    return imgs


def main():
    import sys
    ckpt = sys.argv[1] if len(sys.argv) > 1 else 'last.pth'
    ESPDL.parent.mkdir(exist_ok=True)
    model, classes = build_model(ckpt)
    print('checkpoint:', ckpt)
    x = torch.randn(1, 3, SIZE, SIZE, device=DEVICE)
    with torch.no_grad():
        out = model(x)
    print('sanity float out shape:', tuple(out.shape), 'out[0..4]:', out[0, :4].tolist())

    # 使用 espdl 标准配置
    quant_setting = QuantizationSettingFactory.espdl_setting()
    
    calib = calib_data()
    print('calib images:', len(calib))
    dl = DataLoader(calib, batch_size=1, shuffle=False)
    espdl_quantize_torch(
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
        error_report=True,  # 恢复错误报告以检测问题
        skip_export=False,
        export_test_values=True,
        verbose=1,
    )
    print('exported:', ESPDL, round(ESPDL.stat().st_size / 1048576, 2), 'MB')


if __name__ == '__main__':
    main()
