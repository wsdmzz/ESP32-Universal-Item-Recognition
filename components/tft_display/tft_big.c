// tft_big.c — 4.0" 480x320 ST7796S，独立走 SPI3_HOST，不与 2" ST7789(SPI2) 抢总线
//
// 接线（FireBeetle 2 ESP32-S3 排针丝印，官方 GPIO 表 Num/Pin/GDI/CAM/Map/Other 的 Map 列）：
//   SDI(MOSI) -> IO11  排针 A5
//   SCK       -> IO10  排针 A4
//   LCD_CS    -> IO9   排针 D7   (GDI 的 SD_CS，纯 ST7789 排线未使用)
//   LCD_RST   -> IO14  排针 D10  (GDI 的 BUSY)
//   LCD_RS    -> IO13  排针 D11  (GDI 的 INT)
//   LED       -> 3V3   常亮，不占 GPIO（IO16/排针 MISO 空着，留给 SD 卡的 SD_CS）
//   SDO(MISO) -> IO12  排针 D12  ← 屏第 9 脚，与模组上的 SD 卡共用一条线
//   SD_CS     -> 3V3   ★ 绝对不能悬空：悬空时片上 SD 卡选中、把共享的 MISO 拉死，整块屏白屏
//   VCC -> VCC(5V)   GND -> GND
//
// ★ 官方表 Map 列逐行核对结果：IO12=D12、IO13=D11、IO14=D10、IO9=D7、
//   IO3=D2、IO15=MOSI、IO16=MISO、IO17=SCK、IO18=D6、IO21=D13、IO47=D14、IO48=D9。
//   曾经记成「IO12=D11、IO13=D12」，RS 一直被驱动在没接线的 D12 上，
//   屏收到的全是数据、一条命令都没进去 —— 表现就是恒定白屏、怎么换图案都没反应。
//
// 禁用提醒：排针 D9 = IO0 是 Boot strapping，IO35/36/37 被八线 PSRAM 占用，
//          IO19/20 是 USB D-/D+（烧录口），都不可挪作屏幕信号。
//
// 初始化寄存器值取自 Espressif esp-bsp/components/lcd/esp_lcd_st7796 的
// vendor_specific_init_default（官方 ST7796 表），未自行猜测。
#include "tft_big.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "jpeg_decoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "TFTBIG";

static bool pin_allowed(int p);   // 定义在文件末尾，前面的 set_miso/remap 要用

// 引脚做成变量：接线没定时可以用 /big?dc=..&bl_pin=.. 在线改、用摄像头看结果，
// 不用一版一版烧录。默认值取自官方 GPIO 表的 Map 列：
//   IO9=D7  IO10=A4  IO11=A5  IO12=D12  IO13=D11  IO14=D10  IO16=MISO
static int s_pin_mosi = 11;   // 排针 A5
static int s_pin_sck  = 10;   // 排针 A4
static int s_pin_cs   = 9;    // 排针 D7
static int s_pin_dc   = 13;   // 排针 D11  ← 官方表 Map 列：IO13=D11、IO12=D12
static int s_pin_rst  = 14;   // 排针 D10
static int s_pin_bl   = -1;   // 屏的 LED 已硬接 3V3 常亮，不占 GPIO；-1=不驱动背光
static int s_pin_miso = 12;   // 排针 D12 ← 屏的第 9 脚 SDO(MISO)，与 SD 卡共用一条线
static int s_rst_pol  = 1;    // 1=低电平复位（常规），0=高电平复位（模组带反相时）
static int s_hz       = 1 * 1000 * 1000;   // 上电保守值：杜邦线长、无地线参考时 20MHz
                                           // 会把数据打烂。稳定后用 /big?hz= 逐档往上顶。
static int s_mode     = 0;                 // SPI 模式 0..3，个别 ST7796 模组要 mode3

#define BIG_SPI_HOST    SPI3_HOST

// 命令
#define CMD_SWRESET     0x01
#define CMD_SLPIN       0x10
#define CMD_SLPOUT      0x11
#define CMD_NORON       0x13
#define CMD_INVOFF      0x20
#define CMD_INVON       0x21
#define CMD_DISPOFF     0x28
#define CMD_DISPON      0x29
#define CMD_CASET       0x2A
#define CMD_RASET       0x2B
#define CMD_RAMWR       0x2C
#define CMD_COLMOD      0x3A
#define CMD_MADCTL      0x36

// MADCTL 位
#define MAD_MY          0x80
#define MAD_MX          0x40
#define MAD_MV          0x20
#define MAD_ML          0x10
#define MAD_BGR         0x08
#define MAD_MH          0x04

static spi_device_handle_t s_dev;
static SemaphoreHandle_t s_mux;
// 实测定死的组合（拿 480x320 真照片上屏、逐特征比对出来的，不是推的）：
//   MV|MH 把原生 320x480 落成 480x320 横屏；BGR 修 R/B 对调。
//   方向位是一个一个试出来的：0x2C 整图转 180° → 置 MY(=0xAC) 只修好了左右、上下还反着
//   → 再置 MX(=0xEC) 才全对。★ 注意 MX/MY 管的是「地址映射」，而 MH/ML 只管刷新扫描顺序，
//   别拿 datasheet 上"MX=列方向"的字面含义去猜：MV=1 之后 MX/MY 的物理轴向是互换的。
//   0xEC = MY|MX|MV|BGR|MH，ML 保持清 0（多置 ML 会变 0xFC，画面会再被翻一次）。
static uint8_t s_madctl = MAD_MV | MAD_MH | MAD_MY | MAD_MX | MAD_BGR;   // 0xEC
static bool s_invert;                                  // 实测这块模组要 INVOFF（开了反色全图按位取反）
static uint8_t s_map = 0;                               // 窗口地址映射口径，见 big_set_window
                                                        // 默认 0：CASET 直接送逻辑 x(0..479)，官方 esp_lcd 口径。
                                                        // 之前记成"CASET 上限 319、只能送 0..319"是误判：
                                                        // 那个"接缝落在玻璃 2/3 处"其实是 map=1 把图像转置后
                                                        // 只盖住 480 物理列里的 320 列，窗口本身没回绕。
