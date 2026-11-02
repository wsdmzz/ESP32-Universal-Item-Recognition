# 硬件与接线

> 这篇是「照着就能把机器装起来」的清单。所有引脚值都取自 DFRobot 官方 wiki 与
> 实机验证结果，**不要按普通 ESP32 开发板的习惯改**。

## 1. 部件清单

| 角色 | 型号 | 规格 | 接口 |
|------|------|------|------|
| 主控板 | DFRobot FireBeetle 2 ESP32-S3（DFR0975，**V1.1**） | N16R8：16MB Flash + 8MB Octal PSRAM | — |
| 摄像头 | OV3660（DFRobot CAM 套件） | 200 万像素，这里跑 QVGA 320x240 | CAM FPC 座：8bit DVP + SCCB |
| 推流屏 | 2.0" IPS 320x240（ST7789） | 显示实时画面 + 识别标签 | GDI FPC 座（SPI2） |
| 出题屏 | LCDWiki 4.0" 480x320（ST7796S） | 自学习闭环的「题目」显示在这块 | 杜邦线到排针（SPI3） |
| 电源管理 | AXP313A（板载） | 摄像头供电轨 | I2C `0x36` |

## 2. 引脚总表

### 2.1 共用 I2C

```
SDA = IO1     SCL = IO2
```

AXP313A 与 OV3660 的 SCCB 复用这条总线。

### 2.2 摄像头 OV3660（CAM FPC 座，8bit DVP）

| 信号 | GPIO | 备注 |
|------|------|------|
| XCLK | 45 | 20 MHz |
| PCLK | 5 | |
| VSYNC | 6 | |
| HREF | 42 | |
| D0..D7 | 39, 40, 41, 4, 7, 8, 46, 48 | 对应传感器模组的 D2..D9 |

像素格式 `PIXFORMAT_YUV422`（S3 的 DVP 不支持 RGB888 直出，采集后软件转），
`FRAMESIZE_QVGA`，`fb_count=2`，帧缓冲放 PSRAM。

**V1.1 板必须先开电源轨**，否则永远没有画面。AXP313A 上电顺序写死在 `app_main`：

```
shared_i2c_init() -> axp313a_begin(0x36) -> axp313a_set_camera_power(1.5V DVDD, 2.8V AVDD)
  -> 等 200ms -> camera_init()
```

V1.2+ 板不需要这一步。

### 2.3 推流屏 2.0" ST7789（GDI FPC 座）

| 信号 | GPIO | 排针 |
|------|------|------|
| MOSI | 15 | |
| SCK | 17 | |
| CS | 18 | D6 |
| DC | 3 | D2 |
| RST | 38 | D3 |
| BL | 21 | D13 |

走 `SPI2_HOST`，40 MHz。

### 2.4 出题屏 4.0" ST7796S（杜邦线到排针，独立 `SPI3_HOST`）

模组单排 14 针，从 VCC 那端数：

```
1 VCC   2 GND   3 LCD_CS  4 LCD_RST  5 LCD_RS  6 SDI(MOSI)  7 SCK
8 LED   9 SDO(MISO)  10 CTP_SCL  11 CTP_RST  12 CTP_SDA  13 CTP_INT  14 SD_CS
```

实际接线（固件默认值在 `components/tft_display/tft_big.c` 的 `s_pin_*`）：

| 模组脚 | 接到 | ESP32-S3 GPIO | 排针 |
|--------|------|---------------|------|
| 1 VCC | 5V（官方推荐 5V，模组板载 3.3V LDO） | — | |
| 2 GND | GND | — | |
| 3 LCD_CS | D7 | **IO9** | D7 |
| 4 LCD_RST | D10 | **IO14** | D10 |
| 5 LCD_RS (DC) | D11 | **IO13** | D11 |
| 6 SDI | A5 | **IO11** | A5 |
| 7 SCK | A4 | **IO10** | A4 |
| 8 LED | **3V3 常亮** | — | 不占 GPIO |
| 9 SDO(MISO) | D12 | **IO12** | D12 |
| 14 SD_CS | **3V3** | — | ★ 见下 |
| 10-13（电容触摸四根） | 悬空不接 | — | 未使用 |

### 2.5 ★★ SD_CS(14) 绝对不能悬空

这是整个项目最贵的一条经验：**这个模组把屏的 SDO 和片上 SD 卡的 MISO 并在了一起**
（pin 9），各自靠自己的片选选通。`SD_CS` 浮空时 SD 卡会被选中并**持续驱动**那条共享线，
后果不只是读不到卡——整块屏一起废掉：恒定白屏，任何图案/颜色都不动，换 RS/RST/LED
的排列组合都没反应。

