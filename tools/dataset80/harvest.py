#!/usr/bin/env python3
"""把各类数据源汇总成 models/dataset80/images/<class>/ 目录树。

用法: /home/oooa/msdl/bin/python tools/dataset80/harvest.py [--only class1,class2 ...]
数据源标记见 models/classes80.yaml 注释。
"""
import argparse
import shutil
import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]
MODELS = ROOT / 'models'
DATASET = MODELS / 'dataset80'
IMAGES = DATASET / 'images'
CACHE = Path.home() / '.cache/modelscope/hub/datasets'

MS = {'f100': CACHE / 'tany0699/fruits100/master/data_files/extracted',
      'veg': CACHE / 'ai0pua/vegetable/master/data_files/extracted',
      'f14': CACHE / 'tany0699/flowers14/master/data_files/extracted'}


def find_class_dir(base: Path, split: str, folder: str):
    """在 hash 目录下寻找 <split>/<folder> 或 <split>/<parent>/<folder>。"""
    hits = []
    for h in sorted(base.glob('*/')):
        for cand in (h / split / folder, h / 'Vegetable Images' / split / folder):
            if cand.is_dir():
                hits.append(cand)
    return hits


def copy_images(src_files, dst: Path, cap=None):
    dst.mkdir(parents=True, exist_ok=True)
    existing = {p.name for p in dst.iterdir()}
    n = 0
    files = sorted(src_files)
    if cap and len(files) > cap:
        step = len(files) / cap
        files = [files[int(i * step)] for i in range(cap)]
    for f in files:
        if f.name in existing:
            continue
        try:
            shutil.copy2(f, dst / f.name)
            n += 1
        except OSError:
            pass
    return n


def jpgs(d: Path):
    return [p for p in d.rglob('*') if p.suffix.lower() in ('.jpg', '.jpeg', '.png') and p.is_file()]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--only')
    ap.add_argument('--cap', type=int, default=600, help='每类每源最多复制图片数')
    args = ap.parse_args()

    spec = yaml.safe_load((MODELS / 'classes80.yaml').read_text())
    order = [c for g in spec['groups'].values() for c in g]
    wanted = args.only.split(',') if args.only else order

    for cls in wanted:
        dst = IMAGES / cls
        total_before = len(list(dst.glob('*'))) if dst.exists() else 0
        copied = 0
        for src in spec['sources'].get(cls) or []:
            tag, _, ref = src.partition(':')
            if tag == 'in':
                continue  # ImageNet 由 build_imagenet 处理
            if tag == 'commons':
                continue  # commons 由 commons_fetch 处理
            if tag == 'f102':
                continue  # flowers102 由 build_flowers102 处理
            hits = []
            for split in ('train', 'validation', 'val'):
                hits += find_class_dir(MS[tag], split, ref)
            if not hits:
                print(f'  !! {cls}: source {src} not found', file=sys.stderr)
                continue
            for h in hits:
                copied += copy_images(jpgs(h), dst, args.cap)
        total_after = len(list(dst.glob('*'))) if dst.exists() else 0
        print(f'{cls:16s} {total_before:5d} -> {total_after:5d}')


if __name__ == '__main__':
    main()
