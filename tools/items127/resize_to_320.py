#!/usr/bin/env python3
"""把 raw_augmented 的增强图统一转成 320x240（与端侧摄像头帧同尺寸同比例）。

为什么要这一步（关键）：
  端侧 esp-dl ImagePreprocessor 是把整帧 320x240 **非等比拉伸**到 224x224
  （scale_x=0.700, scale_y=0.933，物体横向压扁 25%），
  而原先训练用 Resize(256)+CenterCrop(224) 是**等比**的，物体无变形。
  两者几何不一致 -> 圆形物体（橙子/樱桃/脸盆）在端侧被压扁后认不出。

  把训练数据预先拉成 320x240，再让训练/量化都走「直接拉伸到 224」，
  就和端侧完全同一条几何链路，形变一致，模型学到的是端侧真实看到的样子。

两种构图都覆盖：
  - FILL（60%）：非等比拉满 320x240。对应实拍时物品图占满摄像头视野。
  - FIT （40%）：等比缩放到画布内、居中，四周留边。对应实拍时图没占满、四周有背景。

输出: models/items127/raw_aug_320/
"""
from pathlib import Path
import random
from PIL import Image, ImageFilter
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / 'models' / 'items127' / 'raw_augmented'
DST = ROOT / 'models' / 'items127' / 'raw_aug_320'
DST.mkdir(exist_ok=True)

TW, TH = 320, 240          # 端侧摄像头帧尺寸 (FRAMESIZE_QVGA)
FILL_RATIO = 0.6           # 拉满构图占比

random.seed(42)
np.random.seed(42)


def to_fill(img):
    """非等比拉满 320x240 —— 与端侧「整帧直接拉伸」几何完全一致。"""
    return img.resize((TW, TH), Image.BILINEAR)


def to_fit(img):
    """等比缩放居中 + 四周留边 —— 模拟实拍时物品图未占满视野。"""
    w, h = img.size
    scale = min(TW / w, TH / h) * random.uniform(0.72, 0.98)
    nw, nh = max(1, int(w * scale)), max(1, int(h * scale))
    fg = img.resize((nw, nh), Image.BILINEAR)

    mode = random.random()
    if mode < 0.45:
        # 边缘延展：最接近「屏幕内容延伸到边框外」的实拍观感
        arr = np.array(img.resize((TW, TH), Image.BILINEAR))
        canvas = Image.fromarray(arr)
    elif mode < 0.75:
        # 随机纯色背景（桌面/墙面）
        g = random.randint(40, 220)
        jitter = lambda: max(0, min(255, g + random.randint(-25, 25)))
        canvas = Image.new('RGB', (TW, TH), (jitter(), jitter(), jitter()))
    else:
        # 模糊放大的自身作背景（屏幕余光/虚化背景）
        canvas = img.resize((TW, TH), Image.BILINEAR).filter(
            ImageFilter.GaussianBlur(radius=random.uniform(8, 20)))

    canvas.paste(fg, ((TW - nw) // 2, (TH - nh) // 2))
    return canvas


def main():
    files = sorted(SRC.glob('*.png'))
    assert files, f'源目录为空: {SRC}'
    print(f'src: {len(files)} images -> {DST}')

    n_fill = n_fit = 0
    for i, f in enumerate(files, 1):
        img = Image.open(f).convert('RGB')
        if random.random() < FILL_RATIO:
            out, tag = to_fill(img), 'fill'
            n_fill += 1
        else:
            out, tag = to_fit(img), 'fit'
            n_fit += 1
        # 文件名保留构图标记，便于事后分析哪种构图识别差
        # JPEG 存盘：顺带模拟摄像头的有损压缩，体积也小得多
        out.save(DST / f'{f.stem}__{tag}.jpg', quality=random.randint(78, 95))
        if i % 200 == 0:
            print(f'  {i}/{len(files)}', flush=True)

    print(f'\ndone: {len(list(DST.glob("*")))} images '
          f'({n_fill} fill / {n_fit} fit) -> {DST}')


if __name__ == '__main__':
    main()
