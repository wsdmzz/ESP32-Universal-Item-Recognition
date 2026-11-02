# ESP32-S3 端侧物品识别 + 自学习闭环

基于 **DFRobot FireBeetle 2 ESP32-S3**（DFR0975 V1.1，N16R8：16MB Flash / 8MB Octal PSRAM）
的纯端侧常见物品识别设备。**推理全部在 ESP32-S3 上完成，不依赖云端。**

WiFi 只用来做一件事：让 PC 能拉走「模型看到的那一帧」做诊断和采集。拔了网线设备照常识别。

## 这台机器在干什么

```
OV3660 拍桌面上的 4 寸屏
   │  自动找屏框（连续 3 帧一致才采纳，采纳后锁 30 帧）+ 像素级边缘细化
   │  点测光：把屏幕区亮度 p90 钉在 target，避免自发光屏幕削顶
   │  裁剪：只把屏幕区（实测 128x84）喂给模型，而不是整张桌面
   ▼
items103 —— MobileNetV2 / 103 类常见物品 / INT8 espdl，2.6MB，跑在 flash 分区
   ▼
2.0" ST7789 实时推流 + 中文标签（如 `西瓜 99%`）      4.0" ST7796S 每 2 分钟随机出一张题
```

两块屏各司其职：2 寸是**答案板**（看识别结果），4 寸是**出题板**（放题目图）。
把实拍帧按真值存下来、增量微调、重导 INT8、重烧 —— 这就是自学习闭环
（`docs/selflearn.md`）。

## 当前实测指标

| 项 | 值 |
|----|----|
| 类别数 | 103（`models/items103.yaml`，中文显示名见 `items103_classes.hpp`） |
| 模型 | MobileNetV2 INT8 espdl，2,652,848 B；参数 2277KB + 激活 1666KB 全在 PSRAM |
| 输入 | 224x224，端侧把裁剪帧**非等比拉伸**（几何链路见 `docs/model-pipeline.md` §2） |
| 推理耗时 | 首次 **1986 ms**（`param_copy` 修复后）；稳态 4.3~5.6 s，偶发 13~19 s 尖峰（未解，见下） |
| 实拍识别率 | 12 张溯源可信的帧：端侧 INT8 **9/12**，同一批 PC 上 fp32 **12/12** |
| 屏幕反光 | 黑底 75 / 白顶 215，对比度只剩 **2.7:1**，蓝通道近削顶 |
| 图库占用 | 103 张 480x320 = 1,840,081 / 3,787,841 B（SPIFFS 用了 48.6%） |
| app 体积 | 3.0 MB / 4 MB 分区（剩 25%） |

## 文档地图

| 想知道 | 看哪篇 |
|--------|--------|
| 怎么接线（含「SD_CS 悬空会弄死整块屏」这类致命坑） | [docs/hardware-wiring.md](docs/hardware-wiring.md) |
| IDF 版本、sdkconfig、分区表、全量/增量烧录 | [docs/build-flash.md](docs/build-flash.md) |
| 模型从 AI 生图到 INT8 的完整链路、训练与量化 | [docs/model-pipeline.md](docs/model-pipeline.md) |
| 自学习闭环：采帧、标签可信、微调、反光改善清单 | [docs/selflearn.md](docs/selflearn.md) |
| 8 个 HTTP 调试端点与典型排查流程 | [docs/http-api.md](docs/http-api.md) |

## 三十分钟跑起来

```bash
# 0) 按 docs/hardware-wiring.md 接线（AXP313A 供电、SD_CS 接 3V3 是两处必查点）
source /path/to/esp-idf/export.sh          # ESP-IDF v5.3
cd ESP32-XM-dt

# 1) 配 WiFi（凭据不入库，只写进本机 sdkconfig）
idf.py menuconfig        # -> "Quiz AI 配置" -> CONFIG_WIFI_SSID / CONFIG_WIFI_PASSWORD

# 2) 编译 + 全量烧录（app + 分区表 + items103.espdl + SPIFFS 图库）
idf.py set-target esp32s3
idf.py -p /dev/ttyACM0 flash monitor
```

上电后 2 寸屏会显示设备 IP。看到 `图库就绪：103 张` 和 `Items103 model loaded`
就说明起来了。之后：

```bash
IP=http://<那个 IP>
curl "$IP/cam?spot=1&crop=1&adetect=1&target=200"   # ★ 重刷后必做，设置不持久
curl "$IP/result.json"
python3 tools/items103/label_server.py              # 标注台 http://localhost:8765
```

