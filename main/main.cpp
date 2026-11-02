// ============================================================
// ESP32-S3 端侧摄像头物品识别 (FireBeetle 2 ESP32-S3 + OV3660)
// 流程: AXP313A 摄像头供电 -> DVP 采集 QVGA RGB888
//       -> ESP-DL MobileNetV2 INT8 (127 类常见物品, 拍屏闭集识别) 本地推理 -> TFT 显示
// 出题机相关组件保留在仓库中，不再由本程序调用。
// ============================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "driver/i2c.h"
#include "esp_camera.h"
#include "img_converters.h"
#include "axp313a.h"
#include "tft_display.h"
#include "tft_big.h"
#include "quiz_photos.h"
#include "items103.hpp"
#include "sdkconfig.h"
#include "wifi_manager.h"
#include "esp_http_server.h"

static const char *TAG = "CAMDET";

// ===== FireBeetle 2 ESP32-S3 (V1.1) CAM DVP 引脚（官方 wiki CAM 接口表）=====
#define CAM_PIN_XCLK   45
#define CAM_PIN_PCLK    5
#define CAM_PIN_VSYNC   6
#define CAM_PIN_HREF   42
// 8bit 总线使用传感器 D2-D9: config.pin_d0..d7 依次对应 D2..D9
#define CAM_DATA_D2    39   // pin_d0
#define CAM_DATA_D3    40   // pin_d1
#define CAM_DATA_D4    41   // pin_d2
#define CAM_DATA_D5     4   // pin_d3
#define CAM_DATA_D6     7   // pin_d4
#define CAM_DATA_D7     8   // pin_d5
#define CAM_DATA_D8    46   // pin_d6
#define CAM_DATA_D9    48   // pin_d7
// I2C: AXP313A 与摄像头 SCCB 共用一组引脚 (SDA=1, SCL=2)
#define I2C_SDA_PIN     1
#define I2C_SCL_PIN     2
#define I2C_PORT        I2C_NUM_0

#define DET_MAX_OBJS    8    // 屏幕最多展示的目标数
#define SCORE_THR       0.25f   // 分类置信度低于此值显示"不确定"

// ---------- 共用 I2C 总线（AXP313A + 摄像头 SCCB）----------
static esp_err_t shared_i2c_init(void)
{
    i2c_config_t conf = {};
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = I2C_SDA_PIN;
    conf.scl_io_num = I2C_SCL_PIN;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = 100000;
    esp_err_t ret = i2c_param_config(I2C_PORT, &conf);
    if (ret == ESP_OK) ret = i2c_driver_install(I2C_PORT, conf.mode, 0, 0, 0);
    return ret;
}

// 总线扫描：打印所有应答的 7 位地址（诊断用）
static void i2c_scan(void)
{
    ESP_LOGI(TAG, "I2C scan:");
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true); // 检查ACK
        i2c_master_stop(cmd);
        esp_err_t ret = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(100));
        i2c_cmd_link_delete(cmd);
        if (ret == ESP_OK) ESP_LOGI(TAG, "  found device @0x%02X", addr);
    }
}

// ---------- 摄像头初始化 ----------
static esp_err_t camera_init(void)
{
    camera_config_t config = {};
    config.pin_pwdn     = -1;   // PWDN 已下拉
    config.pin_reset    = -1;   // RST 已上拉到 DOVDD
    config.pin_xclk     = CAM_PIN_XCLK;
    config.pin_sccb_sda = -1;   // 复用已装好的 I2C 端口
    config.pin_sccb_scl = -1;
    config.sccb_i2c_port = I2C_PORT;

    config.pin_d7 = CAM_DATA_D9;
    config.pin_d6 = CAM_DATA_D8;
    config.pin_d5 = CAM_DATA_D7;
    config.pin_d4 = CAM_DATA_D6;
    config.pin_d3 = CAM_DATA_D5;
    config.pin_d2 = CAM_DATA_D4;
    config.pin_d1 = CAM_DATA_D3;
    config.pin_d0 = CAM_DATA_D2;

    config.pin_vsync = CAM_PIN_VSYNC;
    config.pin_href  = CAM_PIN_HREF;
    config.pin_pclk  = CAM_PIN_PCLK;

    config.xclk_freq_hz = 20000000;
    config.ledc_timer   = LEDC_TIMER_0;
    config.ledc_channel = LEDC_CHANNEL_0;

    config.pixel_format = PIXFORMAT_YUV422;   // S3 DVP 不支持 RGB888 直出，采集后转 RGB888
    config.frame_size   = FRAMESIZE_QVGA;     // 320x240, 与屏幕 1:1
    config.jpeg_quality = 12;
    config.fb_count     = 2;
    config.fb_location  = CAMERA_FB_IN_PSRAM;
    config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s", esp_err_to_name(err));
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    ESP_LOGI(TAG, "Camera sensor PID=0x%04x (%s)", s ? (unsigned)s->id.PID : 0u,
             (s && s->id.PID == (uint16_t)OV3660_PID) ? "OV3660" : "other");
    if (s) {
        s->set_vflip(s, 1);      // 摄像头模组安装方向：上下翻转
        s->set_brightness(s, 1); // 适当提亮
    }
    return ESP_OK;
}

// ---------- 屏幕区定位 + 点测光 + 只把屏幕喂给模型 ----------
// 两个问题一起解决：
//  1) OV3660 的 AEC 按整帧平均测光，而屏只占画面 15% 左右，测光全被桌面带走 →
//     屏削顶成一片白，出题图的细节（浅色物体、白底）全被抹平。改成只量屏幕区，
//     把它的 p90（最亮那 10% 的分界）钉在 target 上 = 「图里最亮的部分刚好不削顶」。
//     桌面因此变暗反而是好事：杂散光和眩光都少了。
//  2) 屏只占 120x100 px 时，模型拿整帧去分类，看到的其实是「桌子」而不是出题图
//     （实测：屏上是玉米，模型 82% 认成台灯）。所以把屏幕区裁出来单独喂给模型。
// 开关/参数都走 /cam，调完不用重烧。
typedef struct {
    int x0, y0, x1, y1;   // 百分比 0..100
    bool ok;
} box_pct_t;

typedef struct {
    bool on;              // 点测光总开关（关掉就把自动曝光/增益还给传感器）
    bool adetect;         // 1=每帧自动找屏；0=用 roi= 手动钉死的框
    int  target;          // 目标 p90 亮度
    int  aecv;            // 控制器自己维护的手动曝光行数
} spot_t;

static spot_t    s_spot  = { false, true, 200, 120 };
static box_pct_t s_panel = { 19, 33, 56, 75, false };   // 屏幕区；没测出来前用手动值兜底
static bool      s_crop  = false;                       // 1=只把屏幕区喂给模型
static uint16_t  s_spot_hist[256];
static const int AECV_MAX = 783;      // = 0x380e 读到的 VTS，超过它驱动会静默钳位

// 自动找屏的「投票 + 锁定」：一帧的亮度分布说了不算（出题图换一张，暗图会让亮块
// 缩到屏的一角，甚至整块框跳到桌面的白墙纸上），所以要求连续几帧量到同一个框才采纳，
// 采纳后锁定 N 次推理不再改，到点再复查。这样喂给模型的取景框是稳定的，
// 自学习攒下来的样本也才对得上同一套几何。
static const int kAgreeNeed = 3;      // 连续几帧一致才换框
static const int kHoldRuns  = 30;     // 换框后锁多少次推理（一次 ≈4~5 秒）
static const int kAgreeTol  = 3;      // 四条边各允许差几个百分点
static box_pct_t s_cand;              // 正在攒票的候选框
static int       s_cand_n   = 0;
static int       s_hold     = 0;      // >0 = 锁定倒计时

#define BLK_C 16
#define BLK_R 12
static uint8_t s_blk[BLK_R][BLK_C];   // 分块亮度图，找屏和调试都用它
static bool    s_seen[BLK_R][BLK_C];

