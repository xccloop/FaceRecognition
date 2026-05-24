#pragma once

#include <ncnn/net.h>
#include <opencv2/core.hpp>
#include <vector>

struct FaceInfo {
    float x1, y1, x2, y2;
    float keypoints[5][2];
    float score;
};

class FaceDetector {
public:
    FaceDetector(const char* param_path, const char* bin_path);
    ~FaceDetector();

    std::vector<FaceInfo> detect(const cv::Mat& bgr,
                                 float score_thresh = 0.3f,
                                 float nms_thresh = 0.4f,
                                 int max_side = 480);

private:
    struct Anchor {
        float cx, cy;
        int stride;
    };

    void generateAnchors(int w, int h);

    ncnn::Net net_;
    std::vector<Anchor> anchors_;
    int last_w_ = 0, last_h_ = 0;

    static constexpr int STRIDES[3] = {8, 16, 32};
    static constexpr int DET_MAX_SIDE = 480;
    static constexpr float MEAN[3] = {127.5f, 127.5f, 127.5f};
    static constexpr float NORM[3] = {1.f / 128.f, 1.f / 128.f, 1.f / 128.f};
};
