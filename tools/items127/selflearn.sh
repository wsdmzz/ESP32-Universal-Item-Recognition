#!/usr/bin/env bash
# 自学习闭环第 2 步：增量微调 + 量化 + 烧录（一条命令）
#
# 前置：已在 http://localhost:8765 标注台确认/改判过若干实拍帧
#       （样本在 models/items127/real_captures/<类>/，train_items127.py 自动混入 train）
#
# 用法:  bash tools/items127/selflearn.sh
# 调参:  EPOCHS=12 LR=0.0005 bash tools/items127/selflearn.sh
set -e
cd "$(dirname "$0")/../.."

EPOCHS=${EPOCHS:-8}
LR=${LR:-0.0002}

echo "== 1/3 增量微调: 从 last.pth 继续, ${EPOCHS} epochs, lr=${LR} =="
T80_FINETUNE=last.pth T80_LR=$LR T80_EPOCHS=$EPOCHS T80_WEIGHT_DECAY=0.001 \
    python3.11 tools/items127/train_items127.py

echo "== 2/3 INT8 量化导出 =="
/home/oooa/miniforge3/envs/espdl/bin/python tools/items127/export_items127.py last.pth

echo "== 3/3 烧录到设备 =="
source /home/oooa/AIQ/esp-idf/export.sh >/dev/null
idf.py -p /dev/ttyACM0 flash

echo
echo "完成。回标注台 http://localhost:8765 继续确认/改判；"
echo "同一屏幕再认不对就再攒几帧跑一次本脚本，模型逐轮收敛。"
