#pragma once

#include <opencv2/core.hpp>

class FaceAligner {
public:
    // Align face using 5 keypoints, output 112x112 standard face
    static cv::Mat align(const cv::Mat& src, const float keypoints[5][2], int out_size = 112);

private:
    static cv::Mat getReferenceMatrix(int out_size);
};