static int s_blit_err;                                  // 0=正常，-2=blittest 的 PSRAM 缓冲没分到
static bool s_ready;

// DMA 中转缓冲：总线 max_transfer_sz 与它一致，且必须放内部 RAM。
// 内部 RAM 很紧（识别任务的栈也从这里来），所以只用 2KB。
static uint8_t s_chunk[2048];

// ========== 底层 ==========

static void big_cmd(uint8_t cmd)
{
    gpio_set_level(s_pin_dc, 0);
    spi_transaction_t t = { .length = 8, .tx_buffer = &cmd };
    ESP_ERROR_CHECK(spi_device_transmit(s_dev, &t));
}

static void big_data(const uint8_t *data, size_t len)
{
    if (!len) return;
    gpio_set_level(s_pin_dc, 1);
    spi_transaction_t t = { .length = len * 8, .tx_buffer = data };
    ESP_ERROR_CHECK(spi_device_transmit(s_dev, &t));
}

static void big_params(uint8_t cmd, const uint8_t *p, int n)
{
    big_cmd(cmd);
    big_data(p, n);
}

static void big_set_window(int x0, int y0, int x1, int y1)
{
    // s_map=0（默认，实测正确）：CASET 送逻辑 x(0..479)、RASET 送逻辑 y(0..319)。
    //   MV=1 之后 CASET 就是物理长边，能一路送到 479，不会回绕；RAMWR 的快轴也正好
    //   沿逻辑 x，所以 blit 直接按行主序送，不用转置。
    // s_map=1：CASET 送逻辑 y、RASET 送逻辑 x。这是把 MADCTL 的方向位当成"能靠地址救"
    //   时的老写法，实测结果是图像转置 90°、且只盖住玻璃的 2/3（没写到的那一块留着
    //   上电前的显存旧内容，看着像"旧图没清干净"）。用 /big?map=1 复现，别当默认。
    uint8_t ca[4], ra[4];
    if (s_map) {
        ca[0] = y0 >> 8; ca[1] = y0 & 0xFF; ca[2] = y1 >> 8; ca[3] = y1 & 0xFF;
        ra[0] = x0 >> 8; ra[1] = x0 & 0xFF; ra[2] = x1 >> 8; ra[3] = x1 & 0xFF;
    } else {
        ca[0] = x0 >> 8; ca[1] = x0 & 0xFF; ca[2] = x1 >> 8; ca[3] = x1 & 0xFF;
        ra[0] = y0 >> 8; ra[1] = y0 & 0xFF; ra[2] = y1 >> 8; ra[3] = y1 & 0xFF;
    }
    big_cmd(CMD_CASET); big_data(ca, 4);
    big_cmd(CMD_RASET); big_data(ra, 4);
    big_cmd(CMD_RAMWR);
}

// ========== 绘制 ==========

void tft_big_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!s_ready) return;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > BIG_WIDTH)  w = BIG_WIDTH - x;
    if (y + h > BIG_HEIGHT) h = BIG_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    xSemaphoreTakeRecursive(s_mux, portMAX_DELAY);
    big_set_window(x, y, x + w - 1, y + h - 1);

    int chunk_px = (int)sizeof(s_chunk) / 2 / w;
    if (chunk_px < 1) chunk_px = 1;

    uint8_t hi = color >> 8, lo = color & 0xFF;
    for (int i = 0; i < chunk_px * w; i++) {
        s_chunk[i * 2] = hi;
        s_chunk[i * 2 + 1] = lo;
    }

    gpio_set_level(s_pin_dc, 1);
    int remaining = w * h;
    while (remaining > 0) {
        int px = remaining < chunk_px * w ? remaining : chunk_px * w;
        spi_transaction_t t = { .length = px * 16, .tx_buffer = s_chunk };
        spi_device_transmit(s_dev, &t);
        remaining -= px;
    }
    xSemaphoreGiveRecursive(s_mux);
}

void tft_big_clear(uint16_t color)
{
    tft_big_fill_rect(0, 0, BIG_WIDTH, BIG_HEIGHT, color);
}