// 粗框的边落在格子上，离屏的真实边能差一整格（20 px ≈ 屏宽的 16%）。
// 这里在粗框外面再扫一遍像素剖面，把四条边推到「屏内亮度」和「屏外亮度」的中点：
//   屏是自发光亮点，和桌面之间是一条很陡的台阶（实测 3 px 内从 65 跳到 200），
//   所以亮度中点就是屏的物理边，比格子准得多。
// 允许往外长也允许往里收，最多各走 1.5 倍粗框尺寸，免得跑到画面另一头。
static void panel_refine(const uint8_t *rgb, int w, int h,
                         int *px0, int *py0, int *px1, int *py1)
{
    uint32_t in_sum = 0, in_n = 0, out_sum = 0, out_n = 0;
    for (int y = 0; y < h; y += 4) {
        const uint8_t *p = rgb + (size_t)y * w * 3;
        for (int x = 0; x < w; x += 4, p += 12) {
            int lum = (p[0] * 77 + p[1] * 151 + p[2] * 28) >> 8;
            if (x >= *px0 && x < *px1 && y >= *py0 && y < *py1) { in_sum += lum; in_n++; }
            else { out_sum += lum; out_n++; }
        }
    }
    if (!in_n || !out_n) return;
    int in_lvl = (int)(in_sum / in_n), out_lvl = (int)(out_sum / out_n);
    if (in_lvl - out_lvl < 25) return;      // 屏和桌面分不开，剖面判据不可信
    int thr = (in_lvl + out_lvl) / 2;

    // 一行 / 一列在给定跨度内的均值（隔一个像素取一个，够准）
    auto row_lum = [&](int y) -> int {
        uint32_t s = 0; int n = 0;
        const uint8_t *p = rgb + ((size_t)y * w + *px0) * 3;
        for (int x = *px0; x < *px1; x += 2, p += 6) {
            s += (p[0] * 77 + p[1] * 151 + p[2] * 28) >> 8; n++;
        }
        return n ? (int)(s / n) : 0;
    };
    auto col_lum = [&](int x) -> int {
        uint32_t s = 0; int n = 0;
        for (int y = *py0; y < *py1; y += 2) {
            const uint8_t *p = rgb + ((size_t)y * w + x) * 3;
            s += (p[0] * 77 + p[1] * 151 + p[2] * 28) >> 8; n++;
        }
        return n ? (int)(s / n) : 0;
    };

    int lim_y = (*py1 - *py0) * 3 / 2, lim_x = (*px1 - *px0) * 3 / 2;
    int cy = (*py0 + *py1) / 2, cx = (*px0 + *px1) / 2;

    int y0 = *py0, n = 0;
    while (y0 > 0 && row_lum(y0 - 1) >= thr && n++ < lim_y) y0--;
    while (y0 < cy && row_lum(y0) < thr && n++ < lim_y) y0++;
    int y1 = *py1; n = 0;
    while (y1 < h && row_lum(y1) >= thr && n++ < lim_y) y1++;
    while (y1 > cy && row_lum(y1 - 1) < thr && n++ < lim_y) y1--;

    *py0 = y0; *py1 = y1;                   // 先定纵向，横向剖面才只在屏的高度里扫

    int x0 = *px0; n = 0;
    while (x0 > 0 && col_lum(x0 - 1) >= thr && n++ < lim_x) x0--;
    while (x0 < cx && col_lum(x0) < thr && n++ < lim_x) x0++;
    int x1 = *px1; n = 0;
    while (x1 < w && col_lum(x1) >= thr && n++ < lim_x) x1++;
    while (x1 > cx && col_lum(x1 - 1) < thr && n++ < lim_x) x1--;
    *px0 = x0; *px1 = x1;

    ESP_LOGD(TAG, "屏幕框精修: 屏内%d/屏外%d 判据%d -> %d,%d,%d,%d",
             in_lvl, out_lvl, thr, x0, y0, x1, y1);
}

// 屏幕是画面里最亮的一大块：阈值取最亮块均值的 60%（且不低于 60），
// 再在亮块里取最大的连通块当屏幕 —— 用连通块而不是全体亮块包围盒，
// 是因为桌面上随便一个亮点（台灯、白墙）都会把包围盒撑歪。
// 注意这只是「粗框」：16x12 的格子每格 20px，而屏本身才 128x88，
// 一格就是屏宽的 23%，边界必然差一整格。真正的边交给 panel_refine()。
static bool panel_detect(const uint8_t *rgb, int w, int h, box_pct_t *out)
{
    int bw = w / BLK_C, bh = h / BLK_R;
    if (bw < 4 || bh < 4) return false;

    int mx = 0;
    for (int r = 0; r < BLK_R; r++) {
        for (int c = 0; c < BLK_C; c++) {
            int x0 = c * bw, x1 = x0 + bw, y0 = r * bh, y1 = y0 + bh;
            if (x1 > w) x1 = w;
            if (y1 > h) y1 = h;
            uint32_t sum = 0, cnt = 0;
            for (int y = y0; y < y1; y += 2) {
                const uint8_t *p = rgb + ((size_t)y * w + x0) * 3;
                for (int x = x0; x < x1; x += 2, p += 6) {
                    sum += (p[0] * 77 + p[1] * 151 + p[2] * 28) >> 8;   // R,G,B 序
                    cnt++;
                }
            }
            int m = cnt ? (int)(sum / cnt) : 0;
            if (m > 255) m = 255;
            s_blk[r][c] = (uint8_t)m;
            if (m > mx) mx = m;
        }
    }
    int thr = mx * 6 / 10;
    if (thr < 60) thr = 60;

    for (int r = 0; r < BLK_R; r++)
        for (int c = 0; c < BLK_C; c++) s_seen[r][c] = false;

    int best = 0, bc0 = 0, br0 = 0, bc1 = 0, br1 = 0;
    int stk_c[BLK_R * BLK_C], stk_r[BLK_R * BLK_C];
    for (int r = 0; r < BLK_R; r++) {
        for (int c = 0; c < BLK_C; c++) {
            if (s_blk[r][c] < thr || s_seen[r][c]) continue;
            int sp = 0, n = 0, c0 = c, c1 = c, r0 = r, r1 = r;
            stk_c[sp] = c; stk_r[sp++] = r; s_seen[r][c] = true;
            while (sp) {
                int cc = stk_c[--sp], rr = stk_r[sp];
                n++;
                if (cc < c0) c0 = cc;
                if (cc > c1) c1 = cc;
                if (rr < r0) r0 = rr;
                if (rr > r1) r1 = rr;
                const int dc[4] = { 1, -1, 0, 0 }, dr[4] = { 0, 0, 1, -1 };
                for (int k = 0; k < 4; k++) {
                    int nc = cc + dc[k], nr = rr + dr[k];
                    if (nc < 0 || nr < 0 || nc >= BLK_C || nr >= BLK_R) continue;
                    if (s_seen[nr][nc] || s_blk[nr][nc] < thr) continue;
                    s_seen[nr][nc] = true;
                    stk_c[sp] = nc; stk_r[sp++] = nr;
                }
            }
            if (n > best) { best = n; bc0 = c0; bc1 = c1; br0 = r0; br1 = r1; }
        }
    }
    if (best < 4) {
        ESP_LOGW(TAG, "找屏失败: 亮块只有 %d 格（阈值 %d，最亮 %d）", best, thr, mx);
        return false;                                   // 太小，不可信
    }
    if (bc1 - bc0 + 1 >= BLK_C - 1 && br1 - br0 + 1 >= BLK_R - 1) {  // 铺满整帧=没区分度
        ESP_LOGW(TAG, "找屏失败: 亮块铺满整帧（阈值 %d，最亮 %d）", thr, mx);
        return false;
    }

    int px0 = bc0 * bw, px1 = (bc1 + 1) * bw;
    int py0 = br0 * bh, py1 = (br1 + 1) * bh;
    if (px1 > w) px1 = w;
    if (py1 > h) py1 = h;

    panel_refine(rgb, w, h, &px0, &py0, &px1, &py1);

    // 屏是 3:2 的矩形，检测框也摆成 3:2：哪条边量得宽就以哪条边为准，另一条按 1.5 撑开。
    // 宁可不完美也别一会儿扁一会儿方 —— 模型输入几何稳定，自学习攒的样本才自洽。
    {
        int pw = px1 - px0, ph = py1 - py0;
        if (pw * 10 > ph * 15) ph = pw * 10 / 15;
        else                   pw = ph * 15 / 10;
        int cx = (px0 + px1) / 2, cy = (py0 + py1) / 2;
        px0 = cx - pw / 2; py0 = cy - ph / 2; px1 = px0 + pw; py1 = py0 + ph;
    }
    if (px0 < 0) { px1 -= px0; px0 = 0; }
    if (py0 < 0) { py1 -= py0; py0 = 0; }
    if (px1 > w) { px0 -= px1 - w; px1 = w; if (px0 < 0) px0 = 0; }
    if (py1 > h) { py0 -= py1 - h; py1 = h; if (py0 < 0) py0 = 0; }
    if ((px1 - px0) * (py1 - py0) > w * h * 8 / 10) {   // 收不住，别拿它当屏
        ESP_LOGW(TAG, "找屏失败: 精修后框占画面 %d%%，放弃",
                 (px1 - px0) * (py1 - py0) * 100 / (w * h));
        return false;
    }

    out->x0 = px0 * 100 / w; out->x1 = px1 * 100 / w;
    out->y0 = py0 * 100 / h; out->y1 = py1 * 100 / h;
    out->ok = true;
    return true;
}

