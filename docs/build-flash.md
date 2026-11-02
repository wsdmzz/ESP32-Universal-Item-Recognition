# 构建与烧录环境

## 1. 工具链

| 项 | 值 | 备注 |
|----|----|------|
| ESP-IDF | **v5.3** | 本机在 `/home/oooa/AIQ/esp-idf`；v5.4+ 也可，见下面的兼容补丁说明 |
| 目标芯片 | `esp32s3` | |
| Python | IDF 自带的 `idf5.3_py3.13_env` | 由 `export.sh` 激活 |
| 串口 | `/dev/ttyACM0` | 板子 USB-C 直连 |
| 应用名 | `esp32_math_quiz` | 出题机时代留下的工程名，与现在的功能无关，别改（改了 `build/` 下产物名跟着变） |
| PC 侧脚本 | `python3`（3.13，含 `cv2`/`serial`/`torch`） | **不要在 `source export.sh` 之后的 shell 里跑采集脚本**——那个 shell 的 `python` 是 IDF 的 venv，没有 `cv2` |
| 量化导出 | `/home/oooa/miniforge3/envs/espdl/bin/python` | `tools/items103/export_items103.py` 要 `import esp_ppq`；这个包只在 espdl 环境和 `python3.11` 里有，默认 `python3`（3.13）没有 |

```bash
source /path/to/esp-idf/export.sh        # 每个新 shell 都要
cd ESP32-XM-dt
idf.py set-target esp32s3                # 仅首次（sdkconfig.defaults 里已含 IDF_TARGET）
idf.py build
```

### 1.1 依赖组件

`main/idf_component.yml` 声明三个直接依赖，`dependencies.lock`（已入库）锁定全部传递依赖，
所以 `idf.py build` 首次会自动下载并复现同一版本：

```
espressif/esp32-camera   -> 2.1.x
espressif/coco_detect    -> 0.4.0   ┐ 带进 espressif/esp-dl 3.3.11
idf >= 5.3.0                        ┘ + dl_fft 0.7.0, esp_jpeg, esp_new_jpeg
```

**esp-dl 是被 coco_detect 传递带进来的**，本项目的 `components/items103` 直接 `REQUIRES esp-dl`。
不要为了「去掉不用的 coco_detect」把它从 `idf_component.yml` 里删掉——原因见 §2.3。

### 1.2 顶层 CMakeLists 里的 IDF 5.3 兼容补丁

esp-dl 3.3.x 在 `dl_image_process.cpp` 里无条件用了 IDF ≥5.4 才有的 `MALLOC_CAP_SIMD`。
5.3 上把它定义为 0（等价于普通分配），仅为编译兼容：

```cmake
if(NOT IDF_VERSION_MAJOR GREATER_EQUAL 5 OR IDF_VERSION_MINOR LESS 4)
    add_compile_definitions(MALLOC_CAP_SIMD=0)
endif()
```

## 2. 构建配置（sdkconfig）

### 2.1 哪些在仓库里、哪些不在

- `sdkconfig.defaults` —— **入库**。可复用的非默认项 + 每一项的理由。
- `sdkconfig` —— **不入库**（`.gitignore`），因为里面有 `CONFIG_WIFI_SSID`、
  `CONFIG_WIFI_PASSWORD`、`CONFIG_QWEN_API_KEY`。
- 首次构建时 `sdkconfig` 由 `sdkconfig.defaults` 生成；**之后 defaults 的改动不会自动套用**。
  要重来：`rm sdkconfig && idf.py build`。
- 想把当前实机配置回写成 defaults：`idf.py save-defconfig`，
  然后**务必手工删掉凭据那三行再提交**（该命令会把它们一起写进去）。

### 2.2 关键项与理由

| 配置 | 值 | 为什么 |
|------|----|--------|
| `COMPILER_OPTIMIZATION_PERF` | y | 推理提速约 20% |
| `ESP32S3_DATA_CACHE_64KB` / `8WAYS` / `LINE_64B` | y | esp-dl 推荐；PSRAM 缓存命中直接决定推理耗时 |
| `SPIRAM_MODE_OCT` / `SPIRAM_SPEED_80M` | y | N16R8 的 8MB 八线 PSRAM，不开就是四线半速 |
| `SPIRAM_MALLOC_ALWAYSINTERNAL` | 4096 | >4KB 的分配允许进 PSRAM（权重拷贝、帧缓冲） |
| `ESP_TASK_WDT_TIMEOUT_S` | **40** | 单轮推理 4~20 秒，默认 5 秒必复位 |
| `ESP_MAIN_TASK_STACK_SIZE` | 8192 | 模型加载在 main 里做 |
| `PARTITION_TABLE_CUSTOM_FILENAME` | `partitions.csv` | 见 §3 |
| `FREERTOS_HZ` | 1000 | 1ms 节拍，测光和换图任务的延时才准 |

