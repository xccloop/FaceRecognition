#include "facedetector.h"
#include "facealigner.h"
#include "featureextractor.h"

#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct RegisteredFace {
    std::string name;
    std::vector<float> feature;
};

// Save single feature to file
static void saveFeature(const std::string& path, const std::vector<float>& feat)
{
    std::ofstream ofs(path, std::ios::binary);
    int32_t dim = (int32_t)feat.size();
    ofs.write(reinterpret_cast<const char*>(&dim), sizeof(dim));
    ofs.write(reinterpret_cast<const char*>(feat.data()), dim * sizeof(float));
}

// Load single feature from file
static std::vector<float> loadFeature(const std::string& path)
{
    std::ifstream ifs(path, std::ios::binary);
    int32_t dim = 0;
    ifs.read(reinterpret_cast<char*>(&dim), sizeof(dim));
    std::vector<float> feat(dim);
    ifs.read(reinterpret_cast<char*>(feat.data()), dim * sizeof(float));
    return feat;
}

// Load all registered faces from features/ directory
static std::vector<RegisteredFace> loadRegistry(const std::string& dir)
{
    std::vector<RegisteredFace> registry;
    if (!fs::exists(dir)) return registry;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() == ".bin") {
            RegisteredFace rf;
            rf.name = entry.path().stem().string();
            rf.feature = loadFeature(entry.path().string());
            registry.push_back(std::move(rf));
        }
    }
    return registry;
}

// Detect the largest face in image (heuristic: main subject)
static FaceInfo* getLargestFace(std::vector<FaceInfo>& faces)
{
    if (faces.empty()) return nullptr;
    FaceInfo* best = &faces[0];
    float bestArea = (best->x2 - best->x1) * (best->y2 - best->y1);
    for (auto& f : faces) {
        float area = (f.x2 - f.x1) * (f.y2 - f.y1);
        if (area > bestArea) { best = &f; bestArea = area; }
    }
    return best;
}

static void printUsage(const char* prog)
{
    std::cerr << "Usage:" << std::endl;
    std::cerr << "  " << prog << " register <photo.jpg> <name>" << std::endl;
    std::cerr << "  " << prog << " identify <photo.jpg>" << std::endl;
    std::cerr << "  " << prog << " compare <img1.jpg> <img2.jpg>" << std::endl;
    std::cerr << "  " << prog << " test <photo.jpg>" << std::endl;
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        printUsage(argv[0]);
        return 1;
    }

    std::string cmd = argv[1];
    // Find models: try relative to exe dir, then relative to cwd
    std::string model_dir = "../../models/ncnn_models/";
    if (!fs::exists(model_dir + "det_500m.param"))
        model_dir = "models/ncnn_models/";

    std::string det_param = model_dir + "det_500m.param";
    std::string det_bin   = model_dir + "det_500m.bin";
    std::string rec_param = model_dir + "w600k_mbf.param";
    std::string rec_bin   = model_dir + "w600k_mbf.bin";

    std::cout << "Loading models..." << std::endl;
    FaceDetector detector(det_param.c_str(), det_bin.c_str());
    FeatureExtractor extractor(rec_param.c_str(), rec_bin.c_str());
    std::cout << "Models loaded." << std::endl;

    // ---- register ----
    if (cmd == "register") {
        if (argc < 4) { printUsage(argv[0]); return 1; }
        std::string imgPath = argv[2];
        std::string name = argv[3];

        cv::Mat img = cv::imread(imgPath);
        if (img.empty()) { std::cerr << "Failed to load: " << imgPath << std::endl; return 1; }

        auto faces = detector.detect(img);
        FaceInfo* f = getLargestFace(faces);
        if (!f) { std::cerr << "No face detected." << std::endl; return 1; }

        cv::Mat aligned = FaceAligner::align(img, f->keypoints);
        auto feat = extractor.extract(aligned);

        fs::create_directories("features");
        saveFeature("features/" + name + ".bin", feat);
        std::cout << "Registered: " << name << std::endl;
        return 0;
    }

    // ---- identify ----
    if (cmd == "identify") {
        std::string imgPath = argv[2];
        cv::Mat img = cv::imread(imgPath);
        if (img.empty()) { std::cerr << "Failed to load: " << imgPath << std::endl; return 1; }

        auto registry = loadRegistry("features");
        if (registry.empty()) { std::cerr << "No registered faces. Use 'register' first." << std::endl; return 1; }

        auto faces = detector.detect(img);
        FaceInfo* f = getLargestFace(faces);
        if (!f) { std::cerr << "No face detected." << std::endl; return 1; }

        cv::Mat aligned = FaceAligner::align(img, f->keypoints);
        auto feat = extractor.extract(aligned);

        // Find best match
        float bestSim = -2.f;
        std::string bestName;
        for (const auto& rf : registry) {
            float sim = FeatureExtractor::cosineSimilarity(feat, rf.feature);
            std::cout << "  " << rf.name << ": similarity=" << sim << std::endl;
            if (sim > bestSim) { bestSim = sim; bestName = rf.name; }
        }

        std::cout << std::endl;
        if (bestSim > 0.4f) {
            std::cout << "Matched: " << bestName << "  (confidence: " << bestSim << ")" << std::endl;
        } else {
            std::cout << "No match. (best " << bestName << " only " << bestSim << ")" << std::endl;
        }
        return 0;
    }

    // ---- compare ----
    if (cmd == "compare") {
        if (argc < 4) { printUsage(argv[0]); return 1; }

        cv::Mat img1 = cv::imread(argv[2]);
        cv::Mat img2 = cv::imread(argv[3]);
        if (img1.empty() || img2.empty()) { std::cerr << "Failed to load image." << std::endl; return 1; }

        auto faces1 = detector.detect(img1);
        auto faces2 = detector.detect(img2);
        FaceInfo* f1 = getLargestFace(faces1);
        FaceInfo* f2 = getLargestFace(faces2);
        if (!f1 || !f2) { std::cerr << "Face not found in one or both images." << std::endl; return 1; }

        cv::Mat a1 = FaceAligner::align(img1, f1->keypoints);
        cv::Mat a2 = FaceAligner::align(img2, f2->keypoints);
        auto feat1 = extractor.extract(a1);
        auto feat2 = extractor.extract(a2);

        float sim = FeatureExtractor::cosineSimilarity(feat1, feat2);
        std::cout << "Similarity: " << sim << std::endl;
        std::cout << (sim > 0.4f ? "SAME person" : "DIFFERENT person") << std::endl;
        return 0;
    }

    // ---- test (default) ----
    {
        cv::Mat img = cv::imread(argv[2]);
        if (img.empty()) { std::cerr << "Failed to load: " << argv[2] << std::endl; return 1; }

        std::cout << "Image: " << img.cols << "x" << img.rows << std::endl;
        auto faces = detector.detect(img);
        std::cout << "Detected " << faces.size() << " face(s)" << std::endl;

        for (size_t i = 0; i < faces.size(); i++) {
            const auto& f = faces[i];
            std::cout << "Face #" << i + 1 << ": bbox=[" << f.x1 << "," << f.y1
                      << "," << f.x2 << "," << f.y2 << "] score=" << f.score << std::endl;

            cv::Mat aligned = FaceAligner::align(img, f.keypoints);
            auto feat = extractor.extract(aligned);
            std::cout << "  Feature[0..7]: ";
            for (int k = 0; k < 8; k++) std::cout << feat[k] << " ";
            std::cout << "..." << std::endl;
        }
    }

    return 0;
}
