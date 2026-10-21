#!/usr/bin/env python3
"""训练好的 127 类常见物品 MobileNetV2 -> ESP32-S3 INT8 .espdl。

输入协议: 模型接受归一化域输入（ToTensor[0,1] -> ImageNet Normalize），
端侧 ImagePreprocessor mean/std = 255*ImageNet 统计量，与之精确对齐。

校准集 = 127 张即梦生成图原图（闭集目标域）。

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
DEVICE = 'cuda'


def build_model():
    spec = yaml.safe_load((ROOT / 'models' / 'items127.yaml').read_text())
    classes = spec['groups']['items127']
    model = mobilenet_v2(weights=None)
    model.classifier[-1] = torch.nn.Linear(model.classifier[-1].in_features, len(classes))
    sd = torch.load(OUT / 'best.pth', map_location='cpu')
    model.load_state_dict(sd)
    model.eval().to(DEVICE)
    return model, classes


def calib_data():
    from PIL import Image
    tf = transforms.Compose([transforms.Resize(256), transforms.CenterCrop(SIZE),
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
    ESPDL.parent.mkdir(exist_ok=True)
    model, classes = build_model()
    x = torch.randn(1, 3, SIZE, SIZE, device=DEVICE)
    with torch.no_grad():
        out = model(x)
    print('sanity float out shape:', tuple(out.shape), 'out[0..4]:', out[0, :4].tolist())

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
        error_report=True,
        skip_export=False,
        export_test_values=True,
        verbose=1,
    )
    print('exported:', ESPDL, round(ESPDL.stat().st_size / 1048576, 2), 'MB')


if __name__ == '__main__':
    main()
