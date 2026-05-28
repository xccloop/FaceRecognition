#include "facedetector.h"
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <opencv2/imgproc.hpp>

FaceDetector::FaceDetector(const char* param_path, const char* bin_path)
{
    net_.opt.use_vulkan_compute = false;
    net_.opt.num_threads = 2;
    net_.load_param(param_path);
    net_.load_model(bin_path);
}

FaceDetector::~FaceDetector() { net_.clear(); }

void FaceDetector::generateAnchors(int w, int h)
{
    if (w == last_w_ && h == last_h_) return;

    anchors_.clear();
    anchors_.reserve((w/8)*(h/8)*2 + (w/16)*(h/16)*2 + (w/32)*(h/32)*2);

    for (int s : STRIDES) {
        int fm_w = (w + s - 1) / s;
        int fm_h = (h + s - 1) / s;
        for (int sub = 0; sub < 2; sub++) {
            for (int i = 0; i < fm_h; i++) {
                float cy = (i + 0.5f) * s;
                for (int j = 0; j < fm_w; j++) {
                    float cx = (j + 0.5f) * s;
                    anchors_.push_back({cx, cy, s});
                }
            }
        }
    }
    last_w_ = w;
    last_h_ = h;
}

std::vector<FaceInfo> FaceDetector::detect(const cv::Mat& bgr,
                                           float score_thresh,
                                           float nms_thresh,
                                           int max_side)
{
    int orig_w = bgr.cols, orig_h = bgr.rows;

    // Match Python: max-side scaling, only downscale (never upscale)
    float scale = 1.0f;
    if (max_side > 0 && std::max(orig_w, orig_h) > max_side) {
        scale = (float)max_side / std::max(orig_w, orig_h);
    }

    int nw = (int)(orig_w * scale);
    int nh = (int)(orig_h * scale);
    cv::Mat resized;
    cv::resize(bgr, resized, cv::Size(nw, nh));

    // Pad to multiples of 32 for FPN feature-map alignment (max 31px)
    int pw = ((nw + 31) / 32) * 32 - nw;
    int ph = ((nh + 31) / 32) * 32 - nh;
    int pad_left   = pw / 2;
    int pad_right  = pw - pad_left;
    int pad_top    = ph / 2;
    int pad_bottom = ph - pad_top;

    cv::Mat img;
    cv::copyMakeBorder(resized, img, pad_top, pad_bottom, pad_left, pad_right,
                       cv::BORDER_CONSTANT, cv::Scalar(128, 128, 128));

    int img_w = img.cols, img_h = img.rows;

    generateAnchors(img_w, img_h);

    ncnn::Mat in = ncnn::Mat::from_pixels(img.data, ncnn::Mat::PIXEL_BGR, img_w, img_h);
    in.substract_mean_normalize(MEAN, NORM);

    ncnn::Extractor ex = net_.create_extractor();
    ex.input("input.1", in);

    const char* score_names[3] = {"443", "468", "493"};
    const char* bbox_names[3]  = {"446", "471", "496"};
    const char* kps_names[3]   = {"449", "474", "499"};

    std::vector<ncnn::Mat> scores(3), bboxes(3), kpss(3);
    for (int i = 0; i < 3; i++) {
        ex.extract(score_names[i], scores[i]);
        ex.extract(bbox_names[i],  bboxes[i]);
        ex.extract(kps_names[i],   kpss[i]);
    }

    std::vector<FaceInfo> faces;
    int anchor_idx = 0;
    for (int level = 0; level < 3; level++) {
        int stride = STRIDES[level];
        int N_total = scores[level].w;
        const float* sdp = (const float*)scores[level].data;
        const float* bdp = (const float*)bboxes[level].data;
        const float* kdp = (const float*)kpss[level].data;

        int fm_w = (img_w + stride - 1) / stride;
        int fm_h = (img_h + stride - 1) / stride;
        int N_spatial = fm_h * fm_w;

        for (int k = 0; k < N_total; k++, anchor_idx++) {
            float score = sdp[k];
            if (score < score_thresh) continue;

            if (anchor_idx >= (int)anchors_.size()) break;
            const Anchor& a = anchors_[anchor_idx];

            int sub = k / N_spatial;
            int pos = k % N_spatial;

            float bx = bdp[(sub * 2 + 0) * N_total + pos];
            float by = bdp[(sub * 2 + 0) * N_total + pos + N_spatial];
            float bw = bdp[(sub * 2 + 1) * N_total + pos];
            float bh = bdp[(sub * 2 + 1) * N_total + pos + N_spatial];

            float x1 = a.cx - bx * stride;
            float y1 = a.cy - by * stride;
            float x2 = a.cx + bw * stride;
            float y2 = a.cy + bh * stride;

            x1 = std::max(0.f, x1);
            y1 = std::max(0.f, y1);
            x2 = std::min((float)img_w, x2);
            y2 = std::min((float)img_h, y2);
            if (x2 <= x1 || y2 <= y1) continue;

            FaceInfo f;
            f.x1 = x1; f.y1 = y1; f.x2 = x2; f.y2 = y2;
            f.score = score;

            for (int p = 0; p < 5; p++) {
                f.keypoints[p][0] = a.cx + kdp[(sub * 5 + p) * N_total + pos] * stride;
                f.keypoints[p][1] = a.cy + kdp[(sub * 5 + p) * N_total + pos + N_spatial] * stride;
            }

            faces.push_back(f);
        }
    }

    // NMS
    std::sort(faces.begin(), faces.end(),
              [](const FaceInfo& a, const FaceInfo& b) { return a.score > b.score; });

    std::vector<FaceInfo> nms_result;
    std::vector<bool> suppressed(faces.size(), false);
    for (size_t i = 0; i < faces.size(); i++) {
        if (suppressed[i]) continue;
        nms_result.push_back(faces[i]);
        float area_i = (faces[i].x2 - faces[i].x1) * (faces[i].y2 - faces[i].y1);
        for (size_t j = i + 1; j < faces.size(); j++) {
            if (suppressed[j]) continue;
            float inter_w = std::max(0.f, std::min(faces[i].x2, faces[j].x2) - std::max(faces[i].x1, faces[j].x1));
            float inter_h = std::max(0.f, std::min(faces[i].y2, faces[j].y2) - std::max(faces[i].y1, faces[j].y1));
            float inter = inter_w * inter_h;
            float area_j = (faces[j].x2 - faces[j].x1) * (faces[j].y2 - faces[j].y1);
            float iou = inter / (area_i + area_j - inter + 1e-6f);
            if (iou > nms_thresh) suppressed[j] = true;
        }
    }

    // Filter faces whose center is outside the content (non-pad) region
    float content_x1 = (float)pad_left;
    float content_x2 = (float)(pad_left + nw);
    float content_y1 = (float)pad_top;
    float content_y2 = (float)(pad_top + nh);

    std::vector<FaceInfo> result;
    for (auto& f : nms_result) {
        float cx = (f.x1 + f.x2) * 0.5f;
        float cy = (f.y1 + f.y2) * 0.5f;
        if (cx >= content_x1 && cx <= content_x2 &&
            cy >= content_y1 && cy <= content_y2) {
            result.push_back(f);
        }
    }

    // Map coordinates back to original image space
    float inv = 1.0f / scale;
    for (auto& f : result) {
        f.x1 = (f.x1 - pad_left) * inv;
        f.y1 = (f.y1 - pad_top) * inv;
        f.x2 = (f.x2 - pad_left) * inv;
        f.y2 = (f.y2 - pad_top) * inv;
        for (int p = 0; p < 5; p++) {
            f.keypoints[p][0] = (f.keypoints[p][0] - pad_left) * inv;
            f.keypoints[p][1] = (f.keypoints[p][1] - pad_top) * inv;
        }
        f.x1 = std::max(0.f, std::min(f.x1, (float)orig_w));
        f.y1 = std::max(0.f, std::min(f.y1, (float)orig_h));
        f.x2 = std::max(0.f, std::min(f.x2, (float)orig_w));
        f.y2 = std::max(0.f, std::min(f.y2, (float)orig_h));
    }

    result.erase(std::remove_if(result.begin(), result.end(),
        [](const FaceInfo& f) { return f.x2 <= f.x1 || f.y2 <= f.y1; }),
        result.end());

    return result;
}
