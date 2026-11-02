#!/usr/bin/env python3
"""使用 fruit19 的成功配置重新量化 items127 模型"""
from pathlib import Path
import torch
import yaml
from torch.utils.data import DataLoader
from torchvision import transforms
from torchvision.models import mobilenet_v2
from esp_ppq import QuantizationSettingFactory
from esp_ppq.api import espdl_quantize_torch

ROOT = Path(__file__).resolve().parents[2]
# 使用 items127 的数据和训练产物
DATA = ROOT / 'models' / 'dataset_items127' / 'val'
OUT = ROOT / 'models' / 'train_items127'
ESPDL = ROOT / 'espdl_files' / 'items127_new.espdl'  # 新文件名避免覆盖
SIZE = 224
DEVICE = 'cpu'  # CPU mode


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
    
    print(f"Model has {len(classes)} classes")
    x = torch.randn(1, 3, SIZE, SIZE, device=DEVICE)
    with torch.no_grad():
        out = model(x)
    print('sanity float out shape:', tuple(out.shape), 'out[0..4]:', out[0, :4].tolist())

    quant_setting = QuantizationSettingFactory.espdl_setting()
    calib = calib_data()
    print('calibration images:', len(calib))
    dl = DataLoader(calib, batch_size=1, shuffle=False)
    
    # 使用与 fruit19 完全相同的参数
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