PC 侧脚本的解释器分工（本机现状，别混用）：需要 `cv2` 或 `serial` 的
（`cap_collect` / `cap_sheet` / `camcheck` / `camstat` / `camsweep` / `camsharp` /
`eval_preproc` / `devlog`）**只能用 `python3`**（3.13，这两个包装在它里面；
3.11 环境没装）；`label_server.py` 只依赖 yaml，两个都能跑；
`tools/items103/selflearn.sh` 内部写死了训练用 `python3.11`、量化导出用
`/home/oooa/miniforge3/envs/espdl/bin/python`。

## 目录结构

```
main/
  main.cpp             摄像头/双屏/模型/WiFi 初始化，推流+识别双任务，找屏+点测光+裁剪，HTTP 端点
  quiz_photos.cpp      SPIFFS 图库扫描 + 定时随机出题任务
components/
  items103/            当前在用的分类模型封装（esp-dl Model + ImagePreprocessor）
  tft_display/         tft_display.c：2.0" ST7789(SPI2) + 中文字库；tft_big.c：4.0" ST7796S(SPI3)
  axp313a/             摄像头供电轨控制（V1.1 板必需）
  wifi_manager/        WiFi 连接
  items127/ ai_client/ quiz_store/ math_generator/   ← 历史遗留，见下
tools/
  items103/            增强→训练→量化→采集→评估→自学习 全链路脚本
  camstat/camcheck/camsweep/camsharp.py  摄像头曝光与清晰度诊断
  devlog.py            串口日志落文件
espdl_files/items103.espdl   设备正在跑的 INT8 模型（入库）
spiffs_data/photos/          103 张出题原图 480x320（入库）
augmented/<类>/              2060 张屏幕模拟增强图（入库，生成脚本未留存，勿删）
models/train_items103/       best.pth / last.pth / classes103.json（入库，可离线重导量化）
partitions.csv               app 4MB + items103 4MB + coco_det 4MB + SPIFFS 3.94MB
sdkconfig.defaults           可复用的非默认构建配置（凭据不在里面）
```

### 已停用的遗留组件

`items127`（上一代 127 类模型）、`fruit19`、以及出题机时代的
`ai_client` / `quiz_store` 都**不参与识别链路**，保留只为历史可查。
`math_generator` 仍被 `tft_display` 依赖（点阵字库的构建顺序），别顺手删。
当前固件的模型组件只有 `items103`（见 `main/CMakeLists.txt` 的 `REQUIRES`）。

## 已知限制与待办

- **推理耗时随运行时间漂移**：首次 2 s，跑久后稳态 4.3~5.6 s、偶发 13~19 s 尖峰，
  预处理也从 7ms 偶发到 300/429ms —— 同一个 PSRAM/cache 争用的特征，但还没定位到具体来源。
- **INT8 是当前精度瓶颈，不是反光**：同一批实拍帧 fp32 全对、INT8 错 3 张。
  最可疑的是量化校准集用的是干净合成图，与实拍眩光域对不上。
  下一步：把实拍帧混进校准集重导一次 INT8（不用重训）。
- **软件反光校正已验证不划算**：11 种预处理变体相对 `raw` 全部 +0，
  所以固件裁剪路径**不要**加去底/拉伸/白平衡。改善要靠物理手段（挪近、关灯、倾屏）。
- **4 寸屏 SPI 时钟还烧在 1 MHz**（blit 2487ms，8 MHz 时约 343ms）。
  杜邦线能顶到多少没测完；现在可以用摄像头拍屏看有没有整段行错位来判，不用人眼盯。
- **背光不可调**：4 寸屏 LED 现在硬接 3V3。改到 IO12 后可以做 LEDC PWM 调光，
  从源头压眩光（`GET /big?bl_pin=12` 可先在线验证）。
- 内部 RAM 跑满后最大连续块只剩约 2.7 KB，所以新任务必须在 `app_main` 早期创建、
  读 flash 的任务栈不能放 PSRAM —— 这是 `quiz_task` 起不来那次的根因。

## 致谢

- [DFRobot FireBeetle 2 ESP32-S3 Wiki](https://wiki.dfrobot.com.cn/_SKU_DFR0975_FireBeetle_2_Board_ESP32_S3_Advanced_Tutorial)（GDI/CAM 引脚表）
- [esp-dl](https://github.com/espressif/esp-dl)（INT8 espdl 推理与量化导出）
- [esp32-camera](https://github.com/espressif/esp32-camera) / [LCDWiki 4.0" ST7796S](https://www.lcdwiki.com/4.0inch_SPI_Module_ST7796S_KD)
