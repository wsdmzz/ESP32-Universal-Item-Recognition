# 自学习闭环：让端侧在自己拍到的画面上收敛

模型是在「AI 生图 + 合成拍屏增强」上训出来的，但它上线后要认的是
**真实摄像头拍真实屏幕**的画面。这两个域不一样，闭环就是把实拍帧变成训练样本：

```
4" 屏出题  ──▶  OV3660 拍屏  ──▶  找屏+点测光+裁剪  ──▶  items103 INT8  ──▶  2" 屏出标签
                                                                      │
        人确认/改判 ◀── 标注台 http://localhost:8765 ◀──────────────────┘
                │
                ▼
   models/items103/real_captures/<真值类>/  ──▶  增量微调  ──▶  重导 INT8  ──▶  重烧
```

固件侧不需要任何改动，`train_items103.py` 会自动把 `real_captures/` 混进 train
（每张复制 `T80_REAL_REPEAT=3` 份）。

## 0. 每次重刷固件后先做这一步

**所有 `/cam`、`/quiz` 的在线设置都是 RAM 变量，重启即回到编译期默认。**
不恢复的话采到的帧是「整张桌面 + 屏幕削顶」，模型必然瞎猜：

```bash
curl "http://<ip>/cam?spot=1&crop=1&adetect=1&target=200"
curl "http://<ip>/result.json"          # width 应 < 300，说明裁剪真的生效了
```

`crop` 生效前 `width` 会是 320；实测稳定后是 **128x84**（屏幕框 15,40,55,75 百分比，
宽高比 1.524，对上 4" 屏的 1.5）。

## 1. 采集：两条路

### 1.1 交互标注台（日常用这个）

```bash
python3 tools/items103/label_server.py        # PC 上跑
# 浏览器打开 http://localhost:8765，页面里填设备 IP
```

左侧是实时画面（`/capture.jpg`）与端侧当前判定（`/result.json`）；
点「抓取当前帧」冻结一帧（`/frame.rgb`，模型真实输入）；认对了点「确认」，
认错了下拉选真值再点「改判保存」→ 落 `models/items103/real_captures/<英文类>/`。

### 1.2 脚本批量采集（要一批干净评测集时用）

```bash
python3 -u tools/items103/cap_collect.py \
    --photos skateboard,pig,watermelon,lamp,banana,carrot,cat,tomato,scissors,ball,broccoli,gloves \
    --out models/items103/cap_test
# 或者随机抽 16 张： --n 16
```

它会自动做 §0 的设置、逐张钉题、**等端侧自己交代出处**（`/result.json` 的
`photo == 出题名`）才收帧、单张等不到就重试 `--tries` 次，
最后把 `/quiz?every=` 恢复成 120 秒。默认每张只采 1 帧，想顺带看抖动模糊就加
`--reps 2`。参数：`--host --photos --n --out --reps --tries`。

## 2. ★ 标签可信是这个闭环的生死线（三个坑，都踩过）

**坑一：出题和推理是两条时间线。**
`GET /quiz?n=X` 只是把 X 送上屏，而那一轮推理可能早就起飞了、吃的还是上一张。
按「我出了什么题」命名采到的帧，会**整批错位一格**。
实测一批 14 张里只有 1 张对得上，而且错位的特征非常隐蔽——预测结果恰好互相指认成
集合里的另一张（`lamp` 帧被判成西瓜 100%、`banana` 被判成台灯 95%、`carrot` 被判成香蕉 99%…），
看起来像「反光把模型彻底打坏了」，其实纯粹是标签脏。

> 修法：让板子自己交代。固件在 `esp_camera_fb_get()` **拿到帧的那一刻**
> 快照 `quiz_photos_last()`，随帧一起存进 `s_frame_photo`，`/result.json` 以
> `photo` 字段吐出来。采集脚本等 `photo == 目标名` 才收帧。
> 注意快照点必须在取帧时，**不能**在推理结束时——推理要 4~20 秒，
> 晚问一句拿到的会是中途被换上去的新图。

**坑二：别想用像素相关性代替出处。**
反光给画面抬了一层很亮的底，两张**完全不同**的出题图原始像素相关能有 **0.92**；
换用高通特征又只剩 0.078（基本是噪声）。两条路都判不出「屏换没换」。
`tools/items103/capmatch.py` 就是照这个思路写的，实测把 4 张人工确认过的帧全认错，
**已放弃**，只留作反面教材。

**坑三：两个采集进程同时跑会互相改屏。**
本次就是这么坏的一批。`cap_collect.py` 现在加了 `~/.cap_collect.lock` 文件锁，
同一时刻只允许一个采集者。

## 3. 采完之后先验标签，再谈精度

```bash
python3 tools/items103/cap_sheet.py            # 左=实拍帧 右=题库原图，肉眼核对构图
python3 -u tools/items103/eval_preproc.py --detail
```

