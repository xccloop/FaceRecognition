#include "featureextractor.h"

#include <cmath>

FeatureExtractor::FeatureExtractor(const char* param_path, const char* bin_path)
{
    net_.opt.use_vulkan_compute = false;
    net_.load_param(param_path);
    net_.load_model(bin_path);
}

FeatureExtractor::~FeatureExtractor() { net_.clear(); }

std::vector<float> FeatureExtractor::extract(const cv::Mat& aligned_bgr)
{
    ncnn::Mat in = ncnn::Mat::from_pixels(aligned_bgr.data, ncnn::Mat::PIXEL_BGR,
                                          aligned_bgr.cols, aligned_bgr.rows);
    in.substract_mean_normalize(MEAN, NORM);

    ncnn::Extractor ex = net_.create_extractor();
    ex.input("input.1", in);

    ncnn::Mat out;
    ex.extract("516", out);

    std::vector<float> feat(out.w);
    for (int i = 0; i < out.w; i++)
        feat[i] = out[i];

    normalize(feat);
    return feat;
}

void FeatureExtractor::normalize(std::vector<float>& feat)
{
    float sum = 0.f;
    for (float v : feat) sum += v * v;
    float inv = 1.f / std::sqrt(sum + 1e-8f);
    for (float& v : feat) v *= inv;
}

float FeatureExtractor::cosineSimilarity(const std::vector<float>& a, const std::vector<float>& b)
{
    float dot = 0.f;
    for (size_t i = 0; i < a.size(); i++)
        dot += a[i] * b[i];
    return dot;  // features are already L2-normalized
}
