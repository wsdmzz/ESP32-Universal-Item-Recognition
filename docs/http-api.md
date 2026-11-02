# 端侧调试接口（HTTP）

固件里跑着一个 `httpd`（`main/main.cpp` 的 `frame_http_start()`），
WiFi 连不上时不启动（只打日志，不阻塞识别）。设备 IP 会显示在 2" 屏上，
本机目前固定在 `http://192.168.8.194`。

设计意图：**所有调参都走 HTTP，改完不用重烧**。8 个端点：

| 端点 | 方法 | 用途 |
|------|------|------|
| `/` | GET | 内置观察页（实时画面 + 当前判定） |
| `/frame.rgb` | GET | 模型真实输入那一帧，裸 RGB888 |
| `/capture.jpg` | GET | 同一帧的 JPEG，给人眼看 |
| `/result.json` | GET | 当前判定 + **出处 `photo`** |
| `/big` | GET | 4" 屏：自检图案 + 在线改引脚/频率/方向 |
| `/show` | POST | 把 body 里的 JPEG 直接上 4" 屏 |
| `/quiz` | GET | 出题图库：换图 / 改间隔 / 列名单 |
| `/cam` | GET | OV3660 全量寄存器级调参 + 找屏 + 点测光 + 裁剪 |

## ★ 先记住：重启即失效

`/cam` 与 `/quiz` 改的全是 RAM 里的变量，**重刷或复位后回到编译期默认**
（点测光关、裁剪关、换图间隔 120 秒）。跑测量前先恢复：

```bash
curl "http://<ip>/cam?spot=1&crop=1&adetect=1&target=200"
curl "http://<ip>/quiz?every=3600"      # 采集期间冻结自动换图
```

## `/result.json`

```json
{"ok":true,"seq":45,"width":128,"height":84,"percent":99,
 "label":"西瓜","photo":"watermelon"}
```

| 字段 | 含义 |
|------|------|
| `seq` | 推理轮次计数，每落地一轮 +1 |
| `width/height` | 喂给模型的那一帧尺寸。`< 300` 说明裁剪真的生效了 |
| `label` / `percent` | 端侧中文判定与置信度（温度 0.5 的 softmax，比真实概率偏自信） |
| **`photo`** | **这一帧过推理时屏上显示的是哪张出题图**（英文键，不带 .jpg） |

`photo` 是自学习闭环的地基：它由固件在 `esp_camera_fb_get()` **拿到帧的那一刻**
快照 `quiz_photos_last()`，随帧一起进临界区。早先的版本在推理结束时才问屏上是哪张，
而推理要 4~20 秒，问到的常是中途被换上去的新图 —— 标签整批错位一格。
细节与证据见 `docs/selflearn.md` §2。

## `/frame.rgb` 与 `/capture.jpg`

两者是同一帧，`/frame.rgb` 是裸 `w*h*3` 字节流，元信息在响应头：

```
X-Frame-Width / X-Frame-Height / X-Frame-Seq / X-Frame-Pct / X-Frame-Label(URL 编码)
```

`/capture.jpg` 给人眼看。**注意一个已修的坑**：`esp_jpeg` 编码器会把传入字节按
B,G,R 解释，直接喂 RGB888 会得到 R/B 互换的图（浏览器里樱桃变蓝），
所以 handler 里先换序再编码 —— 这只影响预览，不影响 `/frame.rgb` 和模型输入。

单帧 128x84x3 = 32KB；裁剪关掉时是 320x240x3 = 230KB，
`cfg.send_wait_timeout` 为此放宽到 20 秒。

## `/cam` —— 摄像头与找屏

### 传感器寄存器级（值直接透传给 `sensor_t` 的 setter）

`bri con sat sharp den eff wb awb awbg aec aec2 ae aecv agc agcg ceil lenc bpc wpc q`

- `aec=0` + `aecv=<曝光行数>` = 手动曝光；`agc=0` + `agcg=<0..30>` = 手动增益；
  `ceil=<0..6>` = 增益上限 2X..128X。
- `ae=-2..2` 是 AE 电平，往负走 = 整体压暗。
- 任一 setter 返回非 0 会汇总进响应的 `"failed":"bri!aec2!..."`。

### 本项目自己加的三件事

| 参数 | 含义 |
|------|------|
| `spot=0/1` | 点测光开关。关掉时把 AEC/AGC 还给传感器 |
| `target=32..250` | 把屏幕区亮度的 p90 控到这个值 = 「图里最亮的部分刚好不削顶」 |
| `adetect=0/1` | 自动找屏框 |
| `roi=x0,y0,x1,y1` | 手动钉死屏幕框（百分比），同时关掉自动找屏 |
| `crop=0/1` | 1 = 只把屏幕区喂给模型 |
| `r=3503,3500,350a` | 直接读传感器寄存器（十六进制逗号分隔），验证手动曝光真的落下去了 |

为什么要这些：OV3660 的 AEC 按**整帧平均**测光，而屏只占画面一小块，
桌面曝光刚好的时候屏早就全白削顶了，出题图的浅色物体/白底细节全被抹平。
另外屏只占 128x84 时模型拿整帧去分类，看到的其实是「桌子」而不是出题图
（实测：屏上是玉米，模型 82% 认成台灯）——所以要裁剪。

