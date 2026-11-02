# 模型链路：从 AI 生图到 ESP32-S3 上跑起来的 INT8

当前在用模型：**items103** —— MobileNetV2 / 103 类常见物品分类 / INT8 espdl，
2,652,848 字节，跑在 `items103` 分区（flash 0x410000）。

## 0. 全链路一张图

```
即梦 AI 生图 103 张 1280x800
  生活常见品图片/photos_1280x800_raw/<类>.jpg          ← 入库（唯一不可再生的源头）
        │  tools/scale_to_480x320.py   （fit-inside + 边框均值补边）
        ▼
  生活常见品图片/photos_480x320/<类>.jpg               ← 与 spiffs_data/photos 逐字节相同，不入库
  spiffs_data/photos/<类>.jpg                          ← 入库：既是出题图库，也是增强图的源
        │  （屏幕模拟增强，20 张/类 —— 生成脚本未留存，见 §1 的警告）
        ▼
  augmented/<类>/*.jpg  480x320, 103x20 = 2060 张      ← 入库（89MB，删了就复现不出这个模型）
        │  tools/items103/augment_to_240.py  （每张同时产出 __fill 与 __fit 两份）
        ▼
  models/items103/raw_aug_240/<类>/*  320x240, 4120 张 ← 不入库，可再生
        │  tools/items103/train_items103.py  （80/20 切分 + 混入实拍帧）
        ▼
  models/dataset_items103/{train 3296, val 824}        ← 不入库，可再生
        │  训练（本地 CPU 或 Cancon DCU）
        ▼
  models/train_items103/{best.pth, best_ema.pth, last.pth, classes103.json}
        │  tools/items103/export_items103.py  （espdl INT8 量化，校准集 = val 824 张）
        ▼
  espdl_files/items103.espdl                           ← 入库
        │  顶层 CMakeLists.txt: esptool_py_flash_to_partition(flash "items103" ...)
        ▼
  设备 items103 分区 @0x410000  →  components/items103 加载推理
```

类名单与中文名：`models/items103.yaml`（`groups.items103` 字母序 103 项 + `zh_map`）
→ `tools/items103/gen_classes_hpp.py` → `components/items103/include/items103_classes.hpp`
（`ITEMS103_CN[103]` / `ITEMS103_EN[103]`）。**顺序即 target 顺序，改动必须同步重训。**

## 1. 重建训练数据

```bash
# 1280x800 -> 480x320（不形变、不裁内容，居中贴到画布上用边框均值补边）
python3 tools/scale_to_480x320.py
cp 生活常见品图片/photos_480x320/*.jpg spiffs_data/photos/

# 480x320 屏幕模拟增强 -> 320x240，同时产出两种构图
python3 tools/items103/augment_to_240.py
```

> ⚠ **`augmented/` 的生成脚本没有留存。** 那 2060 张是「拍屏模拟」增强
> （摩尔纹 / 屏幕反光 / 色偏 / 透视 / 运动模糊 / 传感器噪声 的随机组合，
> 文件名里就写着用了哪些，如 `apple_003_reflection_color_cast_pixel_grid_motion_blur_moire.jpg`），
> 与 `tools/items127/augment_images.py` 的 v1..v10 方案不是同一套。
> 因此这一层**整目录入库**，不要删。要新增类别时只能照它的效果集合重写生成器，
> 或者走 `docs/selflearn.md` 的实拍微调路线。

`augment_to_240.py` 的两种构图对应两条硬件链路，这是 127→103 迁移时定下的：

| 后缀 | 做法 | 模拟的是 |
|------|------|----------|
| `__fill` | 非等比拉满 320x240 | 旧 2.0" 320x240 屏占满视野 |
| `__fit` | 等比缩放置入、左右留边 | 新 4.0" 480x320(3:2) 屏被 4:3 摄像头拍到的真实构图 |

## 2. ★ 三处几何必须一致（最容易改错的地方）

端侧 esp-dl 把整帧 **320x240 非等比拉伸到 224x224**。所以：

1. 训练数据预先转成 320x240（`augment_to_240.py`）；
2. 导出时校准集用 `Resize((224, 224))`，**不是** `Resize(256)+CenterCrop`；
3. 端侧 `ImagePreprocessor` 直接吃 `img_t`（320x240 或裁剪后的 128x84），由 esp-dl 拉伸。

`Resize(256)+CenterCrop` 是**等比**语义，会和端侧的形变错位，模型当场废掉。
改任何一环都要同时改另外两环。

输入协议同理锁死：训练侧 `ToTensor()[0,1] -> Normalize(ImageNet)`，
端侧 esp-dl `NormQuant` 算的是 `(raw - mean)/std` 且 `raw ∈ [0,255]`，
所以固件里的 mean/std 必须是 **255 × ImageNet 统计量**：

```cpp
mean = {0.485f*255, 0.456f*255, 0.406f*255}
std  = {0.229f*255, 0.224f*255, 0.225f*255}
```

## 3. 训练

### 3.1 本地 CPU（小规模验证用）

```bash
python3 tools/items103/train_items103.py
```

### 3.2 Cancon DCU 集群（正式训练）

