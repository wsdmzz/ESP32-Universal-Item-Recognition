#!/usr/bin/env python3
"""从 tany0699/imagenet_val（modelscope 缓存）按类别索引复制图片到 dataset80。"""
import sys
from pathlib import Path

import yaml
from modelscope.msdatasets import MsDataset

ROOT = Path(__file__).resolve().parents[2]
IMAGES = ROOT / 'models' / 'dataset80' / 'images'
CAP = 600


def main():
    spec = yaml.safe_load((ROOT / 'models' / 'classes80.yaml').read_text())
    idx2cls = {}
    for cls, srcs in (spec['sources'] or {}).items():
        for s in srcs or []:
            if s.startswith('in:'):
                idx2cls.setdefault(int(s[3:]), []).append(cls)

    ds = MsDataset.load('imagenet_val', namespace='tany0699', subset_name='default',
                        split='validation')
    kept = {c: 0 for c in idx2cls.values() for c in [cls for cls in c]}
    for row in ds:
        cat = row.get('category')
        path = row.get('image:FILE') or row.get('image')
        if cat not in idx2cls or not isinstance(path, str):
            continue
        for cls in idx2cls[cat]:
            if kept[cls] >= CAP:
                continue
            dst = IMAGES / cls
            dst.mkdir(parents=True, exist_ok=True)
            name = f'in{cat:04d}_{Path(path).name}'
            tgt = dst / name
            if not tgt.exists():
                import shutil
                shutil.copy2(path, tgt)
            kept[cls] += 1
    print({k: v for k, v in sorted(kept.items())})


if __name__ == '__main__':
    sys.exit(main())
