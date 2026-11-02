#!/usr/bin/env python3
"""从 models/items103.yaml 生成 components/items103/include/items103_classes.hpp。

顺序 = yaml groups.items103（字母序，与训练 target 一致）。
CN 取 zh_map 中文显示名，EN 取类键名。
"""
from pathlib import Path
import yaml

ROOT = Path(__file__).resolve().parents[2]
spec = yaml.safe_load((ROOT / 'models' / 'items103.yaml').read_text())
classes = spec['groups']['items103']
zh = spec['zh_map']
assert len(classes) == 103 and classes == sorted(classes)

out = ROOT / 'components' / 'items103' / 'include' / 'items103_classes.hpp'
out.parent.mkdir(parents=True, exist_ok=True)
lines = [
    '// items103_classes.hpp — auto-generated from models/items103.yaml, DO NOT EDIT',
    '// 103 classes closed-set screen recognition (真实照片增强数据集)',
    '#pragma once',
    '',
    f'static const char *ITEMS103_CN[{len(classes)}] = {{',
]
lines += [f'    "{zh[c]}",' for c in classes]
lines += [
    '};',
    '',
    f'static const char *ITEMS103_EN[{len(classes)}] = {{',
]
lines += [f'    "{c}",' for c in classes]
lines += ['};', '']
out.write_text('\n'.join(lines), encoding='utf-8')
print(f'wrote {out} ({len(classes)} classes)')