// 两个框的四条边是否互相差不到 kAgreeTol 个百分点
static bool box_close(const box_pct_t &a, const box_pct_t &b)
{
    return abs(a.x0 - b.x0) <= kAgreeTol && abs(a.y0 - b.y0) <= kAgreeTol
        && abs(a.x1 - b.x1) <= kAgreeTol && abs(a.y1 - b.y1) <= kAgreeTol;
}

// 屏幕区换算成像素矩形；太小就返回 0（宁可不裁也别裁出个垃圾）
static int panel_rect(int w, int h, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = w * s_panel.x0 / 100;
    *x1 = w * s_panel.x1 / 100;
    *y0 = h * s_panel.y0 / 100;
    *y1 = h * s_panel.y1 / 100;
    if (*x1 <= *x0 + 2 || *y1 <= *y0 + 2) return 0;
    return 1;
}

static void spot_apply(sensor_t *s)
{
    if (!s) return;
    s->set_exposure_ctrl(s, 0);   // 0 = 手动曝光
    s->set_gain_ctrl(s, 0);       // 增益也接管并钉在 1x，别留 AGC 跟控制器抢
    s->set_agc_gain(s, 0);
    s->set_aec_value(s, s_spot.aecv);
}

// 每帧一次：先（可选）重新定位屏幕，再把屏幕区的 p90 控到 target
static void spot_update(const uint8_t *rgb, int w, int h)
{
    if (!s_spot.on) return;
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return;

    if (s_spot.adetect) {
        if (s_hold > 0) {
            s_hold--;                       // 锁定期内不改框，只按手动值测光
        } else {
            box_pct_t det;
            if (panel_detect(rgb, w, h, &det)) {
                if (s_cand.ok && box_close(s_cand, det)) s_cand_n++;
                else { s_cand = det; s_cand_n = 1; }
                if (s_cand_n >= kAgreeNeed) {
                    if (!box_close(s_panel, s_cand))
                        ESP_LOGI(TAG, "屏幕框更新: %d,%d,%d,%d -> %d,%d,%d,%d（连续 %d 帧一致）",
                                 s_panel.x0, s_panel.y0, s_panel.x1, s_panel.y1,
                                 s_cand.x0, s_cand.y0, s_cand.x1, s_cand.y1, s_cand_n);
                    s_panel = s_cand;
                    s_hold  = kHoldRuns;    // 采纳后先安静一阵，到点再复查
                }
            }
        }
    } else {
        s_hold = 0;
    }

    int x0, y0, x1, y1;
    if (!panel_rect(w, h, &x0, &y0, &x1, &y1)) return;

    memset(s_spot_hist, 0, sizeof(s_spot_hist));
    uint32_t tot = 0;
    for (int y = y0; y < y1; y += 2) {
        const uint8_t *p = rgb + ((size_t)y * w + x0) * 3;
        for (int x = x0; x < x1; x += 2, p += 6) {
            int lum = (p[0] * 77 + p[1] * 151 + p[2] * 28) >> 8;
            if (lum > 255) lum = 255;
            s_spot_hist[lum]++;
            tot++;
        }
    }
    if (!tot) return;

    uint32_t want = tot * 9 / 10;
    uint32_t acc  = 0;
    int p90 = 255;
    for (int i = 0; i < 256; i++) {
        acc += s_spot_hist[i];
        if (acc >= want) { p90 = i; break; }
    }
    if (abs(p90 - s_spot.target) <= 6) return;      // 死区，免得来回抖

    int cur = s_spot.aecv;
    int nv  = (int)((int64_t)cur * s_spot.target / (p90 < 8 ? 8 : p90));
    if (nv > cur * 2) nv = cur * 2;                 // 一帧最多翻一倍，防过冲
    if (nv < cur / 2) nv = cur / 2;
    if (nv < 8) nv = 8;
    if (nv > AECV_MAX) nv = AECV_MAX;
    s_spot.aecv = nv;
    s->set_aec_value(s, nv);
    ESP_LOGI(TAG, "点测光 p90=%d(目标%d) 屏=%d,%d,%d,%d -> aecv %d->%d",
             p90, s_spot.target, s_panel.x0, s_panel.y0, s_panel.x1, s_panel.y1, cur, nv);
}

// ---------- YUV422 -> RGB888（修正 R/B 通道顺序）----------
// esp32-camera 的 fmt2rgb888 对 YUV422 输出的是 B,G,R 顺序，而 ESP-DL
// (DL_IMAGE_PIX_TYPE_RGB888) 与本项目的 rgb565() 都按 R,G,B 读取，
// 导致红蓝互换：屏幕显示偏色、且模型输入颜色错乱使识别率下降。
// 这里按标准 BT.601 全范围整数近似自行转换，输出真正的 R,G,B。
static inline void yuv422_to_rgb888(const uint8_t *src, size_t src_len, uint8_t *dst)
{
    size_t pairs = src_len / 4;   // 每 4 字节 Y0 U Y1 V 对应 2 像素
    for (size_t i = 0; i < pairs; i++) {
        int y0 = src[i * 4 + 0];
        int u  = (int)src[i * 4 + 1] - 128;
        int y1 = src[i * 4 + 2];
        int v  = (int)src[i * 4 + 3] - 128;
        for (int p = 0; p < 2; p++) {
            int yy = p ? y1 : y0;
            int r = yy + ((1436 * v) >> 10);              // 1.402
            int g = yy - ((352 * u + 731 * v) >> 10);     // -0.344 -0.714
            int b = yy + ((1814 * u) >> 10);              // 1.772
            *dst++ = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
            *dst++ = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g));
            *dst++ = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
        }
    }
}

// 统一帧转 RGB888：YUV422 走自研正确顺序，其余格式回退库函数
static bool frame_to_rgb888(const camera_fb_t *fb, uint8_t *dst)
{
    if (fb->format == PIXFORMAT_YUV422) {
        yuv422_to_rgb888(fb->buf, fb->len, dst);
        return true;
    }
    return fmt2rgb888(fb->buf, fb->len, fb->format, dst);
}

// ---------- 识别主任务 ----------
// ---------- 共享识别结果（detect_task 写，view_task 读）----------
static SemaphoreHandle_t s_res_mux;
static tft_object_t s_res_objs[DET_MAX_OBJS];
static int s_res_count = 0;
static char s_res_status[64] = "识别引擎启动中...";

static void result_publish(const tft_object_t *objs, int count, const char *status)
{
    xSemaphoreTake(s_res_mux, portMAX_DELAY);
    s_res_count = count > DET_MAX_OBJS ? DET_MAX_OBJS : count;
    if (objs && s_res_count > 0)
        memcpy(s_res_objs, objs, sizeof(tft_object_t) * s_res_count);
    snprintf(s_res_status, sizeof(s_res_status), "%s", status);
    xSemaphoreGive(s_res_mux);
}

// ---------- 视频推流任务：采集 -> RGB565 -> 刷屏 + 叠加检测框 ----------
static void view_task(void *arg)
{
    static uint8_t *rgb888 = nullptr;    // YUV422 转 RGB888 暂存
    static uint8_t *be565  = nullptr;    // 推流帧（大端 RGB565）
    rgb888 = (uint8_t *)heap_caps_malloc(320 * 240 * 3, MALLOC_CAP_SPIRAM);
    be565  = (uint8_t *)heap_caps_malloc(320 * 240 * 2, MALLOC_CAP_SPIRAM);
    if (!rgb888 || !be565) {
        ESP_LOGE(TAG, "view buffers alloc failed");
        vTaskDelete(NULL);
        return;
    }

    tft_object_t local_objs[DET_MAX_OBJS];
    char local_status[64];

    while (1) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            vTaskDelay(pdMS_TO_TICKS(30)); // 推理任务占用帧缓冲时属正常，稍候
            continue;
        }

        bool ok = frame_to_rgb888(fb, rgb888);
        esp_camera_fb_return(fb);

        if (!ok) {
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }

        // RGB888 -> 大端 RGB565
        int npix = 320 * 240;
        for (int i = 0; i < npix; i++) {
            uint16_t c = rgb565(rgb888[i * 3], rgb888[i * 3 + 1], rgb888[i * 3 + 2]);
            be565[i * 2]     = c >> 8;
            be565[i * 2 + 1] = c & 0xFF;
        }

        // 取最新识别结果（拷贝后立即释放锁，绘制不与推理互等）
        xSemaphoreTake(s_res_mux, portMAX_DELAY);
        int n = s_res_count;
        memcpy(local_objs, s_res_objs, sizeof(local_objs));
        snprintf(local_status, sizeof(local_status), "%s", s_res_status);
        xSemaphoreGive(s_res_mux);

        tft_render_view(be565, local_objs, n, local_status);
    }
}