`eval_preproc.py` 做三件事：

1. 用三个 checkpoint 各跑一遍原始帧，和板子记录的 INT8 判决对答案，
   **agreement 最高的那个才是当前 espdl 的源头**（不然比了也白比）；
2. 在这个 checkpoint 上 A/B 11 种预处理变体的 top-1 命中率；
3. 拿合成 val 集抽查 120 张——校正若把干净图也拧坏了，自学习会学歪。

### 已得到的结论（12 张溯源可信的实拍帧）

| 推理端 | 命中 |
|--------|------|
| PC fp32 | 12/12，均置信 95.9% |
| 设备 INT8 | 9/12 |

预处理变体 `raw / stretch / stretch_g / gw / wp / clahe / unsharp / wp+stretch /
wp+stretch_g / wp+clahe / stretch+sharp` 相对 `raw` **全部 +0**。

**所以：反光校正不要写进固件裁剪路径**（白花算力、还可能把干净图带歪）。
剩下的 3 张错是 INT8 量化问题，见 `docs/model-pipeline.md` §6。

## 4. 反光的实测数字与物理改善清单

12 张实拍帧（128x84 裁剪后）统计：

| 指标 | 值 |
|------|----|
| 黑底（p2） | 75–78 / 255 |
| 白顶（p98） | 197–218 / 255 |
| 对比度 | **2.7 : 1**（应有 50:1 以上） |
| 蓝通道 p98 | 219–249，接近削顶 |

一层约 75 级的加性眩光底抬起黑场，白场又被顶在 215 出不去，整个画面被压进 140 级窄带。
越亮的出题图受害越重：`skateboard`（原图均值 213.7）和 `pig`（均值 202.8）
正是端侧认错的两张——它们的结构落在 128–233 这段，几乎全被压在眩光窗口之上。

物理改善（按收益排序）：

1. **摄像头挪近到 10–12 cm，让屏幕铺满画面。** 现在屏只占 128x84，
   而模型要 224x224，等于靠插值硬放大约 2.6 倍。这条收益最大。
2. **关掉或调暖那盏冷白台灯。** 它就是 75 级眩光底和偏蓝的来源。
3. 屏幕倾 10–15°、给镜头加遮光罩。
4. 画面左下那条斜的粉色亮带是物理反光/划痕，软件去不掉。

另外，把 4" 屏的 LED 从 3V3 改到 IO12 做 PWM 调光（见
`docs/hardware-wiring.md` §4）能从源头压低眩光，值得一并动手。

## 5. 微调 + 量化 + 烧录（一条命令）

```bash
bash tools/items103/selflearn.sh
# 调参： EPOCHS=12 LR=0.0005 bash tools/items103/selflearn.sh
```

三步依次是：

1. `train_items103.py`，从 `last.pth` 继续，默认 8 epochs / lr 2e-4 / wd 1e-3；
2. `export_items103.py last.pth`（走 `/home/oooa/miniforge3/envs/espdl/bin/python`）；
3. `idf.py -p /dev/ttyACM0 flash`。

然后回标注台继续确认/改判。同一屏幕再认不对就再攒几帧跑一次，逐轮收敛。

## 6. 诊断工具箱

解释器别混用：下表除 `selflearn.sh` 外都用 **`python3`**（3.13，装了 `cv2`/`serial`；
`python3.11` 环境**没有**这两个包，跑 `cap_*`/`cam*`/`devlog` 会 ImportError）。
`label_server.py` 只依赖 yaml，两个都行（脚本 shebang 写的 `python3.11` 是历史遗留）。

| 脚本 | 干什么 |
|------|--------|
| `tools/devlog.py <文件> <秒>` | 串口日志落文件。**打开 ACM0 会让 S3 复位**，别在采集中间启动 |
| `tools/camstat.py [--grid] [--out f.jpg]` | 量曝光：整体/中心区统计，8x6 分块亮度表定位屏幕 |
| `tools/camcheck.py` | 一帧看三件事：屏幕位置与占比、削顶比例、Laplacian 清晰度 |
| `tools/camsweep.py` | 扫 OV3660 曝光/增益组合，用数字挑「屏幕不削顶」那一档 |
| `tools/camsharp.py` | 在固定屏幕区上比曝光×增益的清晰度，找抖动模糊下限 |
| `tools/show_photo.sh <jpg> [ip]` | 把一张图推到 4" 出题屏（板子上 TJpgDec 软解） |
| `tools/items103/cap_collect.py` | 按出处可信地批量采帧 |
| `tools/items103/cap_sheet.py` | 实拍帧 vs 题库原图 对照表 |
| `tools/items103/eval_preproc.py` | 预处理 A/B + checkpoint 认领 |
| `tools/items103/capmatch.py` | **已放弃**（像素相关判不出出处，见 §2 坑二） |