响应里 `"spot":"on=1 adetect=1 crop=1 target=200 aecv=... panel=15,40,55,75 ok=1"`
是回声，用来确认设置真的生效、以及当前屏框在哪。

### 找屏的实现要点

`panel_detect()` 找亮点框，再做像素级边缘细化 `panel_refine()`；
结果要**连续 3 次一致**（`kAgreeNeed=3`，四边差 ≤3 个百分点 `kAgreeTol`）才被采纳，
采纳后锁定 30 帧（`kHoldRuns`）不再改框。这是为了防止某帧反光/换图把框带跑。

## `/quiz`

| 用法 | 效果 |
|------|------|
| `GET /quiz` | 什么都不做，返回当前状态（`"info":"idle"`） |
| `GET /quiz?list=1` | 列出图库全部文件名（4KB 缓冲，够 103 张） |
| `GET /quiz?n=apple` | 立刻把 `apple` 送上屏（钉题） |
| `GET /quiz?next=1` | 随机换下一张 |
| `GET /quiz?every=10` | 改换图间隔（秒，2..3600），立刻生效 |

响应：`{"ok":true,"count":103,"interval":120,"last":"watermelon","info":"...","cfg":"...","diag":"..."}`

图库来自 SPIFFS `/spiffs/photos/*.jpg`（`main/quiz_photos.cpp`），
镜像由 `main/CMakeLists.txt` 的 `spiffs_create_partition_image` 从 `spiffs_data/photos/` 生成。

有个细节：`/quiz` 成功出题后会把 `/big` 的「上次图案」记成 `none`，
免得后面随手一个 `/big` 把出题图盖成自检彩条。

## `/show` —— POST 一张 JPEG 上屏

```bash
curl --data-binary @photo.jpg "http://<ip>/show"
tools/show_photo.sh photo.jpg         # 封装好的脚本
```

限制 `content_len ≤ 1MB`（超了 413，PSRAM 不够 507）。板子上用 ROM 里的
TJpgDec 软解，等比缩放居中、黑边补齐。PNG/BMP 先 `convert a.png -quality 90 a.jpg`。

## `/big` —— 4" 屏在线调参

引脚可在线改（改完 `c=` 重画即可看效果，不用重烧）：

```
?mosi=&sck=&cs=&dc=&rst=&bl_pin=&miso=&hz=&rot=&bl=&map=&mode=&rstpol=&madctl=
```

自检图案 `c=`：`bars red green blue black white checker blit axis probe`。
`none` 是特殊值，但**不是「这一次不重画」** —— 它会把「上次图案」记成 `none`，
而 `none` 不匹配任何图案、落到默认分支，所以 `GET /big?c=none` **当场仍会画一张探针图**。
正确的用法是先单独发一次 `c=none`，**之后**不带 `c` 的 `/big` 请求（改 hz/map/madctl…）
就不重画了，等参数调好再 `c=bars` 看结果：

```bash
curl "$IP/big?c=none"          # 设成 none（这一次会画探针图，忽略它）
curl "$IP/big?hz=8000000"      # 只改频率，不重画
curl "$IP/big?c=blit"          # 要看了，再重画
```

| 用法 | 返回 |
|------|------|
| `c=gpio` | 逐脚上拉/下拉探针结果 —— 判断共享 MISO 有没有被别的芯片按住 |
| `c=id` | `RDDID`/`RDDST`。**这个模组恒返回 0x00000000，不能当链路判据** |
| `c=blit` | 整屏刷屏，配合响应里的耗时测 SPI 频率上限 |

响应回显全部当前配置：`{"ok":true,"pattern":"bars","cfg":"...","diag":"..."}`。

> 当前 `hz` 仍是 **1 MHz**（保守值，blit 2487ms；8 MHz 时约 343ms）。
> 顶到多少会坏，可以用摄像头拍屏看有没有「整段行错位」来判，不用人眼盯。
> 这是仓库里还欠着的一件事。

## 典型排查流程

```bash
IP=http://192.168.8.194
curl "$IP/cam?spot=1&crop=1&adetect=1&target=200"   # 恢复采集配置
curl "$IP/result.json"                               # width 应 < 300
curl -o /tmp/cap.jpg "$IP/capture.jpg"               # 取模型看到的那一帧
python3 tools/camcheck.py                            # 屏在哪 / 削不削顶 / 糊不糊
python3 tools/items103/cap_sheet.py                  # 实拍帧 vs 题库原图 对照表
python3 tools/devlog.py /tmp/devlog.txt 600 &        # 同时抄串口
curl "$IP/quiz?n=watermelon"                         # 钉一张题
```

串口日志里 `items103: top1=西瓜 (99.1%) 4485ms [预处理7 推理4478]` 与
`CAMDET: classify: 西瓜 (watermelon) 99% 4488.2ms 输入 128x84(裁剪)` 是配对的，
后者带裁剪尺寸，用来确认喂进去的到底是多大一张图。
