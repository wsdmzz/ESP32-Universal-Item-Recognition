// items103.hpp — 103 类常见物品 INT8 分类器（espdl, ESP32-S3，拍屏闭集识别）
// 数据源：真实照片 + 屏幕模拟增强（augmented/），替换 items127（即梦生成图 127 类）。
#pragma once
#include <cstdint>
#include <memory>

namespace dl {
class Model;
namespace image {
class ImagePreprocessor;
}
}

class Items103 {
public:
    static constexpr int NUM_CLASSES = 103;

    explicit Items103(bool verbose = false);
    ~Items103();

    struct Result {
        int id;         // 类别索引（items103.yaml groups.items103 字母序）
        float score;    // softmax 置信度 0-1
    };

    // top1 分类。rgb888: w*h*3 字节 RGB 顺序缓冲。
    Result classify(const uint8_t *rgb888, int w, int h);

    static const char *cn(int id);   // 中文名（UTF-8）
    static const char *en(int id);

private:
    dl::Model *m_model = nullptr;
    dl::image::ImagePreprocessor *m_preproc = nullptr;
    bool m_verbose;
};

using Items103Ptr = std::shared_ptr<Items103>;
