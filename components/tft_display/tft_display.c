#include "tft_display.h"
#include "font_cn16.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "TFT";
static SemaphoreHandle_t s_tft_mux;   // SPI 总线并发保护

// ========== FireBeetle 2 ESP32-S3 GDI 引脚定义（官方 wiki 映射） ==========
#define PIN_LCD_MOSI    15   // GDI MOSI
#define PIN_LCD_SCK     17   // GDI SCLK
#define PIN_LCD_CS      18   // LCD_CS  (D6)
#define PIN_LCD_DC      3    // LCD_DC  (D2)
#define PIN_LCD_RST     38   // LCD_RST (D3)
#define PIN_LCD_BL      21   // LCD_BL  (D13) 背光

#define LCD_WIDTH       320
#define LCD_HEIGHT      240
#define SPI_HOST_ID     SPI2_HOST
#define SPI_FREQ_HZ     (40 * 1000 * 1000)

// ST7789 命令
#define CMD_SWRESET     0x01
#define CMD_SLPOUT      0x11
#define CMD_NORON       0x13
#define CMD_INVON       0x21
#define CMD_DISPON      0x29
#define CMD_CASET       0x2A
#define CMD_RASET       0x2B
#define CMD_RAMWR       0x2C
#define CMD_COLMOD      0x3A
#define CMD_MADCTL      0x36

static spi_device_handle_t s_spi_dev;

// ========== SPI 底层 ==========

static void lcd_cmd(uint8_t cmd)
{
    gpio_set_level(PIN_LCD_DC, 0);
    spi_transaction_t t = { .length = 8, .tx_buffer = &cmd };
    spi_device_transmit(s_spi_dev, &t);
}

static void lcd_data(const uint8_t *data, size_t len)
{
    if (len == 0) return;
    gpio_set_level(PIN_LCD_DC, 1);
    spi_transaction_t t = { .length = len * 8, .tx_buffer = data };
    spi_device_transmit(s_spi_dev, &t);
}

static void lcd_data_byte(uint8_t val) { lcd_data(&val, 1); }

static void lcd_set_window(int x0, int y0, int x1, int y1)
{
    uint8_t ca[4] = { x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF };
    uint8_t ra[4] = { y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF };
    lcd_cmd(CMD_CASET); lcd_data(ca, 4);
    lcd_cmd(CMD_RASET); lcd_data(ra, 4);
    lcd_cmd(CMD_RAMWR);
}

static void lcd_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > LCD_WIDTH)  w = LCD_WIDTH - x;
    if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    lcd_set_window(x, y, x + w - 1, y + h - 1);

    uint8_t hi = color >> 8, lo = color & 0xFF;
    uint8_t line_buf[256];
    int chunk = w <= 128 ? w : 128;
    for (int i = 0; i < chunk; i++) { line_buf[i * 2] = hi; line_buf[i * 2 + 1] = lo; }

    gpio_set_level(PIN_LCD_DC, 1);
    int remaining = w * h;
    while (remaining > 0) {
        int pixels = chunk;
        if (remaining < pixels) pixels = remaining;
        spi_transaction_t t = { .length = pixels * 16, .tx_buffer = line_buf };
        spi_device_transmit(s_spi_dev, &t);
        remaining -= pixels;
    }
}

// ========== 字体渲染 ==========