// ===== 帧回传（备用通道）：把送进 classify 的原始 RGB888 以 base64 打到串口 =====
// 默认关闭 —— 每帧 307KB 会刷屏并拖慢识别。正常用 WiFi HTTP 抓帧（见下方 /frame.rgb）。
// 仅当 WiFi 不可用时，在 menuconfig 或编译命令加 -DENABLE_SERIAL_FRAME_DUMP 打开。
#ifdef ENABLE_SERIAL_FRAME_DUMP
static const char B64T[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void frame_dump_b64(const uint8_t *rgb, int w, int h, int seq,
                           const char *label, int pct)
{
    const int len = w * h * 3;
    printf("===FRAME_BEGIN=== seq=%d w=%d h=%d fmt=rgb888 len=%d label=%s pct=%d\n",
           seq, w, h, len, label, pct);

    const int CH = 72;          // 每块输入字节数，3 的倍数 -> 仅末块可能需要 padding
    char line[128];
    for (int off = 0; off < len; off += CH) {
        const int n = (len - off < CH) ? (len - off) : CH;
        const uint8_t *p = rgb + off;
        int k = 0;
        for (int i = 0; i + 2 < n; i += 3) {
            uint32_t v = ((uint32_t)p[i] << 16) | ((uint32_t)p[i + 1] << 8) | p[i + 2];
            line[k++] = B64T[(v >> 18) & 63];
            line[k++] = B64T[(v >> 12) & 63];
            line[k++] = B64T[(v >> 6) & 63];
            line[k++] = B64T[v & 63];
        }
        const int rem = n % 3;
        if (rem == 1) {
            uint32_t v = (uint32_t)p[n - 1] << 16;
            line[k++] = B64T[(v >> 18) & 63];
            line[k++] = B64T[(v >> 12) & 63];
            line[k++] = '=';
            line[k++] = '=';
        } else if (rem == 2) {
            uint32_t v = ((uint32_t)p[n - 2] << 16) | ((uint32_t)p[n - 1] << 8);
            line[k++] = B64T[(v >> 18) & 63];
            line[k++] = B64T[(v >> 12) & 63];
            line[k++] = B64T[(v >> 6) & 63];
            line[k++] = '=';
        }
        line[k] = '\0';
        printf("%s\n", line);
    }
    printf("===FRAME_END===\n");
    fflush(stdout);
}
#endif  // ENABLE_SERIAL_FRAME_DUMP

// ============================================================
// 抓帧服务：把「送进 classify 的那一帧」通过 WiFi HTTP 暴露出来。
// PC 端拿到的就是模型真正看到的像素（含全部几何形变/偏色/噪声），
// 可离线复现推理，不必靠手机翻拍屏幕来猜。
//   GET /            状态页（浏览器直接看画面 + 识别结果）
//   GET /frame.rgb   原始 RGB888 二进制，宽高/序号在响应头
//   GET /capture.jpg JPEG，便于肉眼查看
//   GET /result.json 最近一次识别结果
// ============================================================
static uint8_t *s_frame_buf = nullptr;      // 320*240*3，PSRAM
static int      s_frame_w = 0, s_frame_h = 0;
static uint32_t s_frame_seq = 0;
static char     s_frame_label[64] = {0};
static int      s_frame_pct = 0;
static char     s_frame_photo[32] = {0};   // 这一帧过推理时，屏上显示的是哪张出题图
static SemaphoreHandle_t s_frame_mux = nullptr;

static void frame_share_update(const uint8_t *rgb, int w, int h,
                               const char *label, int pct, const char *photo)
{
    if (!s_frame_buf || !s_frame_mux || w <= 0 || h <= 0) return;
    xSemaphoreTake(s_frame_mux, portMAX_DELAY);
    memcpy(s_frame_buf, rgb, (size_t)w * h * 3);
    s_frame_w = w;
    s_frame_h = h;
    s_frame_seq++;
    snprintf(s_frame_label, sizeof(s_frame_label), "%s", label ? label : "");
    s_frame_pct = pct;
    // 出处必须由「取到这一帧的时刻」决定。等推理跑完（10~20 秒）再问屏上是哪张，
    // 问到的会是中途被换上去的新图 —— 于是 photo 说的不是 pixels 看的东西，
    // 采集脚本拿它当标签就会整批错一格（实测：出 lamp 的那帧被判成西瓜 100%）。
    snprintf(s_frame_photo, sizeof(s_frame_photo), "%s", photo ? photo : "");
    xSemaphoreGive(s_frame_mux);
}

// 只问这一帧的出题图名（给 /result.json 用，不碰 230KB 像素）
static void frame_share_photo(char *dst, size_t n)
{
    if (!dst || !n) return;
    dst[0] = 0;
    if (!s_frame_mux) return;
    xSemaphoreTake(s_frame_mux, portMAX_DELAY);
    snprintf(dst, n, "%s", s_frame_photo);
    xSemaphoreGive(s_frame_mux);
}

// 取一份帧副本；调用方负责 heap_caps_free。返回 nullptr 表示还没有帧。
static uint8_t *frame_share_copy(int *w, int *h, uint32_t *seq,
                                 char *label, size_t label_sz, int *pct)
{
    if (!s_frame_buf || !s_frame_mux) return nullptr;
    xSemaphoreTake(s_frame_mux, portMAX_DELAY);
    *w = s_frame_w; *h = s_frame_h; *seq = s_frame_seq; *pct = s_frame_pct;
    if (label && label_sz) snprintf(label, label_sz, "%s", s_frame_label);
    size_t len = (size_t)s_frame_w * s_frame_h * 3;
    uint8_t *copy = nullptr;
    if (len > 0) {
        copy = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
        if (copy) memcpy(copy, s_frame_buf, len);
    }
    xSemaphoreGive(s_frame_mux);
    return copy;
}

// 只取元数据，不复制 230KB 像素（省一次 PSRAM 分配）
static bool frame_share_meta(int *w, int *h, uint32_t *seq,
                           char *label, size_t label_sz, int *pct)
{
    if (!s_frame_buf || !s_frame_mux) return false;
    xSemaphoreTake(s_frame_mux, portMAX_DELAY);
    *w = s_frame_w; *h = s_frame_h; *seq = s_frame_seq; *pct = s_frame_pct;
    if (label && label_sz) snprintf(label, label_sz, "%s", s_frame_label);
    xSemaphoreGive(s_frame_mux);
    return (*w > 0 && *h > 0);
}

// HTTP 头必须是 ASCII，中文标签做 percent-encoding
static void url_encode(const char *src, char *dst, size_t dst_sz)
{
    static const char HEX[] = "0123456789ABCDEF";
    size_t k = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && k + 4 < dst_sz; ++p) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            dst[k++] = (char)*p;
        } else {
            dst[k++] = '%'; dst[k++] = HEX[*p >> 4]; dst[k++] = HEX[*p & 15];
        }
    }
    dst[k] = '\0';
}

