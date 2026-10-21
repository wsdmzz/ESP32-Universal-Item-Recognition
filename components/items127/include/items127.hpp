// items127.hpp — 127 类常见物品 INT8 分类器（espdl, ESP32-S3，拍屏闭集识别）
#pragma once
#include <cstdint>
#include <memory>

namespace dl {
class Model;
namespace image {
class ImagePreprocessor;
}
}

class Items127 {
public:
    static constexpr int NUM_CLASSES = 127;

    explicit Items127(bool verbose = false);
    ~Items127();

    struct Result {
        int id;         // 类别索引（items127.yaml groups.fruit 顺序）
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

using Items127Ptr = std::shared_ptr<Items127>;