// 5x7 ASCII 点阵（0x20-0x7E）
static const uint8_t font5x7[] = {
    0x00,0x00,0x00,0x00,0x00, // space
    0x00,0x00,0x5F,0x00,0x00, // !
    0x00,0x07,0x00,0x07,0x00, // "
    0x14,0x7F,0x14,0x7F,0x14, // #
    0x24,0x2A,0x7F,0x2A,0x12, // $
    0x23,0x13,0x08,0x64,0x62, // %
    0x36,0x49,0x55,0x22,0x50, // &
    0x00,0x05,0x03,0x00,0x00, // '
    0x00,0x1C,0x22,0x41,0x00, // (
    0x00,0x41,0x22,0x1C,0x00, // )
    0x14,0x08,0x3E,0x08,0x14, // *
    0x08,0x08,0x3E,0x08,0x08, // +
    0x00,0x50,0x30,0x00,0x00, // ,
    0x08,0x08,0x08,0x08,0x08, // -
    0x00,0x60,0x60,0x00,0x00, // .
    0x20,0x10,0x08,0x04,0x02, // /
    0x3E,0x51,0x49,0x45,0x3E, // 0
    0x00,0x42,0x7F,0x40,0x00, // 1
    0x42,0x61,0x51,0x49,0x46, // 2
    0x21,0x41,0x45,0x4B,0x31, // 3
    0x18,0x14,0x12,0x7F,0x10, // 4
    0x27,0x45,0x45,0x45,0x39, // 5
    0x3C,0x4A,0x49,0x49,0x30, // 6
    0x01,0x71,0x09,0x05,0x03, // 7
    0x36,0x49,0x49,0x49,0x36, // 8
    0x06,0x49,0x49,0x29,0x1E, // 9
    0x00,0x36,0x36,0x00,0x00, // :
    0x00,0x56,0x36,0x00,0x00, // ;
    0x08,0x14,0x22,0x41,0x00, // <
    0x14,0x14,0x14,0x14,0x14, // =
    0x00,0x41,0x22,0x14,0x08, // >
    0x02,0x01,0x51,0x09,0x06, // ?
    0x32,0x49,0x79,0x41,0x3E, // @
    0x7E,0x11,0x11,0x11,0x7E, // A
    0x7F,0x49,0x49,0x49,0x36, // B
    0x3E,0x41,0x41,0x41,0x22, // C
    0x7F,0x41,0x41,0x22,0x1C, // D
    0x7F,0x49,0x49,0x49,0x41, // E
    0x7F,0x09,0x09,0x09,0x01, // F
    0x3E,0x41,0x49,0x49,0x7A, // G
    0x7F,0x08,0x08,0x08,0x7F, // H
    0x00,0x41,0x7F,0x41,0x00, // I
    0x20,0x40,0x41,0x3F,0x01, // J
    0x7F,0x08,0x14,0x22,0x41, // K
    0x7F,0x40,0x40,0x40,0x40, // L
    0x7F,0x02,0x0C,0x02,0x7F, // M
    0x7F,0x04,0x08,0x10,0x7F, // N
    0x3E,0x41,0x41,0x41,0x3E, // O
    0x7F,0x09,0x09,0x09,0x06, // P
    0x3E,0x41,0x51,0x21,0x5E, // Q
    0x7F,0x09,0x19,0x29,0x46, // R
    0x46,0x49,0x49,0x49,0x31, // S
    0x01,0x01,0x7F,0x01,0x01, // T
    0x3F,0x40,0x40,0x20,0x1F, // U
    0x1F,0x20,0x40,0x20,0x1F, // V
    0x3F,0x40,0x38,0x40,0x3F, // W
    0x63,0x14,0x08,0x14,0x63, // X
    0x07,0x08,0x70,0x08,0x07, // Y
    0x61,0x51,0x49,0x45,0x43, // Z
    0x00,0x7F,0x41,0x41,0x00, // [
    0x02,0x04,0x08,0x10,0x20, // backslash
    0x00,0x41,0x41,0x7F,0x00, // ]
    0x04,0x02,0x01,0x02,0x04, // ^
    0x40,0x40,0x40,0x40,0x40, // _
    0x00,0x01,0x02,0x04,0x00, // `
    0x20,0x54,0x54,0x54,0x78, // a
    0x7F,0x48,0x44,0x44,0x38, // b
    0x38,0x44,0x44,0x44,0x20, // c
    0x38,0x44,0x44,0x48,0x7F, // d
    0x38,0x54,0x54,0x54,0x18, // e
    0x08,0x7E,0x09,0x01,0x02, // f
    0x0C,0x52,0x52,0x52,0x3E, // g
    0x7F,0x08,0x04,0x04,0x78, // h
    0x00,0x44,0x7D,0x40,0x00, // i
    0x20,0x40,0x44,0x3D,0x00, // j
    0x7F,0x10,0x28,0x44,0x00, // k
    0x00,0x41,0x7F,0x40,0x00, // l
    0x7C,0x04,0x18,0x04,0x78, // m
    0x7C,0x08,0x04,0x04,0x78, // n
    0x38,0x44,0x44,0x44,0x38, // o
    0x7C,0x14,0x14,0x14,0x08, // p
    0x08,0x14,0x14,0x18,0x7C, // q
    0x7C,0x08,0x04,0x04,0x08, // r
    0x48,0x54,0x54,0x54,0x20, // s
    0x04,0x3F,0x44,0x40,0x20, // t
    0x3C,0x40,0x40,0x20,0x1C, // u
    0x1C,0x20,0x40,0x20,0x1C, // v
    0x3C,0x40,0x30,0x40,0x3C, // w
    0x44,0x28,0x10,0x28,0x44, // x
    0x0C,0x50,0x50,0x50,0x3C, // y
    0x44,0x64,0x54,0x4C,0x44, // z
    0x00,0x08,0x36,0x41,0x00, // {
    0x00,0x00,0x7F,0x00,0x00, // |
    0x00,0x41,0x36,0x08,0x00, // }
    0x10,0x08,0x08,0x10,0x08, // ~
};