static esp_err_t h_frame_rgb(httpd_req_t *req)
{
    int w = 0, h = 0, pct = 0;
    uint32_t seq = 0;
    char label[64] = {0};
    uint8_t *buf = frame_share_copy(&w, &h, &seq, label, sizeof(label), &pct);
    if (!buf || w <= 0) {
        if (buf) heap_caps_free(buf);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no frame captured yet");
        return ESP_FAIL;
    }
    // 注意：httpd_resp_set_hdr 只保存指针，直到 send 才写出。
    //       所以每个头必须有自己独立的缓冲区，不能复用一个 char[]。
    static char h_w[16], h_h[16], h_seq[16], h_pct[16], h_label[192];
    snprintf(h_w,    sizeof(h_w),    "%d", w);
    snprintf(h_h,    sizeof(h_h),    "%d", h);
    snprintf(h_seq,  sizeof(h_seq),  "%u", (unsigned)seq);
    snprintf(h_pct,  sizeof(h_pct),  "%d", pct);
    url_encode(label, h_label, sizeof(h_label));

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "X-Frame-Width",  h_w);
    httpd_resp_set_hdr(req, "X-Frame-Height", h_h);
    httpd_resp_set_hdr(req, "X-Frame-Seq",    h_seq);
    httpd_resp_set_hdr(req, "X-Frame-Pct",    h_pct);
    httpd_resp_set_hdr(req, "X-Frame-Label",  h_label);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t r = httpd_resp_send(req, (const char *)buf, (size_t)w * h * 3);
    heap_caps_free(buf);
    return r;
}