```bash
# 上传：项目根 -> $HOME/items103（rsync 失败要重试，超算网络抖）
ssh -p 65023 scnd60f5d7@<cancon>
cd ~/items103 && sbatch tools/items103/items103_train.sbatch
```

sbatch 里的环境是硬约束，别按本机经验改：

| 项 | 值 | 原因 |
|----|----|------|
| 队列/资源 | `-p kshdnormal --gres=dcu:1 -c 8 --mem=16G -t 04:00:00` | |
| module | `devtoolset/7.3.1` + `rocm/dtk-22.04.1` + `PyTorch/1.10/dtk-22.04-py37` | DCU 只有这套能起 |
| `T80_AMP=off` | 关混合精度 | torch1.10 那套没有 bf16 autocast |
| `T80_BATCH=32` | 不要调大 | batch 64 触发 MIOpen 内部错误（见 commit `e841faf`） |
| `T80_DEVICE=cuda` | dtk 的 torch 把 DCU 伪装成 cuda 后端 | |

超算上没有 llama-server 抢卡，但本机 GPU 常驻 llama-server，所以本地训练只走 CPU。

环境变量全集（`train_items103.py`）：
`T80_DEVICE T80_AMP T80_BATCH T80_LR T80_EPOCHS T80_AUG_PER_EPOCH T80_REAL_REPEAT
T80_FINETUNE T80_THREADS T80_WEIGHT_DECAY`

正式训练参数：`EPOCHS=60 LR=0.001 WD=0.01 AUG_PER_EPOCH=2000`。
产出 `models/train_items103/{best.pth, best_ema.pth, last.pth, classes103.json}`。

注意 `classes103.json` 其实是 `torch.save` 出来的 **list**，不是 JSON 文本；
`export_items103.py` / `eval_preproc.py` 里都按这个事实读。

## 4. INT8 量化导出

需要专用 conda 环境（espdl 工具链版本敏感，不要用系统 python）：

```bash
/home/oooa/miniforge3/envs/espdl/bin/python tools/items103/export_items103.py last.pth
```

- `espdl_quantize_torch`，`num_of_bits=8`，`target='esp32s3'`，
  `input_shape=[1,3,224,224]`，`QuantizationSettingFactory.espdl_setting()`（默认逐层对称量化）。
- **校准集 = `models/dataset_items103/val`，824 张 320x240 屏幕模拟增强图**，`calib_steps=824`。
- 输出直接落在 `espdl_files/items103.espdl`，同时生成 `.onnx/.json/.info` 中间物（已 gitignore）。

## 5. 端侧加载与推理

`components/items103/items103.cpp`：

```cpp
Model("items103",                 // 分区 label
      "items103.espdl",           // 打包模型内文件名
      MODEL_LOCATION_IN_FLASH_PARTITION,
      /*max_internal_size=*/0,    // 激活全放 PSRAM（内部 RAM 碎片化，最大块仅 ~2.7KB）
      MEMORY_MANAGER_GREEDY, nullptr,
      param_copy);                // 见下
model->minimize();
```

**`param_copy` 这个坑值得单独记**：原判断是「总 PSRAM < 9MB 就不拷权重」，
而 N16R8 只有 8MB，于是永远走「权重留在 mmap 的 flash 里、每层跨 MMU/cache 去取」的慢路径。
分层计时（`profile_module`）显示 `features.18` 那层 737KB 权重只跑出 2.2 MB/s，
整网 300 MMAC 花 4.6 秒（≈66 MMAC/s）——瓶颈在取权重，不在算。
改成按**空闲** PSRAM 判断（权重 2.3MB，留 1MB 余量）后，首次推理 4600ms → 1986ms。

置信度：对 103 个 logits 做 **温度 0.5** 的 top1 softmax（`exp((v-mx)*2)`），
所以屏上百分比比真实概率更「自信」，跨模型比较时要记得这一点。

内存画像（`profile_memory` 实测）：参数量化后 2277 KB，激活变量 1666 KB 全在 PSRAM。

## 6. 当前已知的精度问题

拿 12 张溯源可信的实拍帧做 A/B（`tools/items103/eval_preproc.py`）：

| 推理端 | 命中 |
|--------|------|
| PC 上 fp32（同一个 checkpoint） | **12/12**，均置信 95.9% |
| 设备上 INT8 | **9/12**（skateboard→台灯 96%、pig→耳机 44%、gloves→玻璃杯 34%） |

三张错的类在索引表里间距不一致（81→48、68→32、39→37），**不是类表位移**，
是量化本身把这几类推给了邻居。最可疑的原因：校准集是**干净合成图**，
和实拍眩光域对不上，激活量程被定歪。

对应的修法（按代价从小到大）：

1. 把实拍帧混进 espdl 导出的校准集，重导一次 INT8（不用重训）；
2. 走 `docs/selflearn.md` 的增量微调（`real_captures/` 会被自动混入 train）；
3. 换更激进的量化设置（逐通道 / 部分层保 fp16）。

同时已排除的假设：**软件预处理救反光不划算**。11 种变体
（去底 / 百分位拉伸 / 白平衡 / clahe / unsharp 及各种组合）在 fp32 上相对 `raw` **全部 +0**，
而干净合成图抽查里校正类还会掉 1 张。固件裁剪路径不要加这些。