// 16x16 汉字点阵绘制（scale=1 时 16px 见方）
static void draw_glyph16(int x, int y, const uint8_t *bmp, uint16_t fg, uint16_t bg, int scale)
{
    for (int row = 0; row < 16; row++) {
        uint16_t bits = (bmp[row * 2] << 8) | bmp[row * 2 + 1];
        for (int col = 0; col < 16; col++) {
            uint16_t color = (bits & (0x8000 >> col)) ? fg : bg;
            lcd_fill_rect(x + col * scale, y + row * scale, scale, scale, color);
        }
    }
}

// ASCII 5x7 绘制（垂直居中于 16*scale 行高）
static void draw_ascii(int x, int y, char c, uint16_t fg, uint16_t bg, int scale)
{
    if (c < 0x20 || c > 0x7E) c = '?';
    const uint8_t *glyph = &font5x7[(c - 0x20) * 5];
    int y_off = (16 - 7) / 2 * scale; // 居中
    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; row++) {
            uint16_t color = (bits & (1 << row)) ? fg : bg;
            lcd_fill_rect(x + col * scale, y + y_off + row * scale, scale, scale, color);
        }
    }
}

// UTF-8 解码（前进一个码点）
static uint32_t utf8_decode(const char *s, int *len)
{
    unsigned char c0 = (unsigned char)s[0];
    if (c0 < 0x80) { *len = 1; return c0; }
    if ((c0 & 0xE0) == 0xC0 && (unsigned char)s[1] >= 0x80) {
        *len = 2; return ((c0 & 0x1F) << 6) | (s[1] & 0x3F);
    }
    if ((c0 & 0xF0) == 0xE0) {
        *len = 3; return ((c0 & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    }
    if ((c0 & 0xF8) == 0xF0) {
        *len = 4; return ((c0 & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
    }
    *len = 1; return '?';
}

static int char_advance(uint32_t cp, int scale)
{
    if (cp < 0x80) return 6 * scale;      // ASCII
    if (cp >= 0x2E80) return 16 * scale;  // CJK 及中文标点
    return 6 * scale;
}

// 测量 UTF-8 字符串像素宽
static int text_width(const char *s, int scale)
{
    int w = 0, len;
    while (*s) {
        uint32_t cp = utf8_decode(s, &len);
        w += char_advance(cp, scale);
        s += len;
    }
    return w;
}

// 渲染一行（不裁剪），返回结束 x
static int render_line(int x, int y, const char *start, const char *end,
                       uint16_t fg, uint16_t bg, int scale)
{
    uint8_t bmp[32];
    while (start < end && *start) {
        int len;
        uint32_t cp = utf8_decode(start, &len);
        int adv = char_advance(cp, scale);
        if (cp < 0x80) {
            draw_ascii(x, y, (char)cp, fg, bg, scale);
        } else if (font_cn16_lookup(cp, bmp)) {
            draw_glyph16(x, y, bmp, fg, bg, scale);
        } else {
            // 无字库时画占位方框
            lcd_fill_rect(x + scale, y + scale, 14 * scale, 14 * scale, bg);
            lcd_fill_rect(x + scale, y + scale, 14 * scale, scale, fg);
            lcd_fill_rect(x + scale, y + 15 * scale - scale, 14 * scale, scale, fg);
        }
        x += adv;
        start += len;
    }
    return x;
}

// 自动换行渲染整段文字，返回下一可用 y
static int render_text_wrapped(int x, int y, int max_width, const char *text,
                               uint16_t fg, uint16_t bg, int scale)
{
    int line_h = 16 * scale + 2;
    const char *p = text;
    while (*p) {
        // 找到本行能放下的最大字节数
        int w = 0, len;
        const char *line_end = p;
        while (*line_end) {
            uint32_t cp = utf8_decode(line_end, &len);
            int adv = char_advance(cp, scale);
            if (w + adv > max_width) break;
            w += adv;
            line_end += len;
        }
        if (line_end == p) { // 单字符超宽，强放一个
            utf8_decode(p, &len);
            line_end = p + len;
        }
        render_line(x, y, p, line_end, fg, bg, scale);
        y += line_h;
        p = line_end;
    }
    return y;
}

// ========== 公共 API ==========

int tft_display_init(void)
{
    ESP_LOGI(TAG, "Initializing ST7789 via GDI...");
    s_tft_mux = xSemaphoreCreateRecursiveMutex();

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_LCD_DC) | (1ULL << PIN_LCD_RST) | (1ULL << PIN_LCD_BL),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_conf);
    gpio_set_level(PIN_LCD_BL, 0);

    gpio_set_level(PIN_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_LCD_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t ret = spi_bus_initialize(SPI_HOST_ID, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %d", ret);
        return -1;
    }

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = SPI_FREQ_HZ,
        .mode = 0,
        .spics_io_num = PIN_LCD_CS,
        .queue_size = 7,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    ret = spi_bus_add_device(SPI_HOST_ID, &dev_cfg, &s_spi_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI add device failed: %d", ret);
        return -1;
    }

    lcd_cmd(CMD_SWRESET); vTaskDelay(pdMS_TO_TICKS(150));
    lcd_cmd(CMD_SLPOUT);  vTaskDelay(pdMS_TO_TICKS(120));
    lcd_cmd(CMD_COLMOD);  lcd_data_byte(0x55);
    lcd_cmd(CMD_MADCTL);  lcd_data_byte(0xA0);   // MY|MV：320x240 横向显示（上下翻转修正）
    lcd_cmd(CMD_INVON);
    lcd_cmd(CMD_NORON);   vTaskDelay(pdMS_TO_TICKS(10));
    lcd_cmd(CMD_DISPON);  vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(PIN_LCD_BL, 1);

    tft_clear(COLOR_BG);
    ESP_LOGI(TAG, "ST7789 ready (%dx%d)", LCD_WIDTH, LCD_HEIGHT);
    return 0;
}

void tft_clear(uint16_t color)
{
    xSemaphoreTakeRecursive(s_tft_mux, portMAX_DELAY);
    lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, color);
    xSemaphoreGiveRecursive(s_tft_mux);
}

// 计算按换行规则需要占多少行
static int wrap_line_count(const char *s, int max_width, int scale)
{
    if (!*s) return 0;
    int lines = 1, w = 0, len;
    while (*s) {
        uint32_t cp = utf8_decode(s, &len);
        int adv = char_advance(cp, scale);
        if (w + adv > max_width) {   // 本字符换到新行
            lines++;
            w = adv;
        } else {
            w += adv;
        }
        s += len;
    }
    return lines;
}

// ===== 四横带布局（占屏高度：头部20% 题干40% 选项30% 状态10%）=====
#define BAND_HDR_Y   0
#define BAND_HDR_H   48                     // 20%
#define BAND_Q_Y     48
#define BAND_Q_H     96                     // 40%
#define BAND_OPT_Y   144
#define BAND_OPT_H   72                     // 30%
#define BAND_ST_Y    216
#define BAND_ST_H    24                     // 10%

static const uint16_t BG_HDR    = RGB565C(30, 30, 52);
static const uint16_t BG_Q      = RGB565C(24, 24, 40);
static const uint16_t BG_OPT    = RGB565C(18, 18, 30);
static const uint16_t BG_ST     = RGB565C(10, 10, 18);
static const uint16_t BG_SEP    = RGB565C(70, 70, 95);
static const uint16_t BG_OK_ROW  = RGB565C(22, 92, 42);
static const uint16_t BG_BAD_ROW = RGB565C(112, 26, 32);

// 头部带：学科+题号(左)、难度(右)
static void render_header(const quiz_problem_t *problem)
{
    lcd_fill_rect(0, BAND_HDR_Y, LCD_WIDTH, BAND_HDR_H, BG_HDR);
    lcd_fill_rect(0, BAND_HDR_Y + BAND_HDR_H - 1, LCD_WIDTH, 1, BG_SEP);

    char tag[48];
    snprintf(tag, sizeof(tag), "[%s] #%u", quiz_subject_name(problem->subject), (unsigned)problem->id);
    render_line(8, BAND_HDR_Y + 16, tag, tag + strlen(tag), COLOR_TAG, BG_HDR, 1);

    static const char *dn[] = {"简单", "中等", "困难"};
    const char *diff = problem->difficulty < DIFFICULTY_MAX ? dn[problem->difficulty] : "";
    uint16_t dcol = (problem->difficulty == DIFFICULTY_EASY) ? COLOR_OK :
                    (problem->difficulty == DIFFICULTY_HARD) ? COLOR_BAD : COLOR_SEL;
    int dw = text_width(diff, 1);
    render_line(LCD_WIDTH - dw - 8, BAND_HDR_Y + 16, diff, diff + strlen(diff), dcol, BG_HDR, 1);
}

// 题干带：统一 1 倍字号（16px），保证完整显示；垂直居中
static void render_question_band(const quiz_problem_t *problem)
{
    lcd_fill_rect(0, BAND_Q_Y, LCD_WIDTH, BAND_Q_H, BG_Q);
    lcd_fill_rect(0, BAND_Q_Y + BAND_Q_H - 1, LCD_WIDTH, 1, BG_SEP);

    const int scale = 1;
    int lines = wrap_line_count(problem->question, LCD_WIDTH - 16, scale);
    int used = lines * (16 * scale + 2);
    int top = BAND_Q_Y + (BAND_Q_H - used) / 2;
    if (top < BAND_Q_Y + 2) top = BAND_Q_Y + 2;
    render_text_wrapped(8, top, LCD_WIDTH - 16, problem->question, COLOR_TEXT, BG_Q, scale);
}

// 选项带：一行 4 列等宽格子；序号居上，选项文字居中换行（最多 2 行）
static void render_options_band(const quiz_problem_t *problem, bool hl_correct, int wrong_idx)
{
    int n = problem->option_count;
    if (n > QUIZ_MAX_OPTIONS) n = QUIZ_MAX_OPTIONS;
    if (n <= 0) { lcd_fill_rect(0, BAND_OPT_Y, LCD_WIDTH, BAND_OPT_H, BG_OPT); return; }

    int cell_w = LCD_WIDTH / QUIZ_MAX_OPTIONS;          // 80px
    int total_w = cell_w * n;
    int x0 = (LCD_WIDTH - total_w) / 2;                 // 整组水平居中
    int cy = BAND_OPT_Y;

    for (int i = 0; i < n; i++) {
        int cx = x0 + i * cell_w;

        uint16_t bg = BG_OPT;
        if (hl_correct && i == (int)problem->correct_index) bg = BG_OK_ROW;
        if (i == wrong_idx) bg = BG_BAD_ROW;

        lcd_fill_rect(cx, cy, cell_w, BAND_OPT_H, bg);
        if (i < n - 1)
            lcd_fill_rect(cx + cell_w - 1, cy, 1, BAND_OPT_H, RGB565C(40, 40, 60));

        // 序号：顶部居中
        char letter = 'A' + i;
        render_line(cx + (cell_w - 6) / 2, cy + 4, &letter, &letter + 1, COLOR_SEL, bg, 1);

        // 选项文字：逐行居中，最多 2 行
        const char *s = problem->options[i];
        int y = cy + 24;
        int text_w = cell_w - 6;
        for (int ln = 0; ln < 2 && *s; ln++) {
            const char *e = s;
            int w = 0, len;
            while (*e) {
                uint32_t cp = utf8_decode(e, &len);
                int adv = char_advance(cp, 1);
                if (w + adv > text_w) break;
                w += adv; e += len;
            }
            if (e == s) { utf8_decode(s, &len); e = s + len; w = char_advance(utf8_decode(s, &len), 1); }
            render_line(cx + (cell_w - w) / 2, y, s, e, COLOR_TEXT, bg, 1);
            y += 18;
            s = e;
        }
    }
}

// 状态带：题库计数 / 反馈文字
static void render_status_band(const char *text, uint16_t col)
{
    lcd_fill_rect(0, BAND_ST_Y, LCD_WIDTH, BAND_ST_H, BG_ST);
    if (!text || !*text) return;
    int w = text_width(text, 1);
    render_line((LCD_WIDTH - w) / 2, BAND_ST_Y + (BAND_ST_H - 16) / 2,
                text, text + strlen(text), col, BG_ST, 1);
}

static void show_quiz_impl(const quiz_problem_t *problem, const char *mode_line)
{
    render_header(problem);
    render_question_band(problem);
    render_options_band(problem, false, -1);
    render_status_band(mode_line, COLOR_HINT);
}

void tft_show_quiz(const quiz_problem_t *problem, const char *mode_line)
{
    if (!problem) return;
    xSemaphoreTakeRecursive(s_tft_mux, portMAX_DELAY);
    show_quiz_impl(problem, mode_line);
    xSemaphoreGiveRecursive(s_tft_mux);
}

void tft_show_feedback(const quiz_problem_t *problem, int8_t chosen)
{
    if (!problem) return;
    xSemaphoreTakeRecursive(s_tft_mux, portMAX_DELAY);
    // 重绘题目后标记对错
    show_quiz_impl(problem, NULL);
    bool right = (chosen == (int8_t)problem->correct_index);
    render_options_band(problem, right,
                        (chosen >= 0 && !right) ? (int)chosen : -1);

    char status[64];
    if (chosen < 0)
        snprintf(status, sizeof(status), "未作答");
    else if (right)
        snprintf(status, sizeof(status), "回答正确!");
    else
        snprintf(status, sizeof(status), "答错了, 正确: %c", 'A' + problem->correct_index);

    render_status_band(status, right ? COLOR_OK : COLOR_BAD);
    xSemaphoreGiveRecursive(s_tft_mux);
}

void tft_show_status(const char *title, const char *detail)
{
    xSemaphoreTakeRecursive(s_tft_mux, portMAX_DELAY);
    tft_clear(COLOR_BG);
    if (title) {
        int w = text_width(title, 2);
        render_line((LCD_WIDTH - w) / 2, 90, title, title + strlen(title), COLOR_TEXT, COLOR_BG, 2);
    }
    if (detail)
        render_text_wrapped(10, 140, LCD_WIDTH - 20, detail, COLOR_HINT, COLOR_BG, 1);
    xSemaphoreGiveRecursive(s_tft_mux);
}

void tft_show_analysis(const char *summary, const char *weak_points,
                       const char *strengths, const char *suggestion)
{
    xSemaphoreTakeRecursive(s_tft_mux, portMAX_DELAY);
    tft_clear(COLOR_BG);
    const char *t = "学习分析报告";
    render_line(6, 4, t, t + strlen(t), COLOR_TAG, COLOR_BG, 1);
    lcd_fill_rect(0, 22, LCD_WIDTH, 1, COLOR_HINT);

    int y = 28;
    static const uint16_t colors[] = {COLOR_TEXT, COLOR_BAD, COLOR_OK, COLOR_SEL};
    const char *labels[] = {"总评: ", "薄弱: ", "优势: ", "建议: "};
    const char *texts[] = {summary, weak_points, strengths, suggestion};
    for (int i = 0; i < 4; i++) {
        if (!texts[i] || !texts[i][0]) continue;
        int lw = text_width(labels[i], 1);
        render_line(6, y, labels[i], labels[i] + strlen(labels[i]), colors[i], COLOR_BG, 1);
        y = render_text_wrapped(6 + lw, y, LCD_WIDTH - 12, texts[i], colors[i], COLOR_BG, 1);
        y += 2;
    }
    xSemaphoreGiveRecursive(s_tft_mux);
}

// ========== 物体识别结果画面 ==========

#define OBJ_BODY_Y    22                      // 画面顶部标题栏高度
#define OBJ_BODY_H    (LCD_HEIGHT - OBJ_BODY_Y - 16) // 底部留 16px 状态行

static const uint16_t s_box_colors[] = {
    RGB565C(255, 80, 80), RGB565C(80, 220, 120), RGB565C(90, 160, 255),
    RGB565C(255, 200, 80), RGB565C(200, 120, 255), RGB565C(80, 220, 220),
    RGB565C(255, 140, 60), RGB565C(180, 220, 90),
};
#define N_BOX_COLORS (sizeof(s_box_colors) / sizeof(s_box_colors[0]))

static void draw_box_rect(int x, int y, int w, int h, uint16_t col)
{
    lcd_fill_rect(x, y, w, 2, col);            // 上
    lcd_fill_rect(x, y + h - 2, w, 2, col);    // 下
    lcd_fill_rect(x, y, 2, h, col);            // 左
    lcd_fill_rect(x + w - 2, y, 2, h, col);    // 右
}

// 在整屏画面（或已推流视频）上绘制检测框+标签。objs 坐标为屏幕坐标。
// y_min: 允许的最高绘制位置；底部 16px 留给状态行。
#define TFT_MAX_OBJS 8
static void draw_overlay(const tft_object_t *objs, int count, int y_min)
{
    if (count > TFT_MAX_OBJS) count = TFT_MAX_OBJS;
    for (int i = 0; i < count; i++) {
        const tft_object_t *o = &objs[i];
        uint16_t col = s_box_colors[i % N_BOX_COLORS];

        int x = o->x, y = o->y, w = o->w, h = o->h;
        if (x < 0) { w += x; x = 0; }
        if (y < y_min) { h += y - y_min; y = y_min; }
        if (x + w > LCD_WIDTH) w = LCD_WIDTH - x;
        if (y + h > LCD_HEIGHT - 16) h = LCD_HEIGHT - 16 - y;
        if (w < 6) w = 6;
        if (h < 6) h = 6;
        draw_box_rect(x, y, w, h, col);

        // 标签块：类别 + 置信度
        char chip[48];
        snprintf(chip, sizeof(chip), "%s %d%%", o->label, o->percent);
        int cw = text_width(chip, 1) + 6;
        if (cw > LCD_WIDTH - 2) cw = LCD_WIDTH - 2;
        int cy = y - 18;
        if (cy < y_min) cy = y + 2;
        lcd_fill_rect(x, cy, cw, 18, col);
        render_line(x + 3, cy + 1, chip, chip + strlen(chip), COLOR_BLACK, col, 1);
    }
}

static void draw_status_strip(const char *status_line)
{
    if (!status_line) return;
    lcd_fill_rect(0, LCD_HEIGHT - 16, LCD_WIDTH, 16, BG_ST);
    render_line(6, LCD_HEIGHT - 15, status_line,
                status_line + strlen(status_line), COLOR_HINT, BG_ST, 1);
}

void tft_show_objects(const tft_object_t *objs, int count, const char *status_line)
{
    xSemaphoreTakeRecursive(s_tft_mux, portMAX_DELAY);
    tft_clear(COLOR_BG);

    // 标题栏
    lcd_fill_rect(0, 0, LCD_WIDTH, OBJ_BODY_Y, BG_HDR);
    const char *t = "端侧物体识别";
    render_line(6, 3, t, t + strlen(t), COLOR_TAG, BG_HDR, 1);

    if (count <= 0 || !objs) {
        const char *e = "未检测到物体";
        render_line((LCD_WIDTH - text_width(e, 2)) / 2,
                    OBJ_BODY_Y + (OBJ_BODY_H - 32) / 2, e, e + strlen(e),
                    COLOR_HINT, COLOR_BG, 2);
    } else {
        // 输入坐标为画面区(320 x OBJ_BODY_H)空间，整体下移 22px
        static tft_object_t shifted[TFT_MAX_OBJS];
        int n = count > TFT_MAX_OBJS ? TFT_MAX_OBJS : count;
        for (int i = 0; i < n; i++) {
            shifted[i] = objs[i];
            shifted[i].y += OBJ_BODY_Y;
        }
        draw_overlay(shifted, n, OBJ_BODY_Y);
    }

    draw_status_strip(status_line);
    xSemaphoreGiveRecursive(s_tft_mux);
}

// ========== 实时推流 + 检测框叠加 ==========

static uint8_t s_frame_chunk[4096];   // SPI 中转缓冲（DMA 要求内部 RAM，max_transfer_sz=4096）

void tft_render_view(const uint8_t *frame_be565, const tft_object_t *objs,
                     int count, const char *status_line)
{
    xSemaphoreTakeRecursive(s_tft_mux, portMAX_DELAY);

    if (frame_be565) {
        lcd_set_window(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
        gpio_set_level(PIN_LCD_DC, 1);
        int total = LCD_WIDTH * LCD_HEIGHT * 2;
        int off = 0;
        while (off < total) {
            int n = total - off;
            if (n > (int)sizeof(s_frame_chunk)) n = sizeof(s_frame_chunk);
            memcpy(s_frame_chunk, frame_be565 + off, n);
            spi_transaction_t t = { .length = n * 8, .tx_buffer = s_frame_chunk };
            spi_device_transmit(s_spi_dev, &t);
            off += n;
        }
    } else {
        tft_clear(COLOR_BG);
    }

    if (objs && count > 0) draw_overlay(objs, count, 0);
    draw_status_strip(status_line);
    xSemaphoreGiveRecursive(s_tft_mux);
}