void tft_big_blit(int x, int y, int w, int h, const uint8_t *be565)
{
    if (!s_ready || !be565) return;
    if (x < 0 || y < 0 || w <= 0 || h <= 0) return;
    if (x + w > BIG_WIDTH || y + h > BIG_HEIGHT) return;

    xSemaphoreTakeRecursive(s_mux, portMAX_DELAY);
    big_set_window(x, y, x + w - 1, y + h - 1);
    gpio_set_level(s_pin_dc, 1);

    if (s_map) {
        // map=1 时窗口是 CASET<-逻辑 y、RASET<-逻辑 x，而 RAMWR 永远沿 CASET(快轴)
        // 填满一行再进 RASET，所以必须**按列**送数据：外层逻辑 x、内层逻辑 y。
        // 源缓冲是行主序（每行 w 像素），这里做一次转置，不另开大缓冲。
        int cols = (int)(sizeof(s_chunk) / 2 / (size_t)h);
        if (cols < 1) cols = 1;
        for (int c0 = 0; c0 < w; c0 += cols) {
            int nc = w - c0 < cols ? w - c0 : cols;
            int n = 0;                                    // 本次送出的字节数
            for (int c = 0; c < nc; c++) {
                const uint8_t *src = be565 + (size_t)(c0 + c) * 2;
                for (int r = 0; r < h; r++) {
                    s_chunk[n++] = src[(size_t)r * w * 2];
                    s_chunk[n++] = src[(size_t)r * w * 2 + 1];
                }
            }
            spi_transaction_t t = { .length = n * 8, .tx_buffer = s_chunk };
            spi_device_transmit(s_dev, &t);
        }
        xSemaphoreGiveRecursive(s_mux);
        return;
    }

    int total = w * h * 2;
    int off = 0;
    while (off < total) {
        int n = total - off;
        if (n > (int)sizeof(s_chunk)) n = sizeof(s_chunk);
        n &= ~1;   // 整像素对齐
        memcpy(s_chunk, be565 + off, n);
        spi_transaction_t t = { .length = n * 8, .tx_buffer = s_chunk };
        spi_device_transmit(s_dev, &t);
        off += n;
    }
    xSemaphoreGiveRecursive(s_mux);
}

void tft_big_write_frame(const uint8_t *be565)
{
    tft_big_blit(0, 0, BIG_WIDTH, BIG_HEIGHT, be565);
}

// ========== 显示控制 ==========

void tft_big_set_rotation(int deg)
{
    uint8_t m;
    switch (deg) {
    case 0:   m = MAD_MV | MAD_MH;                        break;   // 横屏 480x320（基准）
    case 90:  m = MAD_MY | MAD_MX;                        break;   // 竖屏 320x480
    case 180: m = MAD_MV | MAD_MX | MAD_ML;               break;
    case 270: m = MAD_ML;                                 break;   // 竖屏反向
    default:  return;
    }
    // 这块模组的 R/B 是反的，四个方向都带上 BGR 位
    s_madctl = m | MAD_BGR;
    if (!s_ready) return;
    xSemaphoreTakeRecursive(s_mux, portMAX_DELAY);
    big_params(CMD_MADCTL, &s_madctl, 1);
    xSemaphoreGiveRecursive(s_mux);
}

void tft_big_set_invert(bool on)
{
    s_invert = on;
    if (!s_ready) return;
    xSemaphoreTakeRecursive(s_mux, portMAX_DELAY);
    big_cmd(on ? CMD_INVON : CMD_INVOFF);
    xSemaphoreGiveRecursive(s_mux);
}

// 直接写 MADCTL 原始值（诊断用，十六进制字符串），方便一次照片就把方向定死
int tft_big_set_madctl(int raw)
{
    s_madctl = (uint8_t)raw;
    if (!s_ready) return 0;
    xSemaphoreTakeRecursive(s_mux, portMAX_DELAY);
    big_params(CMD_MADCTL, &s_madctl, 1);
    xSemaphoreGiveRecursive(s_mux);
    ESP_LOGI(TAG, "MADCTL -> 0x%02X", s_madctl);
    return s_madctl;
}

void tft_big_backlight(int percent)
{
    if (s_pin_bl < 0) return;      // LED 硬接 3V3，没有可控背光脚
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    gpio_set_level(s_pin_bl, percent > 0 ? 1 : 0);
}

// IDF 5.3 没有 spi_device_set_frequency()，运行时改时钟只能摘掉设备再按新时钟挂回去。
// 这样就能用 curl 一档一档试最高稳定时钟，不必反复烧录。
int tft_big_set_freq(int hz)
{
    if (!s_mux || s_dev == NULL) return -1;
    if (hz < 100 * 1000) hz = 100 * 1000;
    if (hz > 40 * 1000 * 1000) hz = 40 * 1000 * 1000;

    xSemaphoreTakeRecursive(s_mux, portMAX_DELAY);
    if (s_dev) {
        spi_bus_remove_device(s_dev);
        s_dev = NULL;
    }
    spi_device_interface_config_t dev = {
        .clock_speed_hz = hz,
        .mode = s_mode,
        .spics_io_num = s_pin_cs,
        .queue_size = 7,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    esp_err_t err = spi_bus_add_device(BIG_SPI_HOST, &dev, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "重建 SPI3 设备失败(%d Hz): %s", hz, esp_err_to_name(err));
        xSemaphoreGiveRecursive(s_mux);
        return -1;
    }
    s_hz = hz;
    s_ready = true;
    xSemaphoreGiveRecursive(s_mux);
    ESP_LOGI(TAG, "ST7796S SPI 时钟 -> %d Hz", hz);
    return hz;
}

void tft_big_set_map(int on)
{
    s_map = on ? 1 : 0;
    ESP_LOGI(TAG, "窗口映射口径 -> CASET=%s", s_map ? "逻辑y(原生源轴)" : "逻辑x");
}

int tft_big_set_mode(int m)
{
    if (m < 0 || m > 3) return -1;
    s_mode = m;
    int r = tft_big_set_freq(s_hz);   // 重建设备才能换模式，顺带保持原频率
    ESP_LOGI(TAG, "ST7796S SPI 模式 -> %d", m);
    return r;
}