static esp_err_t h_capture_jpg(httpd_req_t *req)
{
    int w = 0, h = 0, pct = 0;
    uint32_t seq = 0;
    char label[64] = {0};
    uint8_t *buf = frame_share_copy(&w, &h, &seq, label, sizeof(label), &pct);
    if (!buf || w <= 0) {
        if (buf) heap_caps_free(buf);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no frame captured yet");
        return ESP_FAIL;
    }
    uint8_t *jpg = nullptr;
    size_t jpg_len = 0;
    // 实测：esp_jpeg 编码器把传入字节按 B,G,R 解释，直接喂 RGB888 会得到
    // R/B 互换的图（浏览器预览樱桃变蓝）。这里先换序再编码，仅影响预览，
    // 不影响 /frame.rgb 与模型输入。
    uint8_t *bgr = (uint8_t *)heap_caps_malloc((size_t)w * h * 3, MALLOC_CAP_SPIRAM);
    if (!bgr) {
        heap_caps_free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    for (size_t i = 0; i < (size_t)w * h; i++) {
        bgr[i * 3]     = buf[i * 3 + 2];
        bgr[i * 3 + 1] = buf[i * 3 + 1];
        bgr[i * 3 + 2] = buf[i * 3];
    }
    bool ok = fmt2jpg(bgr, (size_t)w * h * 3, w, h, PIXFORMAT_RGB888, 90, &jpg, &jpg_len);
    heap_caps_free(bgr);
    heap_caps_free(buf);
    if (!ok || !jpg) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "jpeg encode failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t r = httpd_resp_send(req, (const char *)jpg, jpg_len);
    free(jpg);   // fmt2jpg 内部用 malloc 分配
    return r;
}

static esp_err_t h_result_json(httpd_req_t *req)
{
    int w = 0, h = 0, pct = 0;
    uint32_t seq = 0;
    char label[64] = {0};
    char photo[32] = {0};
    bool ok = frame_share_meta(&w, &h, &seq, label, sizeof(label), &pct);
    frame_share_photo(photo, sizeof(photo));
    char body[256];
    snprintf(body, sizeof(body),
             "{\"ok\":%s,\"seq\":%u,\"width\":%d,\"height\":%d,\"percent\":%d,"
             "\"label\":\"%s\",\"photo\":\"%s\"}",
             ok ? "true" : "false", (unsigned)seq, w, h, pct, label, photo);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_index(httpd_req_t *req)
{
    static const char HTML[] =
        "<!doctype html><meta charset=utf-8><title>ESP32-S3 items103 抓帧</title>"
        "<body style='font-family:sans-serif;background:#111;color:#eee;margin:20px'>"
        "<h3>端侧抓帧 &middot; items103 (模型实际输入)</h3>"
        "<img id=im src='/capture.jpg' style='width:640px;image-rendering:pixelated;border:1px solid #444'>"
        "<p id=r style='font-size:18px'>loading...</p>"
        "<p><a style='color:#6cf' href='/frame.rgb'>/frame.rgb</a> &middot; "
        "<a style='color:#6cf' href='/capture.jpg'>/capture.jpg</a> &middot; "
        "<a style='color:#6cf' href='/result.json'>/result.json</a> &middot; "
        "<span style='color:#fa0'>4寸屏:</span>"
        "<a style='color:#6cf' href='/big?c=probe'>探针</a>"
        "<a style='color:#6cf' href='/big?c=checker'>棋盘</a>"
        "<a style='color:#6cf' href='/big?c=bars'>彩条</a>"
        "<a style='color:#6cf' href='/big?c=black'>黑</a>"
        "<a style='color:#6cf' href='/big?c=white'>白</a>"
        "<a style='color:#6cf' href='/big?c=red'>红</a>"
        "<a style='color:#6cf' href='/big?map=1'>映射1</a>"
        "<a style='color:#6cf' href='/big?map=0'>映射0</a>"
        "<a style='color:#6cf' href='/big?bl=0'>背光关</a>"
        "<a style='color:#6cf' href='/big?bl=100'>背光开</a></p>"
        "<script>"
        "async function u(){try{let j=await (await fetch('/result.json')).json();"
        "document.getElementById('r').textContent='#'+j.seq+'  '+j.label+'  '+j.percent+'%  '+j.width+'x'+j.height;"
        "}catch(e){document.getElementById('r').textContent='err '+e;}}"
        "setInterval(u,1000);u();"
        "setInterval(()=>{document.getElementById('im').src='/capture.jpg?t='+Date.now();},1500);"
        "</script></body>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, HTML, HTTPD_RESP_USE_STRLEN);
}

// 取查询参数值：?c=probe&hz=800000
static bool query_get(httpd_req_t *req, const char *key, char *out, size_t cap)
{
    out[0] = 0;
    char qry[128];
    if (httpd_req_get_url_query_str(req, qry, sizeof(qry)) != ESP_OK) return false;
    return httpd_query_key_value(qry, key, out, cap) == ESP_OK;
}

static char s_big_last[16] = "probe";   // 改频率/方向后重画同一图案

static void big_draw(const char *p)
{
    if      (!strcmp(p, "bars"))   tft_big_colorbars();
    else if (!strcmp(p, "red"))    tft_big_clear(RGB565C(255, 0, 0));
    else if (!strcmp(p, "green"))  tft_big_clear(RGB565C(0, 255, 0));
    else if (!strcmp(p, "blue"))   tft_big_clear(RGB565C(0, 0, 255));
    else if (!strcmp(p, "black"))  tft_big_clear(RGB565C(0, 0, 0));
    else if (!strcmp(p, "white"))  tft_big_clear(RGB565C(255, 255, 255));
    else if (!strcmp(p, "checker")) tft_big_checker();
    else if (!strcmp(p, "blit"))    tft_big_blittest();
    else if (!strcmp(p, "axis"))    tft_big_axis();
    else                           tft_big_probe();
}

static esp_err_t h_big(httpd_req_t *req)
{
    char v[32] = {0};
    int pin[6] = { -1, -1, -1, -1, -1, -1 };   // mosi sck cs dc rst bl
    const char *keys[6] = { "mosi", "sck", "cs", "dc", "rst", "bl_pin" };
    bool any_pin = false;
    for (int i = 0; i < 6; i++) {
        if (query_get(req, keys[i], v, sizeof(v))) {
            pin[i] = atoi(v);
            any_pin = true;
        }
    }
    if (any_pin) {
        tft_big_remap(pin[0], pin[1], pin[2], pin[3], pin[4], pin[5]);
    }
    if (query_get(req, "hz", v, sizeof(v))) {
        int hz = atoi(v);
        if (hz > 0) tft_big_set_freq(hz);
    }
    if (query_get(req, "rot", v, sizeof(v))) {
        tft_big_set_rotation(atoi(v));
    }
    if (query_get(req, "bl", v, sizeof(v))) {
        tft_big_backlight(atoi(v));
    }
    if (query_get(req, "map", v, sizeof(v))) {
        tft_big_set_map(atoi(v));
    }
    if (query_get(req, "mode", v, sizeof(v))) {
        tft_big_set_mode(atoi(v));
    }
    if (query_get(req, "rstpol", v, sizeof(v))) {
        tft_big_set_rstpol(atoi(v));
    }
    if (query_get(req, "madctl", v, sizeof(v))) {
        tft_big_set_madctl((int)strtol(v, nullptr, 0));   // 支持 0x2C / 44
    }
    bool redraw = true;
    const char *gpio = nullptr;
    char idbuf[96] = "";
    if (query_get(req, "miso", v, sizeof(v))) {
        tft_big_set_miso(atoi(v));
    }
    if (query_get(req, "c", v, sizeof(v))) {
        if      (!strcmp(v, "invert"))   { tft_big_set_invert(true);  redraw = false; }
        else if (!strcmp(v, "noinvert")) { tft_big_set_invert(false); redraw = false; }
        else if (!strcmp(v, "gpio"))     { gpio = tft_big_gpio_probe(); redraw = false; }
        else if (!strcmp(v, "id")) {
            snprintf(idbuf, sizeof(idbuf), ",\"id\":\"0x%08lX\",\"st\":\"0x%08lX\"",
                     (unsigned long)tft_big_read_id(), (unsigned long)tft_big_read_status());
            redraw = false;
        }
        else {
            strncpy(s_big_last, v, sizeof(s_big_last) - 1);
            s_big_last[sizeof(s_big_last) - 1] = 0;
        }
    } else if (!strcmp(s_big_last, "none")) {
        redraw = false;   // c=none：只改参数，不重画
    }
    if (redraw) big_draw(s_big_last);

    char body[384];
    snprintf(body, sizeof(body), "{\"ok\":true,\"pattern\":\"%s\",\"cfg\":\"%s\",\"diag\":\"%s\"",
             s_big_last, tft_big_pin_desc(), tft_big_diag());
    if (idbuf[0])  strncat(body, idbuf, sizeof(body) - strlen(body) - 1);
    if (gpio) {
        strncat(body, ",\"gpio\":\"", sizeof(body) - strlen(body) - 1);
        strncat(body, gpio, sizeof(body) - strlen(body) - 1);
        strncat(body, "\"", sizeof(body) - strlen(body) - 1);
    }
    strncat(body, "}", sizeof(body) - strlen(body) - 1);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// ---------- 摄像头测光/增益：GET /cam，调参不用重烧 ----------
// 屏幕是自发光的亮点，而 OV3660 的 AEC 按整帧平均测光：桌面曝光刚好的时候，
// 屏早就全白削顶了（出题图里的浅色物体、白底细节全被抹平，模型只能瞎猜）。
// 所以把这些口子开出来给 PC 端扫：手动曝光 = aec=0 + aecv=<行数>，手动增益 = agc=0 + agcg=<0..30>。
static esp_err_t h_cam(httpd_req_t *req)
{
    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no sensor");
        return ESP_OK;
    }
    char v[32];
    int n = 0;
    // 只有写失败（I2C 返回值非 0）才记进 rets，空串=全部写成功
    char rets[128] = "";

#define CAMSET(key, call)                                     \
    do {                                                      \
        if (query_get(req, key, v, sizeof(v))) {               \
            int r_ = s->call(s, atoi(v));                      \
            n++;                                              \
            if (r_) strncat(rets, key "!", sizeof(rets) - strlen(rets) - 2); \
        }                                                     \
    } while (0)

    CAMSET("bri", set_brightness);
    CAMSET("con", set_contrast);
    CAMSET("sat", set_saturation);
    CAMSET("sharp", set_sharpness);
    CAMSET("den", set_denoise);
    CAMSET("eff", set_special_effect);
    CAMSET("wb", set_wb_mode);
    CAMSET("awb", set_whitebal);
    CAMSET("awbg", set_awb_gain);
    CAMSET("aec", set_exposure_ctrl);   // 0=手动曝光
    CAMSET("aec2", set_aec2);
    CAMSET("ae", set_ae_level);         // -2..2，往负走=整体压暗
    CAMSET("aecv", set_aec_value);      // 手动曝光时长（曝光行数）
    CAMSET("agc", set_gain_ctrl);       // 0=手动增益
    CAMSET("agcg", set_agc_gain);       // 0..30
    if (query_get(req, "ceil", v, sizeof(v))) {           // 0..6 -> 2X..128X
        int c = atoi(v);
        if (c < 0) c = 0;
        if (c > 6) c = 6;
        if (s->set_gainceiling(s, (gainceiling_t)c)) strncat(rets, "ceil!", sizeof(rets) - strlen(rets) - 2);
        n++;
    }
    CAMSET("lenc", set_lenc);
    CAMSET("bpc", set_bpc);
    CAMSET("wpc", set_wpc);
#undef CAMSET
    if (query_get(req, "q", v, sizeof(v))) { s->set_quality(s, atoi(v)); n++; }

    // 点测光：spot=1 开 / spot=0 关（关的时候把自动曝光自动增益还给传感器）
    if (query_get(req, "spot", v, sizeof(v))) {
        s_spot.on = atoi(v) != 0;
        if (s_spot.on) spot_apply(s);
        else { s->set_exposure_ctrl(s, 1); s->set_gain_ctrl(s, 1); }
        n++;
    }
    if (query_get(req, "target", v, sizeof(v))) {
        int t = atoi(v);
        if (t >= 32 && t <= 250) s_spot.target = t;
        n++;
    }
    // roi=x0,y0,x1,y1（百分比）：手动钉死屏幕区，同时关掉自动找屏
    if (query_get(req, "roi", v, sizeof(v))) {
        int a = 0, b = 0, c = 0, d = 0;
        if (sscanf(v, "%d,%d,%d,%d", &a, &b, &c, &d) == 4 && c > a && d > b) {
            int q[4] = { a, b, c, d };
            for (int i = 0; i < 4; i++) {
                if (q[i] < 0) q[i] = 0;
                if (q[i] > 100) q[i] = 100;
            }
            s_panel = { q[0], q[1], q[2], q[3], true };
            s_spot.adetect = false;      // 手动钉了框就别再自动改它
            s_cand.ok = false; s_cand_n = 0; s_hold = 0;
        }
        n++;
    }
    if (query_get(req, "adetect", v, sizeof(v))) {   // 1=每帧自动找屏（默认）
        s_spot.adetect = atoi(v) != 0;
        s_cand.ok = false; s_cand_n = 0; s_hold = 0;  // 重新武装：票从头攒
        n++;
    }
    if (query_get(req, "crop", v, sizeof(v))) {      // 1=只把屏幕区喂给模型
        s_crop = atoi(v) != 0;
        n++;
    }
    // 手动改过 aecv 的话，控制器下一步得从新值出发，不然会跳回去
    if (s_spot.on) s_spot.aecv = (int)s->status.aec_value;

    // r=3503,3500,3501,3502,350a,350b  直接读传感器寄存器（十六进制，逗号分隔）。
    // status 里存的是「我们让它设成多少」，寄存器才是「芯片现在实际是多少」——
    // 两者不一致就说明手动曝光/增益根本没生效（AEC 还在接管）。
    char raw[160] = "";
    if (query_get(req, "r", v, sizeof(v))) {
        char list[32];
        strncpy(list, v, sizeof(list) - 1);
        list[sizeof(list) - 1] = 0;
        for (char *t = strtok(list, ","); t && strlen(raw) < 140; t = strtok(nullptr, ",")) {
            int addr = (int)strtol(t, nullptr, 16);
            int val = s->get_reg(s, addr, 0xFF);
            snprintf(raw + strlen(raw), sizeof(raw) - strlen(raw), " %02X=%02X",
                     addr & 0xFF, val & 0xFF);
        }
    }

    camera_status_t &st = s->status;
    char body[640];
    snprintf(body, sizeof(body),
             "{\"ok\":true,\"set\":%d,"
             "\"bri\":%d,\"con\":%d,\"sat\":%d,\"sharp\":%d,\"den\":%u,\"eff\":%u,\"wb\":%u,"
             "\"awb\":%u,\"awbg\":%u,\"aec\":%u,\"aec2\":%u,\"ae\":%d,\"aecv\":%u,"
             "\"agc\":%u,\"agcg\":%u,\"ceil\":%u,\"lenc\":%u,\"bpc\":%u,\"wpc\":%u,\"q\":%u",
             n, st.brightness, st.contrast, st.saturation, st.sharpness, st.denoise,
             st.special_effect, st.wb_mode, st.awb, st.awb_gain, st.aec, st.aec2,
             st.ae_level, st.aec_value, st.agc, st.agc_gain, st.gainceiling,
             st.lenc, st.bpc, st.wpc, st.quality);
    if (rets[0]) {
        strncat(body, ",\"failed\":\"", sizeof(body) - strlen(body) - 1);
        strncat(body, rets, sizeof(body) - strlen(body) - 1);
        strncat(body, "\"", sizeof(body) - strlen(body) - 1);
    }
    char spotbuf[112];
    snprintf(spotbuf, sizeof(spotbuf),
             "on=%d adetect=%d crop=%d target=%d aecv=%d panel=%d,%d,%d,%d ok=%d",
             (int)s_spot.on, (int)s_spot.adetect, (int)s_crop, s_spot.target,
             s_spot.aecv, s_panel.x0, s_panel.y0, s_panel.x1, s_panel.y1, (int)s_panel.ok);
    strncat(body, ",\"spot\":\"", sizeof(body) - strlen(body) - 1);
    strncat(body, spotbuf, sizeof(body) - strlen(body) - 1);
    strncat(body, "\"", sizeof(body) - strlen(body) - 1);
    if (raw[0]) {
        strncat(body, ",\"raw\":\"", sizeof(body) - strlen(body) - 1);
        strncat(body, raw + 1, sizeof(body) - strlen(body) - 1);
        strncat(body, "\"", sizeof(body) - strlen(body) - 1);
    }
    strncat(body, "}", sizeof(body) - strlen(body) - 1);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// ---------- 出题图片：POST /show，body 直接就是一张 JPEG ----------
//   curl --data-binary @apple.jpg http://<ip>/show
// 不落 flash：一张图几十 KB，收进 PSRAM 直接解、直接刷，换题不用重烧固件。
static esp_err_t h_show(httpd_req_t *req)
{
    char body[420];
    char info[160] = "";
    int rc = -2;

    int len = req->content_len;
    if (len <= 0 || len > 1024 * 1024) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"bad content_len\"}");
    }
    uint8_t *jpg = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!jpg) {
        httpd_resp_set_status(req, "507 Insufficient Storage");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"no psram\"}");
    }
    int off = 0;
    while (off < len) {
        int n = len - off;
        if (n > 4096) n = 4096;
        int r = httpd_req_recv(req, (char *)jpg + off, n);
        if (r <= 0) { off = r; break; }
        off += r;
    }
    if (off == len) {
        rc = tft_big_show_jpeg(jpg, (size_t)len, info, sizeof(info));
        if (rc == 0) strncpy(s_big_last, "none", sizeof(s_big_last) - 1);  // 别让 /big 把图盖掉
    } else {
        snprintf(info, sizeof(info), "recv %d/%d", off, len);
        rc = -5;
    }
    free(jpg);

    snprintf(body, sizeof(body), "{\"ok\":%s,\"rc\":%d,\"bytes\":%d,\"info\":\"%s\",\"cfg\":\"%s\"",
             rc == 0 ? "true" : "false", rc, len, info, tft_big_pin_desc());
    strncat(body, ",\"diag\":\"", sizeof(body) - strlen(body) - 1);
    strncat(body, tft_big_diag(), sizeof(body) - strlen(body) - 1);
    strncat(body, "\"}", sizeof(body) - strlen(body) - 1);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// ---------- 出题图库：GET /quiz ----------