### 2.3 coco_detect 的模型开关不能全关

`coco_detect` 的 Kconfig 里 `COCO_DETECT_YOLO11N_*` 依赖 `FLASH_COCO_DETECT_*`（或 SD 卡模式），
两个 `FLASH_*` 都不选会让它的打包步骤报
`pack_espdl_models.py: error: argument -m/--model_path: expected at least one argument`，
**构建直接失败**。所以 `sdkconfig.defaults` 里显式保留一个（320 输入那版），
刷进 `partitions.csv` 预留的 `coco_det` 分区。固件运行时不碰它。

## 3. 分区表（`partitions.csv`，16MB Flash）

```
nvs,      data, nvs,     0x9000,    0x6000,
phy_init, data, phy,     0xf000,    0x1000,
factory,  app,  factory, 0x10000,   0x400000,   # 4MB，当前 app 3.0MB / 剩 25%
items103, data, 0x30,    0x410000,  0x400000,   # INT8 模型 2.6MB
coco_det, data, 0x30,    0x810000,  0x400000,   # coco_detect 占位，不使用
storage,  data, spiffs,  0xc10000,  0x3F0000,   # 出题图库 3.94MB
```

`storage` 装 103 张 480x320 出题图（源文件合计 1,800,172 B）。运行时
`esp_spiffs_info` 报 **3,787,841 字节 / 已用 1,840,081 = 48.6%**（串口上电日志里
就有这行），还能再塞约 120 张同规格的。

## 4. 烧录

### 4.1 全量（首次、或改过分区/模型/图库）

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

实际写盘由 `build/flash_args` 决定，六段一起：

```
--flash_mode dio --flash_freq 80m --flash_size 16MB
0x0      bootloader/bootloader.bin
0x8000   partition_table/partition-table.bin
0x10000  esp32_math_quiz.bin
0x410000 espdl_files/items103.espdl            <- 本项目在用的模型
0x810000 espdl_models/coco_detect.espdl        <- 传递依赖的占位，见 §2.3
0xc10000 storage.bin                           <- 103 张出题图库
```

### 4.2 只换 app（日常，约 20 秒，分区/模型/图库都不动）

```bash
python -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 \
    --before default_reset --after hard_reset \
    write_flash 0x10000 build/esp32_math_quiz.bin
```

只改图库时：`python -m esptool -p /dev/ttyACM0 -b 460800 write_flash 0xc10000 build/storage.bin`
（图库源文件在 `spiffs_data/photos/`，镜像由 `main/CMakeLists.txt` 的
`spiffs_create_partition_image(storage ../spiffs_data FLASH_IN_PROJECT)` 生成）。

### 4.3 ★ 重刷之后必须重下的两条命令

**所有 `/cam` 与 `/quiz` 的在线设置都是 RAM 变量，重启即回到编译期默认值**：
点测光关闭、裁剪关闭、换图间隔回到 120 秒。跑任何测量/采集脚本前要先恢复：

```bash
curl "http://<ip>/cam?spot=1&crop=1&adetect=1&target=200"   # 端侧采集配置
curl "http://<ip>/quiz?every=3600"                          # 采集期间冻结自动换图
```

`tools/items103/cap_collect.py` 已经自带这两步，手跑脚本时别漏。

### 4.4 看串口

```bash
idf.py -p /dev/ttyACM0 monitor
python3 tools/devlog.py /tmp/devlog.txt 1800     # 落文件，便于事后 grep（端口写死 ACM0）
```

**注意：`open('/dev/ttyACM0')` 本身会让 S3 复位**（DTR/RTS 默认拉一下）。
所以要先起日志、再测东西，不要在一轮测量中间去开串口。

## 5. 一键验证「环境能复刻」

不污染本机 `sdkconfig` 与 `build/` 的前提下，用一份全新的配置编一遍：

```bash
idf.py -B /tmp/fresh_build -DSDKCONFIG=/tmp/fresh_sdkconfig \
       -DSDKCONFIG_DEFAULTS=sdkconfig.defaults build
```

跑通即说明：克隆仓库 + 装好 IDF v5.3 就能直接构建，不需要任何本机私有的 sdkconfig。