// 复位极性：模组上若串了反相器，低有效会把屏一直按在复位里（表现=恒定白屏）。
void tft_big_set_rstpol(int pol)
{
    s_rst_pol = pol ? 1 : 0;
    if (s_mux) xSemaphoreTakeRecursive(s_mux, portMAX_DELAY);
    tft_big_init();
    if (s_mux) xSemaphoreGiveRecursive(s_mux);
    ESP_LOGI(TAG, "RST 极性 -> %s", s_rst_pol ? "低电平复位" : "高电平复位");
}

// 接上屏的 SDO(MISO) 后整条总线要重建，所以单独一个入口。传 -1 断开。
int tft_big_set_miso(int p)
{
    if (p >= 0 && !pin_allowed(p)) {
        ESP_LOGW(TAG, "MISO 脚 %d 不在可用集合，忽略", p);
        return -1;
    }
    if (p == s_pin_miso) return 0;
    if (!s_mux) s_mux = xSemaphoreCreateRecursiveMutex();
    xSemaphoreTakeRecursive(s_mux, portMAX_DELAY);
    s_ready = false;
    if (s_dev) { spi_bus_remove_device(s_dev); s_dev = NULL; }
    spi_bus_free(BIG_SPI_HOST);
    gpio_reset_pin(9); gpio_reset_pin(10); gpio_reset_pin(11);
    gpio_reset_pin(12); gpio_reset_pin(13); gpio_reset_pin(14); gpio_reset_pin(16);
    s_pin_miso = p;
    int r = tft_big_init();
    xSemaphoreGiveRecursive(s_mux);
    return r;
}

// 读一条带返回的寄存器（4 线 8bit SPI：DC=0 发命令，DC=1 发 1 个哑字节再读 4 字节）
static uint32_t big_read_param(uint8_t cmd)
{
    if (!s_ready || s_pin_miso < 0) return 0;
    xSemaphoreTakeRecursive(s_mux, portMAX_DELAY);
    big_cmd(cmd);
    gpio_set_level(s_pin_dc, 1);
    uint8_t tx[1] = { 0 };
    spi_transaction_t t = { 0 };
    t.flags    = SPI_TRANS_USE_RXDATA;
    t.length   = 8;      // 哑字节
    t.rxlength = 32;     // 4 字节返回
    t.tx_buffer = tx;
    esp_err_t err = spi_device_transmit(s_dev, &t);
    uint32_t v = 0;
    if (err == ESP_OK)
        v = ((uint32_t)t.rx_data[0] << 24) | ((uint32_t)t.rx_data[1] << 16) |
            ((uint32_t)t.rx_data[2] << 8) | t.rx_data[3];
    else
        ESP_LOGE(TAG, "读 0x%02X 失败: %s", cmd, esp_err_to_name(err));
    xSemaphoreGiveRecursive(s_mux);
    return v;
}

// ST7796S 的 RDDID(04h) 正常返回 00 85 85 04；RDDST(09h) 返回睡眠/反色/显示状态。
// 读回全 0 = SDO 没接或屏没上电；读回全 FF = 线浮空/屏没驱动。
uint32_t tft_big_read_id(void)  { return big_read_param(0x04); }
uint32_t tft_big_read_status(void) { return big_read_param(0x09); }

// 棋盘格：字节错位/地址回绕在这种图案下最显眼
void tft_big_checker(void)
{
    const int S = 40;
    for (int y = 0; y < BIG_HEIGHT; y += S) {
        for (int x = 0; x < BIG_WIDTH; x += S) {
            bool on = (((x / S) + (y / S)) & 1) != 0;
            tft_big_fill_rect(x, y, S, S, on ? RGB565C(255, 255, 255) : RGB565C(0, 0, 0));
        }
    }
}

// 几何探针：四角色块 + 粗边框 + 中心十字。// 用来区分「SPI 数据被打烂」和「地址/方向配置错」：
//   图形完整清晰 = 链路 OK；斜切/回绕/错位 = MADCTL 或 COLMOD 问题；
//   几乎全白只剩零星条 = 数据线接触不良或时钟过快。
void tft_big_probe(void)
{
    tft_big_clear(RGB565C(0, 0, 0));
    const int B = 24;
    tft_big_fill_rect(0, 0, BIG_WIDTH, B, RGB565C(255, 255, 255));           // 上白
    tft_big_fill_rect(0, BIG_HEIGHT - B, BIG_WIDTH, B, RGB565C(255, 255, 0)); // 下黄
    tft_big_fill_rect(0, 0, B, BIG_HEIGHT, RGB565C(0, 255, 255));            // 左青
    tft_big_fill_rect(BIG_WIDTH - B, 0, B, BIG_HEIGHT, RGB565C(255, 0, 255)); // 右品红
    tft_big_fill_rect(BIG_WIDTH / 2 - 6, B, 12, BIG_HEIGHT - 2 * B, RGB565C(0, 255, 0));
    tft_big_fill_rect(B, BIG_HEIGHT / 2 - 6, BIG_WIDTH - 2 * B, 12, RGB565C(0, 0, 255));
    // 左上角一个 40x40 亮红方块：确认 (0,0) 原点位置，判断有没有镜像
    tft_big_fill_rect(B + 4, B + 4, 40, 40, RGB565C(255, 0, 0));
}