//   /quiz                  看状态（张数/间隔/上一张）
//   /quiz?every=10         改换图间隔（秒，2..3600），立刻生效
//   /quiz?next             马上随机换一张
//   /quiz?n=apple          马上显示指定那张（.jpg 可省）
//   /quiz?list             列出全部文件名
static esp_err_t h_quiz(httpd_req_t *req)
{
    char v[48] = {0};
    char info[192] = "";
    int rc = 0;

    if (query_get(req, "every", v, sizeof(v))) {
        int s = atoi(v);
        rc = quiz_photos_set_interval(s);
        snprintf(info, sizeof(info), "interval=%d", rc);
    } else if (query_get(req, "n", v, sizeof(v))) {
        rc = quiz_photos_show(v, info, sizeof(info));
    } else if (query_get(req, "next", v, sizeof(v))) {
        rc = quiz_photos_show(nullptr, info, sizeof(info));
    } else if (query_get(req, "list", v, sizeof(v))) {
        char *lst = (char *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
        if (!lst) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no psram");
                    return ESP_OK; }
        lst[0] = 0;
        int n = quiz_photos_count(), off = 0;
        off += snprintf(lst + off, 4096 - off, "{\"ok\":true,\"count\":%d,\"photos\":[", n);
        for (int i = 0; i < n && off < 3900; i++)
            off += snprintf(lst + off, 4096 - off, "%s\"%s\"", i ? "," : "", quiz_photos_name(i));
        snprintf(lst + off, 4096 - off, "]}");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, lst);
        free(lst);
        return ESP_OK;
    } else {
        snprintf(info, sizeof(info), "idle");
    }
    // 出题屏归图库管了：别让后面随手一个 /big 把图盖成自检图案
    if (rc == 0) strncpy(s_big_last, "none", sizeof(s_big_last) - 1);

    char body[512];
    snprintf(body, sizeof(body),
             "{\"ok\":%s,\"count\":%d,\"interval\":%d,\"last\":\"%s\",\"info\":\"%s\",\"cfg\":\"%s\"",
             rc >= 0 ? "true" : "false", quiz_photos_count(), quiz_photos_interval(),
             quiz_photos_last(), info, tft_big_pin_desc());
    strncat(body, ",\"diag\":\"", sizeof(body) - strlen(body) - 1);
    strncat(body, tft_big_diag(), sizeof(body) - strlen(body) - 1);
    strncat(body, "\"}", sizeof(body) - strlen(body) - 1);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static void frame_http_start(void)
{
    if (!s_frame_mux) s_frame_mux = xSemaphoreCreateMutex();
    if (!s_frame_buf) {
        s_frame_buf = (uint8_t *)heap_caps_malloc(320 * 240 * 3, MALLOC_CAP_SPIRAM);
    }
    if (!s_frame_buf || !s_frame_mux) {
        ESP_LOGE(TAG, "抓帧缓冲分配失败，HTTP 服务不启动");
        return;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 10;
    cfg.stack_size = 8192;
    cfg.send_wait_timeout = 20;   // 单帧 230KB，放宽超时
    cfg.recv_wait_timeout = 20;

    httpd_handle_t srv = nullptr;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start 失败");
        return;
    }
    httpd_uri_t u = { "/",           HTTP_GET, h_index,       nullptr };
    httpd_register_uri_handler(srv, &u);
    u = { "/frame.rgb",   HTTP_GET, h_frame_rgb,   nullptr };
    httpd_register_uri_handler(srv, &u);
    u = { "/capture.jpg", HTTP_GET, h_capture_jpg, nullptr };
    httpd_register_uri_handler(srv, &u);
    u = { "/result.json", HTTP_GET, h_result_json, nullptr };
    httpd_register_uri_handler(srv, &u);
    u = { "/big",         HTTP_GET,  h_big,         nullptr };
    httpd_register_uri_handler(srv, &u);
    u = { "/show",        HTTP_POST, h_show,        nullptr };
    httpd_register_uri_handler(srv, &u);
    u = { "/quiz",        HTTP_GET,  h_quiz,        nullptr };
    httpd_register_uri_handler(srv, &u);
    u = { "/cam",         HTTP_GET,  h_cam,         nullptr };
    httpd_register_uri_handler(srv, &u);

    const char *ip = wifi_manager_get_ip();
    ESP_LOGI(TAG, "HTTP 抓帧服务已启动: http://%s/", ip ? ip : "(未连接)");
}

// ---------- 识别任务：周期性推理，更新共享结果 ----------
// 栈只能来自内部 RAM。接上 4 寸屏后内部 RAM 只剩约 27KB、最大连续块 15KB，
// 16KB 栈直接分配失败（xTaskCreate 返回 -1 且默认不报错，表现为「识别任务凭空消失」）。
// 先按 12KB 建任务，跑起来后用高水位决定能不能再收紧/放宽。
#define DETECT_TASK_STACK   12288
static TaskHandle_t s_detect_h;