判据：纯软件探针读 SDO 那脚得到 `000/00`（内部上拉拉不起、推挽写 1 也拉不起 = 被硬按在 0）。
把 `SD_CS` 接 3V3 之后立刻变成 `000/10`，写入当场生效、屏就亮了。

推论：这类「屏 + SD」或「屏 + 触摸」的二合一模组，**所有片选脚都按必接脚处理**，
别默认「不用就可以不接」。

## 3. 这块屏的三个死值（实测，别按 datasheet 猜）

| 项 | 值 | 说明 |
|----|----|------|
| MADCTL | `0xEC` = MY\|MX\|MV\|BGR\|MH（ML 清 0） | 原生 320x480 落成 480x320 横屏 |
| 窗口口径 `map` | `0` | CASET←逻辑 x(0..479)、RASET←逻辑 y(0..319)，数据按行主序直送、不转置 |
| SPI 时钟 `hz` | 默认 **1 MHz**（保守值） | 杜邦线长、无地线参考时 20 MHz 会把数据打烂；用 `/big?hz=` 逐档往上顶 |

MADCTL 的试出来顺序是 `0x2C`（整图转 180°）→ `0xAC`（左右好了上下还反）→ `0xEC`（全对）。
两个反直觉的点：MV=1 之后 MX/MY 管的物理轴向是**互换**的；MH/ML 只管扫描顺序，
多置 ML（→`0xFC`）会再把画面翻一次。**判方向只能用「上下+左右都不对称的真照片」**
（纯色矩形、十字、棋盘格对 180° 和转置全都看不出来）。

`map=1`（转置口径）时窗口只有 320 个物理列，画面被推到玻璃一侧、只盖住 2/3，
剩下 1/3 露的是显存里上电前的旧图案——一眼像「CASET 超 319 回绕」，其实是覆盖不足。
接缝落在 320/480 处时先怀疑写入窗口。

**8 位 SPI 读回不可用**：`RDDID(04h)` 恒返回 `0x00000000`，即使屏正常显示也一样。
不要把「读不到 ID」当成链路不通的判据。

## 4. 空闲 GPIO 与后续接线

插满 GDI + CAM 之后，板上只剩 **IO9 / IO10 / IO11 / IO12 / IO13 / IO14 / IO16** 可用
（`D9` 丝印 = IO0 是 Boot 脚；IO35-37 被八线 PSRAM 占用，外部不可用）。
当前 4 寸屏用掉了 9/10/11/12/13/14，只剩 **IO16**——而 IO16 同时是 GDI 2" 屏的 MISO
网络，两块屏会在这根线上碰头，慎用。

已定的改进方向（等动手）：**SDO 读回本来就用不了，IO12 那根线可以挪作背光控制**——
把模组 pin 8 LED 从 3V3 改到 IO12，固件里 `GET /big?bl_pin=12` 在线验证后再把
`s_pin_bl` 的默认值改成 12，并把 `tft_big_backlight()` 从「开/关」升级成 LEDC PWM 调光。
这样就能用背光亮度直接压屏幕眩光（见 `docs/selflearn.md` 的反光实测）。

## 5. 装完之后先跑这三步

```bash
# 1) 上电看串口：应出现 "图库就绪：103 张，间隔 120 秒" 和 "4寸出题屏自检图形已输出"
python3 tools/devlog.py /tmp/devlog.txt 1800     # 抄 30 分钟串口日志到文件

# 2) 确认两块屏都活着：2" 屏应显示 IP，4" 屏应打出自检图形
curl "http://<ip>/big?c=bars"           # 4" 屏彩条；可选 bars/red/green/blue/black/
                                         #                     white/checker/blit/axis/probe
curl "http://<ip>/result.json"           # 端侧当前判定

# 3) 用摄像头反过来验 4" 屏：把镜头对准屏幕，取模型真正吃进去的那一帧
curl -o /tmp/cap.jpg "http://<ip>/capture.jpg"
python3 tools/items103/cap_sheet.py      # 实拍帧 vs 题库原图 对照表
```

接线没定死的时候，`/big?mosi=&sck=&cs=&dc=&rst=&bl_pin=&miso=&hz=&madctl=&map=&mode=&rstpol=`
可以在线改再 `c=` 重画，不用一版一版烧录。详见 `docs/http-api.md`。