// 转置自检：走 tft_big_blit（真实出题图的路径），探针图全是纯色矩形、
// 对数据顺序不敏感，只有这种「一块一块内容各不相同」的图才能看出送数据顺序错。
// 图案：左半水平渐变(黑→白，右端最亮)、右半垂直渐变(上黑→下白)、
//       四角 20x20 分别 红(左上)/绿(右上)/蓝(左下)/白(右下)，
//       中间一条 4px 亮黄竖线。顺序错的话渐变方向会转 90°、色块会跑到对边。
void tft_big_blittest(void)
{
    const int W = 240, H = 160;
    static uint8_t *buf;
    if (!buf) {
        buf = heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM);
        if (!buf) {
            ESP_LOGE(TAG, "blittest 缓冲分配失败(需要 %d 字节 PSRAM)", W * H * 2);
            s_blit_err = -2;
            return;
        }
    }
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint16_t c;
            if (x < W / 2) {
                int v = x * 255 / (W / 2 - 1);
                c = RGB565C(v, v, v);                       // 左半：水平渐变
            } else {
                int v = y * 255 / (H - 1);
                c = RGB565C(v, v, v);                       // 右半：垂直渐变
            }
            if (x >= W / 2 - 2 && x <= W / 2 + 1) c = RGB565C(255, 255, 0);   // 中线
            if (x < 20 && y < 20)               c = RGB565C(255, 0, 0);
            if (x >= W - 20 && y < 20)          c = RGB565C(0, 255, 0);
            if (x < 20 && y >= H - 20)          c = RGB565C(0, 128, 255);
            if (x >= W - 20 && y >= H - 20)     c = RGB565C(255, 255, 255);
            buf[(size_t)(y * W + x) * 2]     = c >> 8;      // 大端 RGB565
            buf[(size_t)(y * W + x) * 2 + 1] = c & 0xFF;
        }
    }
    tft_big_clear(RGB565C(0, 0, 0));
    tft_big_blit((BIG_WIDTH - W) / 2, (BIG_HEIGHT - H) / 2, W, H, buf);
    s_blit_err = 0;
}

// 轴向编码图：一张照片就能定死 x/y 的方向和是否转置。
// 左半 = 8 条竖带（按逻辑 x 递增排 白黄青绿品红红蓝黑），右半 = 8 条横带（按逻辑 y 递增同序），
// 左上角一个橙色块标 (0,0)。照片里只要读出「色带顺序」和「橙块落在哪个角」就够了，
// 不依赖精确尺寸测量，也不怕拍照有色偏。
void tft_big_axis(void)
{
    static const uint16_t seq[8] = {
        RGB565C(255, 255, 255), RGB565C(255, 255, 0),
        RGB565C(0, 255, 255),   RGB565C(0, 255, 0),
        RGB565C(255, 0, 255),   RGB565C(255, 0, 0),
        RGB565C(0, 0, 255),     RGB565C(0, 0, 0),
    };
    const int half_h = BIG_HEIGHT / 2;
    const int half_w = BIG_WIDTH / 2;
    tft_big_clear(RGB565C(130, 130, 130));
    for (int i = 0; i < 8; i++)                       // 左半：竖带，编码 x 方向
        tft_big_fill_rect(i * half_w / 8, 0, half_w / 8, half_h, seq[i]);
    for (int i = 0; i < 8; i++)                       // 右半：横带，编码 y 方向
        tft_big_fill_rect(half_w, half_h + i * half_h / 8, half_w, half_h / 8, seq[i]);
    tft_big_fill_rect(0, 0, 48, 48, RGB565C(255, 140, 0));   // (0,0) 橙色原点标记
}

// 诊断串：内部/PSRAM 余量 + 上次 blit 自检的失败原因（-2=PSRAM 分配失败）
const char *tft_big_diag(void)
{
    static char buf[128];
    snprintf(buf, sizeof(buf),
             "psram=%u internal=%u maxblk=%u blit_err=%d",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             s_blit_err);
    return buf;
}

// ========== JPEG 出题显示 ==========
//
// 走 esp_jpeg（ROM 里的 TJpgDec）软解，输出 RGB565，再等比缩放居中送屏。
// 三块缓冲全放 PSRAM：内部 RAM 接上两屏+摄像头后只剩十几 KB，放不下 300KB 帧。
// 缓冲 static 常驻复用，避免每次出题都做一次大分配（碎片 + 偶发失败）。
static int show_jpeg_locked(const uint8_t *jpg, size_t len, char *info, size_t sz);

int tft_big_show_jpeg(const uint8_t *jpg, size_t len, char *info, size_t sz)
{
    // work/dst 是常驻复用的静态缓冲，周期换图任务和 /show 端点会同时来，
    // 所以整段（解码 + 缩放 + 刷屏）都要压在总线锁里，不能只锁 blit。
    if (!s_ready) { if (info && sz) snprintf(info, sz, "panel not ready"); return -1; }
    xSemaphoreTakeRecursive(s_mux, portMAX_DELAY);
    int r = show_jpeg_locked(jpg, len, info, sz);
    xSemaphoreGiveRecursive(s_mux);
    return r;
}

