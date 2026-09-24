# ESP32-S3 端侧物体识别（摄像头实时推流 + AI 画框）

基于 **DFRobot FireBeetle 2 ESP32-S3**（DFR0975，N16R8：16MB Flash / 8MB Octal PSRAM）的
纯端侧通用物体识别设备：**OV3660 摄像头**采集画面，实时推流到板载 **2.0" 320x240 ST7789 TFT**，
由 **ESP-DL YOLO11n INT8** 模型在设备本地完成 COCO-80 类目标检测，识别框与中文标签直接叠加在视频上。

**不依赖云端、不依赖 WiFi** —— 模型推理全部在 ESP32-S3 上运行。

## 效果

- 全屏实时视频：QVGA(320x240) 画面连续刷新推流到 TFT
- 端侧检测：约每 5~6 秒一轮推理，检测到物体时画彩色边框 + 中文标签（如 `键盘 62%`）
- 底栏状态：`识别到 N 个物体 | 耗时 X.Xs`
- 支持 80 类 COCO 常见物体（人/杯子/椅子/电脑/键盘/瓶子/食物…），最多同屏展示 8 个目标
- 注：由于推理耗时，识别框相对视频有约 5 秒刷新间隔（端侧算力限制，属正常现象）

## 硬件

| 部件 | 型号 | 接口 |
|------|------|------|
| 主控板 | FireBeetle 2 ESP32-S3（V1.1，带 AXP313A PMIC） | — |
| 摄像头 | OV3660（DFRobot CAM 套件） | CAM FPC 座（8bit DVP + SCCB） |
| 显示屏 | 2.0" 320x240 IPS（ST7789） | GDI FPC 座（SPI） |
| 电源管理 | AXP313A | 共用 I2C（SDA=1 / SCL=2） |

### 引脚一览（均来自 DFRobot 官方 wiki，勿凭"常规 ESP32"经验改动）

**GDI 屏**：MOSI=15，SCK=17，CS=18，DC=3，RST=38，BL=21

**CAM DVP**：XCLK=45，PCLK=5，VSYNC=6，HREF=42，
数据 D0..D7（传感器 D2..D9）= 39, 40, 41, 4, 7, 8, 46, 48

**I2C**：SDA=1，SCL=2（AXP313A 与摄像头 SCCB 复用同一总线）

**摄像头供电**：AXP313A（I2C 地址 `0x36`）需先使能 ALDO=1.5V（DVDD）、DLDO=2.8V（AVDD/DOVDD），
OV3660 才有电 —— 顺序是 `shared_i2c_init → axp313a_begin → axp313a_set_camera_power → camera_init`。

## 软件架构

```
app_main
 ├─ 共用 I2C + AXP313A 摄像头上电
 ├─ TFT 初始化（ST7789 SPI 40MHz）
 ├─ OV3660 初始化（YUV422 / QVGA / 2 帧缓冲 @ PSRAM）
 ├─ view_task   (prio 5)   视频推流：采集 → YUV422→RGB888→大端RGB565 → 整屏刷屏 + 叠加识别框
 └─ detect_task (prio 3)   识别引擎：ESP-DL COCODetect(YOLO11n-320-int8) → 结果经互斥锁发布
```

- 两个任务共享摄像头帧缓冲（fb_count=2，取帧后尽快归还）；识别结果通过 `s_res_mux` 互斥锁
  以"最新值覆盖"方式传给绘制任务，视频流畅度不受推理阻塞。
- SPI 刷屏使用内部 RAM 的 4KB 中转缓冲（DMA 限制），PSRAM 存放帧转换暂存。

### 目录结构

```
main/main.cpp           应用入口：I2C/AXP/摄像头初始化、推流+识别双任务、COCO-80 中文标签表
components/
  axp313a/              AXP313A 电源驱动（legacy i2c，摄像头供电轨控制）
  tft_display/          ST7789 驱动 + 5x7 ASCII / 16x16 中文点阵（7549 字 GB2312 字库）
                        tft_render_view()：视频帧 + 检测框叠加绘制
  （以下出题机遗留组件，当前未编译进固件）
  wifi_manager/ ai_client/ quiz_store/ math_generator/
tools/gen_font_cn16.py  中文字库表生成脚本
partitions.csv          app 4MB + SPIFFS 4MB + coco_det 模型分区 4MB (@0x810000)
sdkconfig.defaults      目标/PSRAM/性能优化/模型选择等构建配置
```

## 编译与烧录

要求：ESP-IDF v5.3+（v5.3 下 esp-dl 需要的 `MALLOC_CAP_SIMD` 兼容处理已在顶层
CMakeLists.txt 中自动完成），16MB Flash / 8MB PSRAM 的 ESP32-S3 目标。

```bash
source /path/to/esp-idf/export.sh
cd ESP32-XM-dt
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor   # 会自动烧录 app + 分区表 + YOLO11n 模型分区
```

注意：`sdkconfig.defaults` 只在**首次**生成 sdkconfig 时生效；如已存在 sdkconfig，
可删除后重新 `idf.py build`，或手动核对以下关键项：

| 配置 | 值 | 原因 |
|------|----|------|
| CONFIG_COMPILER_OPTIMIZATION_PERF | y | 推理提速约 20% |
| CONFIG_ESP_TASK_WDT_TIMEOUT_S | 40 | 单帧推理约 6s，防看门狗复位 |
| CONFIG_ESP32S3_DATA_CACHE_64KB / LINE_64B | y | ESP-DL 推荐配置 |
| CONFIG_FLASH_COCO_DETECT_YOLO11N_320_S8_V1 | y | 只打包 320 输入模型（两个模型同时开会超出模型分区） |

## 关键参数（main/main.cpp）

- `SCORE_THR = 0.35f`：检测置信度阈值，调低召回更多、误检也更多
- `DET_MAX_OBJS = 8`：同屏最多绘制目标数
- 分辨率固定 QVGA 320x240，与屏幕 1:1 对齐，无需坐标缩放

## 已知限制

- COCO-80 类别中没有"毛巾"等类别；需要更多类别时可用 ESP-DL 训练管线换自训练模型
- 端到端推理约 5~6 秒/帧（S3 @240MHz，INT8 量化模型，输入 320x320）
- V1.1 板必须经 AXP313A 开电才有摄像头画面；V1.2+ 板无需此步

## 致谢

- [DFRobot Wiki](https://wiki.dfrobot.com.cn/_SKU_DFR0975_FireBeetle_2_Board_ESP32_S3_Advanced_Tutorial)（GDI/CAM 引脚表）
- [esp-dl](https://github.com/espressif/esp-dl) / [coco_detect](https://github.com/espressif/esp-dl)（ESP-NN 端侧推理）
- [esp32-camera](https://github.com/espressif/esp32-camera)
