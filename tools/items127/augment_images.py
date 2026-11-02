#!/usr/bin/env python3
"""对 items127 的 127 张原始生成图进行增强，模拟真实拍摄屏幕的效果。

关键改进：加入摩尔纹、屏幕反光、拍摄角度、环境光、摄像头噪声等真实特征
输出到 models/items127/raw_augmented/
"""
from pathlib import Path
import random
from PIL import Image, ImageEnhance, ImageFilter, ImageDraw
import numpy as np
import cv2

ROOT = Path(__file__).resolve().parents[2]
RAW = ROOT / 'models' / 'items127' / 'raw'
AUG = ROOT / 'models' / 'items127' / 'raw_augmented'
AUG.mkdir(exist_ok=True)

random.seed(42)


def add_moire_pattern(img, intensity=0.3):
    """添加摩尔纹（屏幕像素网格与摄像头传感器干涉）"""
    arr = np.array(img, dtype=np.float32)
    h, w = arr.shape[:2]
    
    # 生成摩尔纹图案（正弦波干涉）
    freq = random.uniform(0.05, 0.15)
    angle = random.uniform(0, np.pi)
    
    x = np.arange(w)
    y = np.arange(h)
    xx, yy = np.meshgrid(x, y)
    
    # 旋转坐标
    xx_rot = xx * np.cos(angle) - yy * np.sin(angle)
    yy_rot = xx * np.sin(angle) + yy * np.cos(angle)
    
    moire = np.sin(2 * np.pi * freq * xx_rot) * np.sin(2 * np.pi * freq * yy_rot)
    moire = (moire + 1) / 2  # Normalize to [0, 1]
    
    # 叠加到图像
    for c in range(3):
        arr[:, :, c] = arr[:, :, c] * (1 - intensity * 0.3) + moire * 255 * intensity * 0.3
    
    arr = np.clip(arr, 0, 255).astype(np.uint8)
    return Image.fromarray(arr)


