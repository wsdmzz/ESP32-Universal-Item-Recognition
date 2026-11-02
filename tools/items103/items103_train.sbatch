#!/bin/bash
#SBATCH -J items103
#SBATCH -p kshdnormal
#SBATCH -N 1
#SBATCH --gres=dcu:1
#SBATCH -c 8
#SBATCH --mem=16G
#SBATCH -t 04:00:00
#SBATCH -o items103_train_%j.log

cd $HOME/items103

module purge
module load compiler/devtoolset/7.3.1
module load compiler/rocm/dtk-22.04.1
module load apps/PyTorch/1.10/dtk-22.04-py37

# DCU fp32（dtk torch1.10 无 bf16 autocast；batch 32 避免 MIOpen 内部错误，
# 见 items127 时期 commit e841faf）。若 MIOpen 报错，改 T80_DEVICE=cpu 重交。
export T80_DEVICE=cuda
export T80_AMP=off
export T80_BATCH=32
export T80_LR=0.001
export T80_EPOCHS=60
export T80_AUG_PER_EPOCH=2000
export T80_WEIGHT_DECAY=0.01

python tools/items103/train_items103.py
