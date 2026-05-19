#include "facedetector.h"
#include "custom_layers.h"

#include <algorithm>
#include <cmath>

FaceDetector::FaceDetector(const char* param_path, const char* bin_path)
{
    net_.opt.use_vulkan_compute = false;
    registerCustomLayers(net_);
    net_.load_param(param_path);
    net_.load_model(bin_path);
}

FaceDetector::~FaceDetector() { net_.clear(); }

void FaceDetector::generateAnchors(int w, int h)
{
    if (w == last_w_ && h == last_h_) return;

    anchors_.clear();
    for (int s : STRIDES) {
        int fm_w = (w + s - 1) / s;
        int fm_h = (h + s - 1) / s;
        for (int i = 0; i < fm_h; i++) {
            float cy = (i + 0.5f) * s;
            for (int j = 0; j < fm_w; j++) {
                float cx = (j + 0.5f) * s;
                anchors_.push_back({cx, cx, s}); // first anchor
                anchors_.push_back({cx, cx, s}); // second anchor (same center)
            }
        }
    }
    last_w_ = w;
    last_h_ = h;
}

std::vector<FaceInfo> FaceDetector::detect(const cv::Mat& bgr, float score_thresh, float nms_thresh)
{
    int img_w = bgr.cols, img_h = bgr.rows;
    generateAnchors(img_w, img_h);

    // Preprocess
    ncnn::Mat in = ncnn::Mat::from_pixels(bgr.data, ncnn::Mat::PIXEL_BGR, img_w, img_h);
    in.substract_mean_normalize(MEAN, NORM);

    // Forward
    ncnn::Extractor ex = net_.create_extractor();
    ex.input("input.1", in);

    // Extract outputs for 3 strides
    const char* score_names[3] = {"443", "468", "493"};
    const char* bbox_names[3]  = {"446", "471", "496"};
    const char* kps_names[3]   = {"449", "474", "499"};

    std::vector<ncnn::Mat> scores(3), bboxes(3), kpss(3);
    for (int i = 0; i < 3; i++) {
        ex.extract(score_names[i], scores[i]);
        ex.extract(bbox_names[i],  bboxes[i]);
        ex.extract(kps_names[i],   kpss[i]);
    }

    // Decode
    std::vector<FaceInfo> faces;
    int anchor_idx = 0;
    for (int level = 0; level < 3; level++) {
        int stride = STRIDES[level];
        int n = scores[level].h;  // number of anchors for this level

        for (int k = 0; k < n; k++, anchor_idx++) {
            float score = scores[level].row(k)[0];
            if (score < score_thresh) continue;

            const Anchor& a = anchors_[anchor_idx];

            const float* b = bboxes[level].row(k);
            float x1 = a.cx - b[0] * stride;
            float y1 = a.cy - b[1] * stride;
            float x2 = a.cx + b[2] * stride;
            float y2 = a.cy + b[3] * stride;

            // Clamp to image
            x1 = std::max(0.f, x1);
            y1 = std::max(0.f, y1);
            x2 = std::min((float)img_w, x2);
            y2 = std::min((float)img_h, y2);
            if (x2 <= x1 || y2 <= y1) continue;

            FaceInfo f;
            f.x1 = x1; f.y1 = y1; f.x2 = x2; f.y2 = y2;
            f.score = score;

            const float* kp = kpss[level].row(k);
            for (int p = 0; p < 5; p++) {
                f.keypoints[p][0] = a.cx + kp[p * 2]     * stride;
                f.keypoints[p][1] = a.cy + kp[p * 2 + 1] * stride;
            }

            faces.push_back(f);
        }
    }

    // NMS (sort by score descending)
    std::sort(faces.begin(), faces.end(),
              [](const FaceInfo& a, const FaceInfo& b) { return a.score > b.score; });

    std::vector<FaceInfo> result;
    std::vector<bool> suppressed(faces.size(), false);
    for (size_t i = 0; i < faces.size(); i++) {
        if (suppressed[i]) continue;
        result.push_back(faces[i]);
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

    return result;
}
