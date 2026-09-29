// fruit19.cpp — 从 fruit19 分区加载 espdl 分类模型并推理（19 类固定水果）
#include "fruit19.hpp"
#include "fruit19_classes.hpp"

#include "dl_model_base.hpp"
#include "dl_image_preprocessor.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include <cmath>
#include <vector>

static const char *TAG = "fruit19";

using namespace dl;
using namespace fbs;

Fruit19::Fruit19(bool verbose) : m_verbose(verbose)
{
    bool param_copy = true;
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) < 1024 * 1024 * 9) {
        param_copy = false;
    }
    m_model = new Model("fruit19",                                    // 分区 label
                        "fruit19.espdl",                              // 打包模型内文件名
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
    ESP_LOGI(TAG, "fruit19 espdl loaded");
}

Fruit19::~Fruit19()
{
    delete m_preproc;
    delete m_model;
}

Fruit19::Result Fruit19::classify(const uint8_t *rgb888, int w, int h)
{
    image::img_t img = {};
    img.data     = const_cast<uint8_t *>(rgb888);
    img.width    = w;
    img.height   = h;
    img.pix_type = image::DL_IMAGE_PIX_TYPE_RGB888;

    int64_t t0 = esp_timer_get_time();
    m_preproc->preprocess(img);
    m_model->run();
    int64_t dt_us = esp_timer_get_time() - t0;

    TensorBase *out = m_model->get_output();
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

    if (m_verbose) {
        ESP_LOGI(TAG, "top1=%s (%.1f%%) %.0fms", FRUIT19_CN[best], score * 100.f, dt_us / 1000.f);
    }
    Result r{best, score};
    return r;
}

const char *Fruit19::cn(int id)
{
    return (id >= 0 && id < NUM_CLASSES) ? FRUIT19_CN[id] : "未知";
}

const char *Fruit19::en(int id)
{
    return (id >= 0 && id < NUM_CLASSES) ? FRUIT19_EN[id] : "?";
}