static int show_jpeg_locked(const uint8_t *jpg, size_t len, char *info, size_t sz)
{
    if (info && sz) info[0] = 0;
    if (!jpg || len < 16 || len > 1024 * 1024) {
        if (info) snprintf(info, sz, "bad len %u", (unsigned)len);
        return -2;
    }

    // TJpgDec 的 scratchpad：ROM 版按 JD_FASTDECODE 要 3.1KB，非 ROM 高速版要 64KB，
    // 一律给 66KB，省得跟着 Kconfig 变。
    static uint8_t *work;
    static uint8_t *dst;            // 480x320 大端 RGB565，真正上屏的帧
    if (!work) work = (uint8_t *)heap_caps_malloc(66 * 1024, MALLOC_CAP_SPIRAM);
    if (!dst)  dst  = (uint8_t *)heap_caps_malloc((size_t)BIG_WIDTH * BIG_HEIGHT * 2,
                                                  MALLOC_CAP_SPIRAM);
    if (!work || !dst) {
        if (info) snprintf(info, sz, "no psram (work=%p dst=%p)", (void *)work, (void *)dst);
        return -3;
    }

    esp_jpeg_image_cfg_t cfg = {
        .indata      = (uint8_t *)jpg,
        .indata_size = (uint32_t)len,
        .out_format  = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale   = JPEG_IMAGE_SCALE_0,
        .advanced.working_buffer      = work,
        .advanced.working_buffer_size = 66 * 1024,
    };
    esp_jpeg_image_output_t io = {0};
    if (esp_jpeg_get_image_info(&cfg, &io) != ESP_OK) {
        if (info) snprintf(info, sz, "not a decodable jpeg");
        return -4;
    }
    const int sw = io.width, sh = io.height;

    uint8_t *src = (uint8_t *)heap_caps_malloc(io.output_len, MALLOC_CAP_SPIRAM);
    if (!src) { if (info) snprintf(info, sz, "no psram for src"); return -3; }
    cfg.outbuf      = src;
    cfg.outbuf_size = io.output_len;
    int64_t t0 = esp_timer_get_time();
    if (esp_jpeg_decode(&cfg, &io) != ESP_OK) {
        free(src);
        if (info) snprintf(info, sz, "decode failed (%ux%u)", sw, sh);
        return -4;
    }
    int64_t t_dec = esp_timer_get_time() - t0;

    // 等比缩放到屏内并居中；先铺黑底当 letterbox
    int dw = BIG_WIDTH, dh = (int)((int64_t)BIG_WIDTH * sh / sw);
    if (dh > BIG_HEIGHT) {
        dh = BIG_HEIGHT;
        dw = (int)((int64_t)BIG_HEIGHT * sw / sh);
    }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    const int ox = (BIG_WIDTH - dw) / 2;
    const int oy = (BIG_HEIGHT - dh) / 2;
    for (int y = 0; y < BIG_HEIGHT; y++) {
        uint8_t *d = dst + (size_t)y * BIG_WIDTH * 2;
        int sy = (y - oy) * sh / dh;
        for (int x = 0; x < BIG_WIDTH; x++) {
            uint16_t px = 0;
            int sx = (x - ox) * sw / dw;
            if (sy >= 0 && sy < sh && sx >= 0 && sx < sw) {
                // TJpgDec 出的是本机小端 RGB565，屏要大端，这里顺手换一次
                px = (uint16_t)((uint16_t)src[((size_t)sy * sw + sx) * 2 + 1] << 8 |
                                (uint16_t)src[((size_t)sy * sw + sx) * 2]);
            }
            d[x * 2]     = px >> 8;
            d[x * 2 + 1] = px & 0xFF;
        }
    }
    free(src);

    // dst 本身就是整屏 480x320（黑边已经在里面铺好了），blit 一次盖满全玻璃，
    // 不需要额外 clear —— 之前"旧图没清掉"是因为写入窗口只有 2/3 玻璃，不是没擦。
    tft_big_blit(0, 0, BIG_WIDTH, BIG_HEIGHT, dst);
    int64_t t_all = esp_timer_get_time() - t0;
    if (info)
        snprintf(info, sz, "src=%ux%u -> %dx%d@%d,%d dec=%lldms blit=%lldms",
                 sw, sh, dw, dh, ox, oy, (long long)(t_dec / 1000),
                 (long long)((t_all - t_dec) / 1000));
    ESP_LOGI(TAG, "JPEG 上屏 %s", info ? info : "");
    return 0;
}

void tft_big_sleep(void)
{
    if (!s_ready) return;
    xSemaphoreTakeRecursive(s_mux, portMAX_DELAY);
    big_cmd(CMD_DISPOFF);
    big_cmd(CMD_SLPIN);
    xSemaphoreGiveRecursive(s_mux);
    if (s_pin_bl >= 0) gpio_set_level(s_pin_bl, 0);
}

// ========== 彩条自检 ==========

