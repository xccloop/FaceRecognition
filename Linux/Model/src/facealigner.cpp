#include "facealigner.h"

#include <opencv2/imgproc.hpp>
#include <opencv2/calib3d.hpp>

cv::Mat FaceAligner::getReferenceMatrix(int out_size)
{
    // ArcFace standard 5-point reference for 112x112
    float src[5][2] = {
        {30.2946f, 51.6963f},
        {65.5318f, 51.6963f},
        {48.0252f, 71.7366f},
        {33.5493f, 92.3655f},
        {62.7299f, 92.3655f},
    };

    float scale = out_size / 112.f;
    cv::Mat ref(5, 2, CV_32F);
    for (int i = 0; i < 5; i++) {
        ref.at<float>(i, 0) = src[i][0] * scale;
        ref.at<float>(i, 1) = src[i][1] * scale;
    }
    return ref;
}

cv::Mat FaceAligner::align(const cv::Mat& src, const float keypoints[5][2], int out_size)
{
    cv::Mat src_pts(5, 2, CV_32F);
    for (int i = 0; i < 5; i++) {
        src_pts.at<float>(i, 0) = keypoints[i][0];
        src_pts.at<float>(i, 1) = keypoints[i][1];
    }

    cv::Mat ref = getReferenceMatrix(out_size);
    cv::Mat M = cv::estimateAffinePartial2D(src_pts, ref);

    if (M.empty()) {
        // Fallback: use simple crop around bbox center
        cv::Rect roi(0, 0, out_size, out_size);
        return cv::Mat::zeros(out_size, out_size, CV_8UC3);
    }

    cv::Mat aligned;
    cv::warpAffine(src, aligned, M, cv::Size(out_size, out_size));
    return aligned;
}