def add_screen_glare(img):
    """添加屏幕反光（局部高光区域）"""
    arr = np.array(img, dtype=np.float32)
    h, w = arr.shape[:2]
    
    # 随机位置的反光椭圆
    cx = random.randint(w // 4, 3 * w // 4)
    cy = random.randint(h // 4, 3 * h // 4)
    rx = random.randint(w // 6, w // 3)
    ry = random.randint(h // 6, h // 3)
    
    # 创建反光蒙版
    mask = np.zeros((h, w), dtype=np.float32)
    cv2.ellipse(mask, (cx, cy), (rx, ry), random.uniform(0, 360), 0, 360, 1, -1)
    mask = cv2.GaussianBlur(mask, (51, 51), 0)
    
    # 白色反光
    glare_color = np.array([random.uniform(200, 255) for _ in range(3)], dtype=np.float32)
    for c in range(3):
        arr[:, :, c] = arr[:, :, c] * (1 - mask * 0.4) + glare_color[c] * mask * 0.4
    
    arr = np.clip(arr, 0, 255).astype(np.uint8)
    return Image.fromarray(arr)


def simulate_camera_angle(img):
    """模拟拍摄角度倾斜（透视变换）"""
    arr = np.array(img)
    h, w = arr.shape[:2]
    
    # 随机透视变换
    pts1 = np.float32([[0, 0], [w, 0], [w, h], [0, h]])
    
    offset_x = random.uniform(-w * 0.1, w * 0.1)
    offset_y = random.uniform(-h * 0.1, h * 0.1)
    
    pts2 = np.float32([
        [offset_x, offset_y],
        [w - offset_x, offset_y * 0.5],
        [w - offset_x * 0.5, h - offset_y],
        [offset_x * 0.5, h - offset_y * 0.5]
    ])
    
    matrix = cv2.getPerspectiveTransform(pts1, pts2)
    warped = cv2.warpPerspective(arr, matrix, (w, h), borderMode=cv2.BORDER_REPLICATE)
    
    return Image.fromarray(warped)


def add_environment_lighting(img):
    """添加环境光影响（色偏）"""
    arr = np.array(img, dtype=np.float32)
    
    # 随机色温偏移（暖光/冷光）
    temp_shift = random.uniform(-30, 30)
    
    if temp_shift > 0:  # 暖光（偏黄红）
        arr[:, :, 0] *= (1 + temp_shift / 200)
        arr[:, :, 1] *= (1 + temp_shift / 400)
        arr[:, :, 2] *= (1 - temp_shift / 300)
    else:  # 冷光（偏蓝）
        arr[:, :, 0] *= (1 + temp_shift / 300)
        arr[:, :, 1] *= (1 - temp_shift / 400)
        arr[:, :, 2] *= (1 - temp_shift / 200)
    
    arr = np.clip(arr, 0, 255).astype(np.uint8)
    return Image.fromarray(arr)


def add_camera_noise(img):
    """添加摄像头噪声（高斯噪声 + ISO 噪点）"""
    arr = np.array(img, dtype=np.float32)
    
    # 高斯噪声
    noise_level = random.uniform(5, 15)
    noise = np.random.normal(0, noise_level, arr.shape)
    arr = arr + noise
    
    # 彩色噪点（模拟低光照下的色彩噪声）
    color_noise = np.random.uniform(-10, 10, arr.shape)
    arr = arr + color_noise * 0.3
    
    arr = np.clip(arr, 0, 255).astype(np.uint8)
    return Image.fromarray(arr)


def add_screen_pixel_grid(img):
    """模拟屏幕像素网格可见"""
    arr = np.array(img, dtype=np.float32)
    h, w = arr.shape[:2]
    
    # 创建像素网格图案
    grid_spacing = random.choice([2, 3, 4])
    grid_pattern = np.zeros((h, w), dtype=np.float32)
    
    for y in range(0, h, grid_spacing):
        grid_pattern[y, :] = random.uniform(0.02, 0.05)
    for x in range(0, w, grid_spacing):
        grid_pattern[:, x] = random.uniform(0.02, 0.05)
    
    # 叠加网格
    for c in range(3):
        arr[:, :, c] = arr[:, :, c] * (1 - grid_pattern) + grid_pattern * 255 * 0.5
    
    arr = np.clip(arr, 0, 255).astype(np.uint8)
    return Image.fromarray(arr)


def add_motion_blur(img):
    """添加运动模糊（手持拍摄抖动）"""
    arr = np.array(img)
    
    # 随机方向的运动模糊
    kernel_size = random.choice([5, 7, 9])
    angle = random.uniform(0, 360)
    
    # 创建运动模糊核
    kernel = np.zeros((kernel_size, kernel_size))
    center = kernel_size // 2
    dx = int(center * np.cos(np.radians(angle)))
    dy = int(center * np.sin(np.radians(angle)))
    
    # 简化的运动模糊
    kernel[center, :] = 1.0 / kernel_size
    
    blurred = cv2.filter2D(arr, -1, kernel)
    return Image.fromarray(blurred.astype(np.uint8))


def augment_v1_screen_photo(img):
    """变体 1：完整屏幕拍摄模拟（摩尔纹 + 反光 + 角度 + 噪声）"""
    img = add_moire_pattern(img, intensity=0.3)
    img = add_screen_glare(img)
    img = simulate_camera_angle(img)
    img = add_camera_noise(img)
    img = ImageEnhance.Brightness(img).enhance(random.uniform(0.9, 1.1))
    return img


def augment_v2_indoor_lighting(img):
    """变体 2：室内光照条件（色偏 + 对比度变化）"""
    img = add_environment_lighting(img)
    img = ImageEnhance.Contrast(img).enhance(random.uniform(0.8, 1.2))
    img = add_camera_noise(img)
    img = img.rotate(random.uniform(-5, 5), resample=Image.BICUBIC, expand=False)
    return img


def augment_v3_handheld_shake(img):
    """变体 3：手持拍摄抖动（运动模糊 + 角度）"""
    img = add_motion_blur(img)
    img = simulate_camera_angle(img)
    img = add_camera_noise(img)
    img = ImageEnhance.Brightness(img).enhance(random.uniform(0.85, 1.15))
    return img


def augment_v4_low_quality_camera(img):
    """变体 4：低质量摄像头效果（强噪声 + 模糊 + 色彩失真）"""
    img = add_camera_noise(img)
    img = img.filter(ImageFilter.GaussianBlur(radius=random.uniform(1.0, 2.0)))
    img = ImageEnhance.Color(img).enhance(random.uniform(0.6, 0.8))
    img = add_screen_pixel_grid(img)
    return img


def augment_v5_glare_heavy(img):
    """变体 5：强反光场景（多重反光 + 过曝）"""
    img = add_screen_glare(img)
    img = add_screen_glare(img)  # 二次反光
    img = ImageEnhance.Brightness(img).enhance(random.uniform(1.1, 1.3))
    img = add_moire_pattern(img, intensity=0.2)
    return img


def augment_v6_angle_extreme(img):
    """变体 6：极端拍摄角度（大角度倾斜 + 透视变形）"""
    img = simulate_camera_angle(img)
    img = img.rotate(random.uniform(-20, 20), resample=Image.BICUBIC, expand=False)
    img = add_camera_noise(img)
    img = ImageEnhance.Contrast(img).enhance(random.uniform(0.9, 1.1))
    return img


def augment_v7_mixed_conditions(img):
    """变体 7：混合条件（轻度所有效果）"""
    img = add_moire_pattern(img, intensity=0.15)
    img = add_environment_lighting(img)
    img = add_camera_noise(img)
    img = img.filter(ImageFilter.GaussianBlur(radius=0.5))
    img = ImageEnhance.Brightness(img).enhance(random.uniform(0.95, 1.05))
    return img


def augment_v8_night_mode(img):
    """变体 8：夜间/低光照模式（高 ISO 噪声 + 暗调）"""
    img = ImageEnhance.Brightness(img).enhance(random.uniform(0.6, 0.8))
    img = add_camera_noise(img)
    img = ImageEnhance.Color(img).enhance(random.uniform(0.7, 0.9))
    img = img.filter(ImageFilter.GaussianBlur(radius=random.uniform(0.5, 1.0)))
    return img


def augment_v9_display_artifacts(img):
    """变体 9：显示器伪影（像素网格 + 摩尔纹 + 色带）"""
    img = add_screen_pixel_grid(img)
    img = add_moire_pattern(img, intensity=0.25)
    img = ImageEnhance.Contrast(img).enhance(random.uniform(1.1, 1.3))
    return img


def augment_v10_realistic_combined(img):
    """变体 10：最真实的综合效果（模拟实际拍摄）"""
    # 按真实拍摄的概率分布应用效果
    img = add_moire_pattern(img, intensity=random.uniform(0.2, 0.4))
    
    if random.random() > 0.3:  # 70% 概率有反光
        img = add_screen_glare(img)
    
    if random.random() > 0.4:  # 60% 概率有色偏
        img = add_environment_lighting(img)
    
    img = add_camera_noise(img)
    
    if random.random() > 0.5:  # 50% 概率有角度
        img = simulate_camera_angle(img)
    
    if random.random() > 0.6:  # 40% 概率有模糊
        img = img.filter(ImageFilter.GaussianBlur(radius=random.uniform(0.3, 1.0)))
    
    # 最终亮度和对比度调整
    img = ImageEnhance.Brightness(img).enhance(random.uniform(0.85, 1.15))
    img = ImageEnhance.Contrast(img).enhance(random.uniform(0.9, 1.1))
    
    return img


def main():
    aug_funcs = [
        ('orig', lambda img: img.copy()),
        ('v1_screen_photo', augment_v1_screen_photo),
        ('v2_indoor_lighting', augment_v2_indoor_lighting),
        ('v3_handheld_shake', augment_v3_handheld_shake),
        ('v4_low_quality_camera', augment_v4_low_quality_camera),
        ('v5_glare_heavy', augment_v5_glare_heavy),
        ('v6_angle_extreme', augment_v6_angle_extreme),
        ('v7_mixed_conditions', augment_v7_mixed_conditions),
        ('v8_night_mode', augment_v8_night_mode),
        ('v9_display_artifacts', augment_v9_display_artifacts),
        ('v10_realistic_combined', augment_v10_realistic_combined),
    ]

    total = 0
    skipped = 0
    for f in sorted(RAW.glob('*.png')):
        try:
            img = Image.open(f).convert('RGB')
        except Exception as e:
            print(f'Skip {f.name}: {e}')
            skipped += 1
            continue

        # 从文件名提取品种名（格式：jimeng-...-超写实...，品种，...png）
        name_parts = f.stem.split('，')
        variety = name_parts[1] if len(name_parts) >= 2 else f.stem[:20]

        for suffix, func in aug_funcs:
            out_name = f'{variety}_{suffix}.png'
            try:
                img_out = func(img)
                img_out.save(AUG / out_name)
                total += 1
            except Exception as e:
                print(f'Error saving {out_name}: {e}')
                skipped += 1

    print(f'\nAugmentation complete:')
    print(f'  Original images: {len(list(RAW.glob("*.png")))}')
    print(f'  Augmented images: {total}')
    print(f'  Skipped: {skipped}')
    print(f'  Output directory: {AUG}')


if __name__ == '__main__':
    main()