void tft_big_colorbars(void)
{
    static const uint16_t bar[8] = {
        RGB565C(255, 255, 255), RGB565C(255, 255, 0),
        RGB565C(0, 255, 255),   RGB565C(0, 255, 0),
        RGB565C(255, 0, 255),   RGB565C(255, 0, 0),
        RGB565C(0, 0, 255),     RGB565C(0, 0, 0),
    };
    tft_big_clear(RGB565C(0, 0, 0));
    int bw = BIG_WIDTH / 8;
    for (int i = 0; i < 8; i++)
        tft_big_fill_rect(i * bw, 0, bw, BIG_HEIGHT - 40, bar[i]);

    // 底部灰阶梯度 + 四角色块：确认没有坏线、错位、色序反了
    for (int g = 0; g < 16; g++) {
        int v = g * 16;
        tft_big_fill_rect(g * (BIG_WIDTH / 16), BIG_HEIGHT - 40,
                          BIG_WIDTH / 16, 20, RGB565C(v, v, v));
    }
    tft_big_fill_rect(0,                 BIG_HEIGHT - 20, BIG_WIDTH / 4, 20, RGB565C(255, 0, 0));
    tft_big_fill_rect(BIG_WIDTH / 4,     BIG_HEIGHT - 20, BIG_WIDTH / 4, 20, RGB565C(0, 255, 0));
    tft_big_fill_rect(BIG_WIDTH / 2,     BIG_HEIGHT - 20, BIG_WIDTH / 4, 20, RGB565C(0, 0, 255));
    tft_big_fill_rect(BIG_WIDTH * 3 / 4, BIG_HEIGHT - 20, BIG_WIDTH / 4, 20, RGB565C(255, 255, 255));
}

// ========== 初始化 ==========

static const uint8_t GAMMA_P[14] = { 0xf0, 0x09, 0x0b, 0x06, 0x04, 0x15, 0x2f,
                                     0x54, 0x42, 0x3c, 0x17, 0x14, 0x18, 0x1b };
static const uint8_t GAMMA_N[14] = { 0xf0, 0x09, 0x0b, 0x06, 0x04, 0x03, 0x2d,
                                     0x43, 0x42, 0x3b, 0x16, 0x14, 0x17, 0x1b };

