// tft_big.h — 4.0" 480x320 ST7796S 出题屏（独立 SPI3 总线，与 2" ST7789 互不干扰）
#ifndef TFT_BIG_H
#define TFT_BIG_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BIG_WIDTH   480
#define BIG_HEIGHT  320

// RGB565（大端写入屏内存，与 tft_display.h 同一定义）
#ifndef RGB565C
#define RGB565C(r, g, b)  ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#endif

/**
 * 初始化 4" ST7796S 屏。独立占用 SPI3_HOST：MOSI=IO11(A5) SCK=IO10(A4) CS=IO9(D7)
 * RST=IO14(D10) DC=IO13(D11) SDO=IO12(D12)；屏的 LED 硬接 3V3 不占 GPIO，
 * 剩下的 IO16(MISO) 留给 SD 卡的 SD_CS。与 2" ST7789(SPI2_HOST) 无关，调用顺序随意。
 * @return 0=成功, -1=失败
 */
int tft_big_init(void);

/** 关显示 + 灭背光（休眠） */
void tft_big_sleep(void);

void tft_big_clear(uint16_t color);
void tft_big_fill_rect(int x, int y, int w, int h, uint16_t color);

/** 大端 RGB565 位图局部刷屏（len 必须等于 w*h*2） */
void tft_big_blit(int x, int y, int w, int h, const uint8_t *be565);

/** 整屏写入：be565 为 BIG_WIDTH*BIG_HEIGHT*2 字节，大端 RGB565 */
void tft_big_write_frame(const uint8_t *be565);

/** 0/90/180/270 横竖屏与镜像修正（只改 MADCTL，不动帧缓冲） */
void tft_big_set_rotation(int deg);

/** 反色开关：颜色发阴/红蓝颠倒时切换这个 */
void tft_big_set_invert(bool on);

/** 背光 0-100（GPIO 未接调光时忽略） */
void tft_big_backlight(int percent);

/** 运行时改窗口/接线（诊断用）。传 -1 表示该信号保持不变；
 *  只接受 {9,10,11,12,13,14,16}，改完整条 SPI3 总线与面板重新初始化。
 *  @return 0=成功, -1=失败或参数被拒绝 */
int tft_big_remap(int mosi, int sck, int cs, int dc, int rst, int bl);

/** 当前引脚/时钟/映射配置，用于回显诊断 */
const char *tft_big_pin_desc(void);

/** 运行时改 SPI 时钟（Hz，限 100k~40M），用于逐档试出杜邦线能跑多快。返回生效值或 -1 */
int tft_big_set_freq(int hz);

/** 几何探针：边框 + 十字 + 左上红块，用来区分信号完整性问题和地址/方向问题 */
void tft_big_probe(void);

/** 40px 棋盘格：字节错位 / 地址回绕最容易在这种图案上暴露 */
void tft_big_checker(void);

/** 转置自检：走 tft_big_blit 送一块渐变+四角色块，专门验证送数据顺序（探针图是纯色矩形，看不出顺序错） */
void tft_big_blittest(void);

/** 轴向编码图：左半 8 条竖带编码 x 方向、右半 8 条横带编码 y 方向、左上橙色块标 (0,0)。
 *  一张照片读出「色带顺序 + 橙块在哪个角」就能定死 MV/MX/MY，不依赖尺寸测量 */
void tft_big_axis(void);

/** 诊断串：内部/PSRAM 余量 + 上次 blit 自检失败原因 */
const char *tft_big_diag(void);

/** 解一幅 JPEG 并全屏显示：等比缩放居中、上下/左右黑边 letterbox。
 *  返回 0=成功，负数=失败码（-1 屏未就绪 / -2 参数 / -3 PSRAM 不够 / -4 不是合法 JPEG）；
 *  info 回填一行诊断（源尺寸、落位、耗时）。解码缓冲放 PSRAM，首次调用分配后常驻复用。 */
int tft_big_show_jpeg(const uint8_t *jpg, size_t len, char *info, size_t sz);

/** 窗口地址映射口径：0=CASET 送逻辑 x(0..479)、行主序送数据（默认，实测整屏铺满且方向正确）；
 *  1=CASET 送逻辑 y、blit 按列转置送数据 —— 实测会把图转置 90° 且只盖住 2/3 玻璃，只留作对照 */
void tft_big_set_map(int on);

/** SPI 模式 0..3（个别模组要 mode3）。内部重建设备，返回生效值或 -1 */
int tft_big_set_mode(int m);

/** 复位极性：1=低电平复位（默认），0=高电平复位。改完自动重新初始化面板 */
void tft_big_set_rstpol(int pol);

/** 接/断屏的 SDO（-1=断开）。会重建 SPI3 总线，之后才能读回 ID/状态 */
int tft_big_set_miso(int p);

/** 读 ST7796S 的 RDDID：正常 = 0x00858504。全 0=没接/没上电，全 F=浮空 */
uint32_t tft_big_read_id(void);

/** 读 RDDST(09h)：bit2=DISPON, bit1=INVOFF, bit0=SLPOUT */
uint32_t tft_big_read_status(void);

/** 直接写 MADCTL 原始值（诊断用），返回生效值 */
int tft_big_set_madctl(int raw);

/** 逐脚量「有没有外部设备在驱动这根线」，抓与 GDI 排线冲突（如触摸 BUSY/INT）。
 *  返回 "IO9:010/10 IO10:..." 形式：高阻/上拉/下拉 读数 + 写1/写0 读回。
 *  上拉=1 且 下拉=0 = 浮空；三种输入读数相同 = 被外部强驱动。 */
const char *tft_big_gpio_probe(void);

/** 彩条自检图：接线是否正确的最小验证画面 */
void tft_big_colorbars(void);

#ifdef __cplusplus
}
#endif

#endif // TFT_BIG_H