static void detect_task(void *arg)
{
    ESP_LOGI(TAG, "detect_task 启动，开始加载 items103 模型");
    Items103 *items = new Items103(false);
    ESP_LOGI(TAG, "Items103 model loaded");

    static tft_object_t objs[DET_MAX_OBJS];
    uint8_t *rgb_buf = (uint8_t *)heap_caps_malloc(320 * 240 * 3, MALLOC_CAP_SPIRAM);
    uint8_t *crop_buf = nullptr;    // 屏幕区裁剪缓冲，crop=1 时才懒分配（PSRAM 230KB）
    if (!rgb_buf) {
        ESP_LOGE(TAG, "rgb_buf alloc failed");
        result_publish(NULL, 0, "内存不足");
        vTaskDelete(NULL);
        return;
    }

    char status[64];
    bool told_wait = false;
    int  frames = 0;
    while (1) {
        if (!told_wait) { ESP_LOGI(TAG, "等待摄像头第一帧…"); told_wait = true; }
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGW(TAG, "Camera capture failed");
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (!frame_to_rgb888(fb, rgb_buf)) {
            ESP_LOGE(TAG, "frame_to_rgb888 failed");
            esp_camera_fb_return(fb);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        int fw = fb->width, fh = fb->height;
        // 就在拿到帧的这一刻记下屏上是哪张出题图。推理要 10~20 秒，期间屏可能
        // 被换图任务或采集脚本改走，晚问一句就会把标签记到下一张头上。
        char shot[32] = {0};
        snprintf(shot, sizeof(shot), "%s", quiz_photos_last());
        esp_camera_fb_return(fb);   // 尽早归还，推流任务继续使用

        // 点测光放在推理之前：这一帧的屏幕区亮度分布就是控制器的输入，
        // 调完下一帧就正常了（推理本身 4 秒一跑，控制器有足够时间收敛）。
        spot_update(rgb_buf, fw, fh);

        // crop=1：只把屏幕区喂给模型。屏只占画面 15% 的时候，拿整帧分类
        // 分出来的是「桌子」而不是出题图（实测：屏上玉米，模型 82% 说台灯）。
        const uint8_t *in = rgb_buf;
        int iw = fw, ih = fh;
        int px0, py0, px1, py1;
        if (s_crop && panel_rect(fw, fh, &px0, &py0, &px1, &py1)) {
            int cw = px1 - px0, ch = py1 - py0;
            if (!crop_buf) crop_buf = (uint8_t *)heap_caps_malloc(320 * 240 * 3, MALLOC_CAP_SPIRAM);
            if (crop_buf && cw >= 24 && ch >= 24 && cw <= 320 && ch <= 240) {
                for (int y = 0; y < ch; y++)
                    memcpy(crop_buf + ((size_t)y * cw) * 3,
                           rgb_buf + ((size_t)(py0 + y) * fw + px0) * 3, (size_t)cw * 3);
                in = crop_buf;
                iw = cw;
                ih = ch;
            }
        }

        int64_t t0 = esp_timer_get_time();
        Items103::Result r = items->classify(in, iw, ih);
        int64_t dt_us = esp_timer_get_time() - t0;

        const char *label = Items103::cn(r.id);
        int pct = (int)(r.score * 100.0f + 0.5f);
        ESP_LOGI(TAG, "classify: %s (%s) %d%%  %.1fms  输入 %dx%d%s",
                 label, Items103::en(r.id), pct, dt_us / 1e3, iw, ih,
                 in == crop_buf ? "(裁剪)" : "");

#ifdef ENABLE_SERIAL_FRAME_DUMP
        // 备用通道：回传送进 classify 的原始帧
        static int s_dump_seq = 0;
        frame_dump_b64(in, iw, ih, s_dump_seq++, label, pct);
#endif

        // 主通道：把这一帧（就是 classify 的输入）存进共享缓冲，供 HTTP 抓取
        frame_share_update(in, iw, ih, label, pct, shot);

        int n = 0;
        if (r.score >= SCORE_THR) {
            // 分类器输出整幅画面的类别，铺满屏作为叠加框
            objs[n].label   = label;
            objs[n].percent = pct;
            objs[n].x = 0;
            objs[n].y = 0;
            objs[n].w = 320;
            objs[n].h = 240;
            n++;
            snprintf(status, sizeof(status), "%s %d%% | %.1fms",
                     label, pct, dt_us / 1e3);
        } else {
            snprintf(status, sizeof(status), "未识别 | %.1fms", dt_us / 1e3);
        }
        result_publish(objs, n, status);

        if (++frames == 3) {
            ESP_LOGI(TAG, "detect_task 栈高水位=%u 字节未用 / %d，内部空闲=%u",
                     (unsigned)(uxTaskGetStackHighWaterMark(s_detect_h) * sizeof(StackType_t)),
                     DETECT_TASK_STACK,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ---------- 入口 ----------
extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32-S3 端侧物体识别 ===");

    s_res_mux = xSemaphoreCreateMutex();

    // 1) 共用 I2C + AXP313A 摄像头供电
    ESP_ERROR_CHECK(shared_i2c_init());
    i2c_scan();
    if (axp313a_begin(I2C_PORT, AXP313A_I2C_ADDR) != ESP_OK)
        ESP_LOGE(TAG, "AXP313A not responding - camera may have no power");
    if (axp313a_set_camera_power(1.5f, 2.8f) != ESP_OK) // OV3660: DVDD/AVDD
        ESP_LOGW(TAG, "Camera power set failed");
    vTaskDelay(pdMS_TO_TICKS(200)); // 等电源稳定

    // 2) 屏幕
    if (tft_display_init() != 0) {
        ESP_LOGE(TAG, "Display init failed! Halting.");
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    tft_show_status("摄像头识别", "正在初始化 OV3660...");

    // 2b) 4" ST7796S 出题屏（独立 SPI3：MOSI=11(A5) SCK=10(A4) CS=9(D7)
    //     DC=13(D11) RST=14(D10) SDO=12(D12)，LED 硬接 3V3 故 bl=-1 不驱动背光；
    //     模组的 SD_CS 必须接 3V3，悬空会让片上 SD 卡占死共享 MISO、整屏无响应）
    //     失败不影响 2" 屏与识别，只打日志。
    if (tft_big_init() == 0) {
        tft_big_probe();
        ESP_LOGI(TAG, "4寸出题屏自检图形已输出（可 curl /big?c=... 远程改）");
    } else {
        ESP_LOGW(TAG, "4寸屏(ST7796S)初始化失败，检查 A5/A4/D7/D11/D10/D12 接线与 VCC/GND");
    }

    // 2c) 出题图库：挂载 flash 里的 storage(SPIFFS)，扫出 /spiffs/photos/*.jpg 名单。
    //     注意这一步会占内部 RAM（SPIFFS 的 fd/cache），所以放在建任务之前，日志里能看到吃多少。
    int nphotos = quiz_photos_init();
    if (nphotos <= 0)
        ESP_LOGE(TAG, "出题图库不可用：检查 storage 分区是否烧了 build/storage.bin");
    // 换图任务必须在这里就建：后面摄像头 GDMA、WiFi/httpd、模型加载会把内部 RAM 的
    // 最大连续块榨干（实测到 step 7 只剩 3.2 KB），xTaskCreate 会直接失败。
    if (nphotos > 0) quiz_photos_start_task();

    // 3) 摄像头
    if (camera_init() != ESP_OK) {
        tft_show_status("摄像头初始化失败", "请检查 CAM 排线是否插好");
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // 4) 加载模型（推流启动前先显示加载提示，避免黑屏等待）
    tft_show_status("加载识别模型", "Items103 MobileNetV2 INT8 -> PSRAM");

    // 5) WiFi + HTTP 抓帧服务（诊断用：PC 直接拉取「模型看到的那一帧」）
    //    连不上也不能阻塞识别，所以失败只打日志。
    if (wifi_manager_connect(CONFIG_WIFI_SSID, CONFIG_WIFI_PASSWORD, 15000) == 0) {
        frame_http_start();
        const char *ip = wifi_manager_get_ip();
        if (ip && *ip) {
            char buf[96];
            snprintf(buf, sizeof(buf), "http://%s/", ip);
            tft_show_status("摄像头识别", buf);
        }
    } else {
        ESP_LOGW(TAG, "WiFi 连接失败，跳过 HTTP 抓帧服务");
    }

    // 6) 双任务：推流(高优先级) + 周期推理
    //    xTaskCreate 的栈必须来自内部 RAM，分配失败是静默的，所以这里显式检查。
    BaseType_t r1 = xTaskCreate(view_task,   "view_task",   8192,  NULL, 5, NULL);
    BaseType_t r2 = xTaskCreate(detect_task, "detect_task", DETECT_TASK_STACK, NULL, 3, &s_detect_h);
    ESP_LOGI(TAG, "任务创建 view=%d detect=%d | 内部空闲=%u 最小连续块=%u PSRAM空闲=%u",
             (int)r1, (int)r2,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (r1 != pdPASS || r2 != pdPASS) {
        ESP_LOGE(TAG, "任务创建失败：内部 RAM 不足，检查两块屏的 DMA 缓冲占用");
        tft_show_status("任务创建失败", "内部 RAM 耗尽");
    }

    // 7) 出题换图任务已在 2c 起好（内部 RAM 还宽裕的时候），这里不再重复。
}