int tft_big_init(void)
{
    if (!s_mux) s_mux = xSemaphoreCreateRecursiveMutex();

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << s_pin_dc) | (1ULL << s_pin_rst),
        .mode = GPIO_MODE_OUTPUT,
    };
    if (s_pin_bl >= 0) io.pin_bit_mask |= 1ULL << s_pin_bl;
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(s_pin_dc, 1);
    if (s_pin_bl >= 0) gpio_set_level(s_pin_bl, 0);

    // 硬复位（s_rst_pol=1 低有效；若模组那一路带了反相，用 /big?rstpol=0 反过来试）
    gpio_set_level(s_pin_rst, s_rst_pol ? 0 : 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(s_pin_rst, s_rst_pol ? 1 : 0);
    vTaskDelay(pdMS_TO_TICKS(120));

    spi_bus_config_t bus = {
        .mosi_io_num     = s_pin_mosi,
        .miso_io_num     = s_pin_miso,
        .sclk_io_num     = s_pin_sck,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 2048,
    };
    esp_err_t err = spi_bus_initialize(BIG_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI3 bus init failed: %s", esp_err_to_name(err));
        return -1;
    }

    spi_device_interface_config_t dev = {
        .clock_speed_hz = s_hz,
        .mode = s_mode,
        .spics_io_num = s_pin_cs,
        .queue_size = 7,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    err = spi_bus_add_device(BIG_SPI_HOST, &dev, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        return -1;
    }

    big_cmd(CMD_SWRESET);   vTaskDelay(pdMS_TO_TICKS(150));
    big_cmd(CMD_SLPOUT);    vTaskDelay(pdMS_TO_TICKS(120));

    uint8_t colmod = 0x55;              // 16-bit/pixel
    big_params(CMD_COLMOD, &colmod, 1);

    // --- Espressif esp_lcd_st7796 官方 vendor init 表 ---
    uint8_t d;
    d = 0xc3; big_params(0xF0, &d, 1);          // 解锁命令 2/3
    d = 0x96; big_params(0xF0, &d, 1);
    d = 0x01; big_params(0xB4, &d, 1);          // Inversion Control
    d = 0xc6; big_params(0xB7, &d, 1);          // Entry Mode
    { uint8_t e8[8] = { 0x40, 0x8a, 0x00, 0x00, 0x29, 0x19, 0xa5, 0x33 };
      big_params(0xE8, e8, 8); }                 // 3-Gamma Control
    d = 0x06; big_params(0xC1, &d, 1);          // Power Control 2
    d = 0xa7; big_params(0xC2, &d, 1);          // Power Control 3
    d = 0x18; big_params(0xC5, &d, 1);          // VCM Control
    big_params(0xE0, GAMMA_P, 14);              // P-Gamma
    big_params(0xE1, GAMMA_N, 14);              // N-Gamma
    d = 0x3c; big_params(0xF0, &d, 1);          // 重新上锁
    d = 0x69; big_params(0xF0, &d, 1);
    // ------------------------------------------

    big_params(CMD_MADCTL, &s_madctl, 1);
    big_cmd(s_invert ? CMD_INVON : CMD_INVOFF);   // 这块模组实测要 INVOFF，反色会把全图按位取反
    big_cmd(CMD_NORON);   vTaskDelay(pdMS_TO_TICKS(10));
    big_cmd(CMD_DISPON);  vTaskDelay(pdMS_TO_TICKS(100));

    s_ready = true;
    if (s_pin_bl >= 0) gpio_set_level(s_pin_bl, 1);
    ESP_LOGI(TAG, "ST7796S ready (%dx%d) MADCTL=0x%02X inv=%d @%dHz mosi=%d sck=%d cs=%d dc=%d rst=%d bl=%d miso=%d",
             BIG_WIDTH, BIG_HEIGHT, s_madctl, (int)s_invert, s_hz,
             s_pin_mosi, s_pin_sck, s_pin_cs, s_pin_dc, s_pin_rst, s_pin_bl, s_pin_miso);
    return 0;
}

const char *tft_big_pin_desc(void)
{
    static char buf[128];
    snprintf(buf, sizeof(buf),
             "mosi=%d sck=%d cs=%d dc=%d rst=%d bl=%d miso=%d hz=%d map=%d m=%d rp=%d mad=0x%02X inv=%d",
             s_pin_mosi, s_pin_sck, s_pin_cs, s_pin_dc, s_pin_rst, s_pin_bl,
             s_pin_miso, s_hz, s_map, s_mode, s_rst_pol, s_madctl, (int)s_invert);
    return buf;
}

// 只允许用插满 GDI+CAM 之后真正空闲的脚，避免误碰摄像头/2寸屏/USB/Boot。
static bool pin_allowed(int p)
{
    return p == 9 || p == 10 || p == 11 || p == 12 || p == 13 || p == 14 || p == 16;
}

// 运行时改接线（诊断用）：-1 表示保持不变。改完整总线+面板重新初始化。
int tft_big_remap(int mosi, int sck, int cs, int dc, int rst, int bl)
{
    int cand[6] = { mosi, sck, cs, dc, rst, bl };
    for (int i = 0; i < 6; i++) {
        if (cand[i] >= 0 && !pin_allowed(cand[i])) {
            ESP_LOGW(TAG, "引脚 %d 不在可用集合 {9,10,11,12,13,14,16}，忽略", cand[i]);
            cand[i] = -1;
        }
    }
    int next[6] = { s_pin_mosi, s_pin_sck, s_pin_cs, s_pin_dc, s_pin_rst, s_pin_bl };
    for (int i = 0; i < 6; i++) if (cand[i] >= 0) next[i] = cand[i];
    // 六个信号必须各占一个脚
    for (int i = 0; i < 6; i++)
        for (int j = i + 1; j < 6; j++)
            if (next[i] == next[j]) {
                ESP_LOGW(TAG, "引脚 %d 被两个信号同时占用，放弃 remap", next[i]);
                return -1;
            }

    xSemaphoreTakeRecursive(s_mux, portMAX_DELAY);
    s_ready = false;
    if (s_dev) { spi_bus_remove_device(s_dev); s_dev = NULL; }
    spi_bus_free(BIG_SPI_HOST);
    for (int p = 9; p <= 16; p++) if (pin_allowed(p)) gpio_reset_pin(p);

    s_pin_mosi = next[0]; s_pin_sck = next[1]; s_pin_cs  = next[2];
    s_pin_dc   = next[3]; s_pin_rst = next[4]; s_pin_bl  = next[5];
    int r = tft_big_init();
    xSemaphoreGiveRecursive(s_mux);
    return r;
}

// 纯软件的「这根线上有没有别人在驱动」探针，不用万用表。
// 每个脚测五种状态：高阻、内部上拉、内部下拉、输出写1读回、输出写0读回。
//   上拉=1 且 下拉=0            -> 浮空（排线上没接东西，或对面只是高阻输入）
//   三种输入都一样（全 0 或全1） -> 被外部强驱动，我们和它打架
//   输出写 1 读回 0              -> 被外部拉死（最可疑：推挽输出撞我们的脚）
// 4 寸屏一直白屏（=没收到任何命令）时，最先怀疑 RST 被 GDI 排线上的
// 触摸 BUSY/INT 输出按住，这里能直接量出来。
const char *tft_big_gpio_probe(void)
{
    static const int pins[7] = { 9, 10, 11, 12, 13, 14, 16 };
    static char out[7 * 24];
    out[0] = 0;
    int n = 0;

    if (!s_mux) s_mux = xSemaphoreCreateRecursiveMutex();
    xSemaphoreTakeRecursive(s_mux, portMAX_DELAY);
    s_ready = false;
    if (s_dev) { spi_bus_remove_device(s_dev); s_dev = NULL; }
    spi_bus_free(BIG_SPI_HOST);

    for (int i = 0; i < 7; i++) {
        int p = pins[i];
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << p,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_reset_pin(p); gpio_config(&io); vTaskDelay(1);
        int hi_z = gpio_get_level(p);

        gpio_reset_pin(p); io.pull_up_en = GPIO_PULLUP_ENABLE; gpio_config(&io); vTaskDelay(1);
        int pullup = gpio_get_level(p);

        gpio_reset_pin(p); io.pull_up_en = GPIO_PULLUP_DISABLE;
        io.pull_down_en = GPIO_PULLDOWN_ENABLE; gpio_config(&io); vTaskDelay(1);
        int pulldn = gpio_get_level(p);

        gpio_reset_pin(p); io.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io.mode = GPIO_MODE_INPUT_OUTPUT; gpio_config(&io);
        gpio_set_level(p, 1); vTaskDelay(1); int w1 = gpio_get_level(p);
        gpio_set_level(p, 0); vTaskDelay(1); int w0 = gpio_get_level(p);
        gpio_reset_pin(p);

        n += snprintf(out + n, sizeof(out) - n, "%d:%d%d%d/%d%d ",
                      p, hi_z, pullup, pulldn, w1, w0);
        ESP_LOGI(TAG, "GPIO%d  高阻=%d 上拉=%d 下拉=%d | 写1读回=%d 写0读回=%d",
                 p, hi_z, pullup, pulldn, w1, w0);
    }

    tft_big_init();
    xSemaphoreGiveRecursive(s_mux);
    return out;
}
