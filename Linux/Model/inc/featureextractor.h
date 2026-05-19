#pragma once

#include <ncnn/net.h>
#include <opencv2/core.hpp>
#include <vector>

class FeatureExtractor {
public:
    FeatureExtractor(const char* param_path, const char* bin_path);
    ~FeatureExtractor();

    // Extract 512-dim feature from 112x112 aligned face (BGR)
    std::vector<float> extract(const cv::Mat& aligned_bgr);

    // L2-normalize feature in-place
    static void normalize(std::vector<float>& feat);

    // Cosine similarity between two features
    static float cosineSimilarity(const std::vector<float>& a, const std::vector<float>& b);

private:
    ncnn::Net net_;

    static constexpr float MEAN[3] = {127.5f, 127.5f, 127.5f};
    static constexpr float NORM[3] = {1.f / 127.5f, 1.f / 127.5f, 1.f / 127.5f};
    static constexpr int INPUT_SIZE = 112;
};
