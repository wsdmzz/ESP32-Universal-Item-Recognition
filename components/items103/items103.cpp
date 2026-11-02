// items103.cpp — 从 items103 分区加载 espdl 分类模型并推理（103 类常见物品）
#include "items103.hpp"
#include "items103_classes.hpp"

#include "dl_model_base.hpp"
#include "dl_image_preprocessor.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include <cmath>
#include <vector>

static const char *TAG = "items103";

using namespace dl;
using namespace fbs;

Items103::Items103(bool verbose) : m_verbose(verbose)
{
    // 权重要不要从 flash 拷进 PSRAM。esp-dl 的注释写得很直白：param_copy=false
    // 省 PSRAM 但「牺牲推理性能」——权重留在 mmap 的 flash 里，每层都要跨着
    // MMU/cache 去取。分层计时实测 features.18 那层 737KB 权重只跑出 2.2MB/s，
    // 整网 300 MMAC 用了 4.6 秒（≈66 MMAC/s），瓶颈就在取权重，不在算。
    // 原来按「总 PSRAM < 9MB 就不拷」判断，N16R8 是 8MB，于是永远走慢路径。
    // 改成看空闲量：权重 2.3MB，留 1MB 余量才拷。
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    bool param_copy = psram_free > (1024 * 1024 * 4);
    ESP_LOGI(TAG, "PSRAM 空闲 %uKB -> param_copy=%d",
             (unsigned)(psram_free / 1024), param_copy ? 1 : 0);
    m_model = new Model("items103",                                    // 分区 label
                        "items103.espdl",                              // 打包模型内文件名
                        MODEL_LOCATION_IN_FLASH_PARTITION,
                        0,
                        MEMORY_MANAGER_GREEDY,
                        nullptr,
                        param_copy);
    m_model->minimize();

    // 模型输入协议：与训练 transforms 完全一致 (ToTensor[0,1] -> Normalize(m,s))。
    // esp-dl NormQuant 计算 (raw - mean)/std，raw∈[0,255]，故 mean/std 取 255*ImageNet 值：
    //   (raw - 255*m)/(255*s) = (raw/255 - m)/s。归一化在卷积前完成，padding 语义与训练一致。
    m_preproc = new image::ImagePreprocessor(
        m_model,
        std::array<float, 3>{0.485f * 255.f, 0.456f * 255.f, 0.406f * 255.f},
        std::array<float, 3>{0.229f * 255.f, 0.224f * 255.f, 0.225f * 255.f});
    ESP_LOGI(TAG, "items103 espdl loaded");
}

Items103::~Items103()
{
    delete m_preproc;
    delete m_model;
}

Items103::Result Items103::classify(const uint8_t *rgb888, int w, int h)
{
    image::img_t img = {};
    img.data     = const_cast<uint8_t *>(rgb888);
    img.width    = w;
    img.height   = h;
    img.pix_type = image::DL_IMAGE_PIX_TYPE_RGB888;

    int64_t t0 = esp_timer_get_time();
    m_preproc->preprocess(img);
    int64_t t1 = esp_timer_get_time();
    m_model->run();
    int64_t dt_us = esp_timer_get_time() - t0;

    // 一次性的分层体检（只在 verbose 构造时做）：哪一层在吃时间、张量落在哪里。
    // 就是靠它定位到「权重留在 flash 里取不动」这个瓶颈的。
    static bool s_profiled = false;
    if (m_verbose && !s_profiled) {
        s_profiled = true;
        ESP_LOGI(TAG, "首次推理：预处理 %.0fms / 推理 %.0fms（输入 %dx%d）",
                 (t1 - t0) / 1000.0, (esp_timer_get_time() - t1) / 1000.0, w, h);
        m_model->profile_memory();
        m_model->profile_module(true);
    }

    TensorBase *out = m_model->get_output();
    if (!out || !out->data) {
        ESP_LOGE(TAG, "classify: invalid model output out=%p data=%p", out, out ? out->data : nullptr);
        return {-1, 0.0f};
    }

    const int8_t *raw = static_cast<const int8_t *>(out->data);
    float scale = std::ldexp(1.0f, out->exponent.get());

    int n = out->size < NUM_CLASSES ? out->size : NUM_CLASSES;
    std::vector<float> logits(n);
    int best = 0;
    float mx = -1e30f;
    for (int i = 0; i < n; i++) {
        logits[i] = raw[i] * scale;
        if (logits[i] > mx) { mx = logits[i]; best = i; }
    }
    // top1 softmax 置信度（温度 0.5 使分数分布更适合展示）
    float ssum = 0.f;
    for (float v : logits) ssum += std::exp((v - mx) * 2.0f);
    float score = 1.f / ssum;

    ESP_LOGI(TAG, "top1=%s (%.1f%%) %.0fms [预处理%.0f 推理%.0f]",
             ITEMS103_CN[best], score * 100.f, dt_us / 1000.f,
             (t1 - t0) / 1000.0, (esp_timer_get_time() - t1) / 1000.0);
    Result r{best, score};
    return r;
}

const char *Items103::cn(int id)
{
    return (id >= 0 && id < NUM_CLASSES) ? ITEMS103_CN[id] : "未知";
}

const char *Items103::en(int id)
{
    return (id >= 0 && id < NUM_CLASSES) ? ITEMS103_EN[id] : "?";
}
