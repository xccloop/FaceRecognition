#include "facedetector.h"
#include "facealigner.h"
#include "featureextractor.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/highgui.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// ============================================================================
// FeatureDB — persistent feature database (one .bin file per registered face)
// ============================================================================
class FeatureDB {
public:
    void save(const std::string& name, const std::vector<float>& feat) {
        std::lock_guard<std::mutex> lock(mtx_);
        fs::create_directories("features");
        std::ofstream ofs("features/" + name + ".bin", std::ios::binary);
        if (!ofs) throw std::runtime_error("Cannot write features/" + name + ".bin");
        int32_t dim = (int32_t)feat.size();
        ofs.write(reinterpret_cast<const char*>(&dim), sizeof(dim));
        ofs.write(reinterpret_cast<const char*>(feat.data()), dim * sizeof(float));
    }

    bool has(const std::string& name) const {
        std::lock_guard<std::mutex> lock(mtx_);
        return fs::exists("features/" + name + ".bin");
    }

    void load() {
        std::lock_guard<std::mutex> lock(mtx_);
        registry_.clear();
        if (!fs::exists("features")) return;
        for (const auto& entry : fs::directory_iterator("features")) {
            if (entry.path().extension() != ".bin") continue;
            Entry e;
            e.name = entry.path().stem().string();
            e.feature = loadOne(entry.path().string());
            registry_.push_back(std::move(e));
        }
        std::cout << "[FeatureDB] Loaded " << registry_.size() << " registered face(s)." << std::endl;
    }

    struct Match { std::string name; float confidence; };

    Match identify(const std::vector<float>& feat, float threshold = 0.4f) const {
        std::lock_guard<std::mutex> lock(mtx_);
        Match best{"", -2.f};
        for (const auto& e : registry_) {
            float sim = cosineSimilarity(feat, e.feature);
            if (sim > best.confidence) { best.name = e.name; best.confidence = sim; }
        }
        if (best.confidence < threshold) return {"", best.confidence};
        return best;
    }

    std::vector<Match> compareAll(const std::vector<float>& feat) const {
        std::lock_guard<std::mutex> lock(mtx_);
        std::vector<Match> results;
        results.reserve(registry_.size());
        for (const auto& e : registry_) {
            results.push_back({e.name, cosineSimilarity(feat, e.feature)});
        }
        std::sort(results.begin(), results.end(),
                  [](const Match& a, const Match& b) { return a.confidence > b.confidence; });
        return results;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return registry_.empty();
    }
    size_t size() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return registry_.size();
    }
    const std::vector<float>& feature(size_t idx) const {
        std::lock_guard<std::mutex> lock(mtx_);
        return registry_[idx].feature;
    }
    const std::string& name(size_t idx) const {
        std::lock_guard<std::mutex> lock(mtx_);
        return registry_[idx].name;
    }

private:
    struct Entry { std::string name; std::vector<float> feature; };
    std::vector<Entry> registry_;
    mutable std::mutex mtx_;

    static std::vector<float> loadOne(const std::string& path) {
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs) throw std::runtime_error("Cannot read " + path);
        int32_t dim = 0;
        ifs.read(reinterpret_cast<char*>(&dim), sizeof(dim));
        std::vector<float> feat(dim);
        ifs.read(reinterpret_cast<char*>(feat.data()), dim * sizeof(float));
        return feat;
    }

    static float cosineSimilarity(const std::vector<float>& a, const std::vector<float>& b) {
        float dot = 0.f;
        for (size_t i = 0; i < a.size(); i++) dot += a[i] * b[i];
        return dot;
    }
};

// ============================================================================
// Helpers
// ============================================================================

static void printUsage(const char* prog) {
    std::cerr << "Usage:" << std::endl;
    std::cerr << "  " << prog << " register <photo.jpg> <name>" << std::endl;
    std::cerr << "  " << prog << " identify <photo.jpg>" << std::endl;
    std::cerr << "  " << prog << " compare <img1.jpg> <img2.jpg>" << std::endl;
    std::cerr << "  " << prog << " test <photo.jpg>" << std::endl;
    std::cerr << "  " << prog << " live                (webcam, press q to quit)" << std::endl;
}

static FaceInfo requireSingleFace(std::vector<FaceInfo>& faces) {
    if (faces.empty())
        throw std::runtime_error("CMD_NOFACE: No face detected");
    if (faces.size() > 1) {
        std::sort(faces.begin(), faces.end(),
                  [](const FaceInfo& a, const FaceInfo& b) { return a.score > b.score; });
        std::cout << "[INFO] " << faces.size() << " faces detected, using best (score="
                  << faces[0].score << ")" << std::endl;
    }
    return faces[0];
}

static void drawFace(cv::Mat& img, const FaceInfo& f, const std::string& label) {
    cv::rectangle(img, cv::Point((int)f.x1, (int)f.y1),
                  cv::Point((int)f.x2, (int)f.y2), cv::Scalar(0, 255, 0), 2);
    for (int p = 0; p < 5; p++) {
        cv::circle(img, cv::Point((int)f.keypoints[p][0], (int)f.keypoints[p][1]),
                   2, cv::Scalar(0, 0, 255), -1);
    }
    if (!label.empty()) {
        cv::putText(img, label, cv::Point((int)f.x1, (int)f.y1 - 10),
                    cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 0), 2);
    }
}

static void drawHUD(cv::Mat& img, float fps, float detectMs, float extractMs, int faces) {
    int y = 25;
    auto put = [&](const std::string& text, cv::Scalar color = cv::Scalar(0, 255, 0)) {
        cv::putText(img, text, cv::Point(10, y), cv::FONT_HERSHEY_SIMPLEX, 0.55, color, 2);
        y += 22;
    };

    char buf[64];
    snprintf(buf, sizeof(buf), "FPS: %.1f", fps);
    put(buf);

    snprintf(buf, sizeof(buf), "Detect: %.1f ms", detectMs);
    put(buf);

    snprintf(buf, sizeof(buf), "Extract: %.1f ms", extractMs);
    put(buf);

    snprintf(buf, sizeof(buf), "Faces: %d", faces);
    put(buf, faces > 0 ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 0, 255));
}

// ============================================================================
// Commands
// ============================================================================

static int cmdRegister(const std::string& imgPath, const std::string& name,
                       FaceDetector& detector, FeatureExtractor& extractor,
                       FeatureDB& db) {
    if (db.has(name)) {
        std::cerr << "[WARN] '" << name << "' already registered. Overwriting..." << std::endl;
    }
    cv::Mat img = cv::imread(imgPath);
    if (img.empty()) throw std::runtime_error("Failed to load image: " + imgPath);
    auto faces = detector.detect(img, 0.3f);
    auto face = requireSingleFace(faces);
    cv::Mat aligned = FaceAligner::align(img, face.keypoints);
    auto feat = extractor.extract(aligned);
    db.save(name, feat);
    std::cout << "[OK] Registered: " << name << std::endl;
    return 0;
}

static int cmdIdentify(const std::string& imgPath,
                       FaceDetector& detector, FeatureExtractor& extractor,
                       FeatureDB& db) {
    db.load();
    if (db.empty()) throw std::runtime_error("No registered faces. Use 'register' first.");
    cv::Mat img = cv::imread(imgPath);
    if (img.empty()) throw std::runtime_error("Failed to load image: " + imgPath);
    auto faces = detector.detect(img, 0.3f);
    auto face = requireSingleFace(faces);
    cv::Mat aligned = FaceAligner::align(img, face.keypoints);
    auto feat = extractor.extract(aligned);
    for (size_t i = 0; i < db.size(); i++) {
        float sim = FeatureExtractor::cosineSimilarity(feat, db.feature(i));
        std::cout << "  " << db.name(i) << ": similarity=" << sim << std::endl;
    }
    auto match = db.identify(feat, 0.4f);
    if (match.confidence > 0.4f)
        std::cout << "[OK] Matched: " << match.name
                  << " (confidence: " << match.confidence << ")" << std::endl;
    else
        std::cout << "[NO MATCH] best=" << match.name
                  << " (confidence: " << match.confidence << ")" << std::endl;
    return 0;
}

static int cmdCompare(const std::string& path1, const std::string& path2,
                      FaceDetector& detector, FeatureExtractor& extractor) {
    cv::Mat img1 = cv::imread(path1);
    cv::Mat img2 = cv::imread(path2);
    if (img1.empty() || img2.empty()) throw std::runtime_error("Failed to load image.");
    auto faces1 = detector.detect(img1, 0.3f);
    auto faces2 = detector.detect(img2, 0.3f);
    auto f1 = requireSingleFace(faces1);
    auto f2 = requireSingleFace(faces2);
    cv::Mat a1 = FaceAligner::align(img1, f1.keypoints);
    cv::Mat a2 = FaceAligner::align(img2, f2.keypoints);
    auto feat1 = extractor.extract(a1);
    auto feat2 = extractor.extract(a2);
    float sim = FeatureExtractor::cosineSimilarity(feat1, feat2);
    std::cout << "Similarity: " << sim << std::endl;
    std::cout << (sim > 0.4f ? "[SAME person]" : "[DIFFERENT person]") << std::endl;
    return 0;
}

static int cmdTest(const std::string& imgPath,
                   FaceDetector& detector, FeatureExtractor& extractor) {
    cv::Mat img = cv::imread(imgPath);
    if (img.empty()) throw std::runtime_error("Failed to load: " + imgPath);
    std::cout << "Image: " << img.cols << "x" << img.rows << std::endl;
    auto faces = detector.detect(img, 0.3f);
    std::cout << "Detected " << faces.size() << " face(s)" << std::endl;
    if (faces.size() != 1) {
        std::cout << "[CMD_MULTIFACE] Expected exactly 1 face, got " << faces.size()
                  << " - rejecting." << std::endl;
    }
    for (size_t i = 0; i < faces.size(); i++) {
        const auto& f = faces[i];
        std::cout << "Face #" << (i + 1) << ": bbox=[" << f.x1 << "," << f.y1
                  << "," << f.x2 << "," << f.y2 << "] score=" << f.score << std::endl;
        try {
            cv::Mat aligned = FaceAligner::align(img, f.keypoints);
            auto feat = extractor.extract(aligned);
            std::cout << "  Feature[0..7]: ";
            for (int k = 0; k < 8 && k < (int)feat.size(); k++)
                std::cout << feat[k] << " ";
            std::cout << "..." << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "  [Align/Extract failed]: " << e.what() << std::endl;
        }
    }
    return 0;
}

// ============================================================================
// Live webcam mode
// ============================================================================

static int cmdLive(FaceDetector& detector, FeatureExtractor& extractor, FeatureDB& db) {
    db.load();

    cv::VideoCapture cap(0);
    if (!cap.isOpened()) {
        std::cerr << "[ERROR] Cannot open webcam." << std::endl;
        return 1;
    }

    cap.set(cv::CAP_PROP_FRAME_WIDTH, 320);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 240);

    cv::Mat frame;
    cap >> frame;
    if (frame.empty()) {
        std::cerr << "[ERROR] Cannot read from webcam." << std::endl;
        return 1;
    }

    int camW = frame.cols, camH = frame.rows;
    std::cout << "[Live] Webcam: " << camW << "x" << camH << std::endl;
    std::cout << "[Live] Keys: q=quit  r=register  i=identify  s=snapshot" << std::endl;

    // FPS tracking: sliding window over last 30 frames
    std::deque<double> frameTimes;
    std::deque<double> detectTimes;
    std::deque<double> extractTimes;
    const size_t WINDOW = 30;

    std::vector<float> lastFeat;
    FaceInfo lastFace;
    bool hasFace = false;

    // Multi-frame registration state
    int regCountdown = 0;
    const int REG_FRAMES = 20;
    std::vector<float> regAccum;
    std::string regName;

    while (true) {
        auto t0 = Clock::now();

        cap >> frame;
        if (frame.empty()) break;
        cv::Mat display = frame.clone();

        // Detect (match Python's threshold for webcam)
        auto t1 = Clock::now();
        auto faces = detector.detect(frame, 0.3f);
        auto t2 = Clock::now();

        double detectMs = std::chrono::duration<double, std::milli>(t2 - t1).count();

        hasFace = false;
        double extractMs = 0.0;

        // Extract and identify EVERY face independently
        for (size_t i = 0; i < faces.size(); i++) {
            try {
                auto t3 = Clock::now();
                cv::Mat aligned = FaceAligner::align(frame, faces[i].keypoints);
                auto feat = extractor.extract(aligned);
                auto t4 = Clock::now();
                extractMs += std::chrono::duration<double, std::milli>(t4 - t3).count();

                std::string label;
                bool recognized = false;
                if (!db.empty()) {
                    auto all = db.compareAll(feat);
                    auto match = all[0];
                    if (match.confidence > 0.4f) {
                        recognized = true;
                        label = match.name + " " + std::to_string((int)(match.confidence * 100)) + "%";

                        // Show detailed per-person similarity next to each face
                        int yOff = 0;
                        for (const auto& m : all) {
                            std::string line = m.name + ": " + std::to_string((int)(m.confidence * 100)) + "%";
                            cv::putText(display, line,
                                        cv::Point((int)faces[i].x2 + 8, (int)faces[i].y1 + yOff),
                                        cv::FONT_HERSHEY_SIMPLEX, 0.5,
                                        m.confidence > 0.4f ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 165, 255),
                                        1);
                            yOff += 18;
                        }
                    }
                }
                // Only draw recognized faces
                if (recognized) {
                    drawFace(display, faces[i], label);
                    hasFace = true;
                }
            } catch (...) {}
        }

        // Keep last face/key for 'r'/'i' hotkeys
        if (!faces.empty()) {
            lastFace = faces[0];
            try {
                cv::Mat aligned = FaceAligner::align(frame, lastFace.keypoints);
                lastFeat = extractor.extract(aligned);
            } catch (...) {}
        }

        // Multi-frame registration: accumulate & finalize
        if (regCountdown > 0 && hasFace && lastFeat.size() == regAccum.size()) {
            for (size_t k = 0; k < lastFeat.size(); k++)
                regAccum[k] += lastFeat[k];
            regCountdown--;
            if (regCountdown == 0) {
                for (size_t k = 0; k < regAccum.size(); k++)
                    regAccum[k] /= REG_FRAMES;
                db.save(regName, regAccum);
                db.load();
                std::cout << "[Register] " << regName << " saved (avg of "
                          << REG_FRAMES << " frames)" << std::endl;
            }
        }
        if (regCountdown > 0) {
            cv::putText(display,
                        "Registering " + regName + " ... " + std::to_string(regCountdown),
                        cv::Point(10, display.rows - 20),
                        cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 255, 255), 2);
        }

        auto t5 = Clock::now();
        double frameMs = std::chrono::duration<double, std::milli>(t5 - t0).count();

        // Track metrics
        frameTimes.push_back(frameMs);
        detectTimes.push_back(detectMs);
        extractTimes.push_back(extractMs);
        if (frameTimes.size() > WINDOW) frameTimes.pop_front();
        if (detectTimes.size() > WINDOW) detectTimes.pop_front();
        if (extractTimes.size() > WINDOW) extractTimes.pop_front();

        double avgFrame  = std::accumulate(frameTimes.begin(),  frameTimes.end(),  0.0) / frameTimes.size();
        double avgDetect = std::accumulate(detectTimes.begin(), detectTimes.end(), 0.0) / detectTimes.size();
        double avgExtract= std::accumulate(extractTimes.begin(),extractTimes.end(),0.0) / extractTimes.size();
        float fps = 1000.0f / (float)std::max(avgFrame, 0.001);

        drawHUD(display, fps, (float)avgDetect, (float)avgExtract, (int)faces.size());

        cv::imshow("FaceRecognition Live", display);
        int key = cv::waitKey(1) & 0xFF;

        if (key == 'q' || key == 27) break;

        if (key == 's') {
            std::string fname = "snapshot_" + std::to_string((int)frameTimes.size()) + ".jpg";
            cv::imwrite(fname, display);
            std::cout << "[Snapshot] " << fname << std::endl;
        }

        if (key == 'r') {
            if (!hasFace) {
                std::cout << "[Register] No face. Wait for detection." << std::endl;
            } else if (regCountdown > 0) {
                std::cout << "[Register] Already capturing..." << std::endl;
            } else {
                regCountdown = REG_FRAMES;
                regAccum.assign(lastFeat.size(), 0.f);
                regName = "face_" + std::to_string(time(nullptr));
                std::cout << "[Register] Capturing " << REG_FRAMES << " frames for '"
                          << regName << "', slowly turn your head..." << std::endl;
            }
        }

        if (key == 'i') {
            if (!hasFace) {
                std::cout << "[Identify] No face." << std::endl;
            } else if (db.empty()) {
                std::cout << "[Identify] No registered faces. Press 'r'." << std::endl;
            } else {
                auto all = db.compareAll(lastFeat);
                std::cout << "[Identify] ";
                for (const auto& m : all) {
                    std::cout << m.name << "=" << (int)(m.confidence * 100) << "% ";
                }
                std::cout << std::endl;
            }
        }
    }

    cap.release();
    cv::destroyAllWindows();
    return 0;
}

// ============================================================================
// Main
// ============================================================================
int main(int argc, char** argv) {
    if (argc < 2) { printUsage(argv[0]); return 1; }

    try {
        std::string cmd = argv[1];

        std::string modelDir = "../../models/ncnn_models/";
        if (!fs::exists(modelDir + "det_500m_dyn.param"))
            modelDir = "../models/ncnn_models/";
        if (!fs::exists(modelDir + "det_500m_dyn.param"))
            modelDir = "models/ncnn_models/";

        std::string detParam = modelDir + "det_500m_dyn.param";
        std::string detBin   = modelDir + "det_500m_dyn.bin";
        std::string recParam = modelDir + "w600k_mbf.param";
        std::string recBin   = modelDir + "w600k_mbf.bin";

        std::cout << "[Init] Loading models..." << std::endl;
        FaceDetector detector(detParam.c_str(), detBin.c_str());
        FeatureExtractor extractor(recParam.c_str(), recBin.c_str());
        FeatureDB db;
        std::cout << "[Init] Models loaded." << std::endl;

        if (cmd == "live") {
            return cmdLive(detector, extractor, db);
        }
        if (cmd == "register") {
            if (argc < 4) { printUsage(argv[0]); return 1; }
            return cmdRegister(argv[2], argv[3], detector, extractor, db);
        }
        if (cmd == "identify") {
            return cmdIdentify(argv[2], detector, extractor, db);
        }
        if (cmd == "compare") {
            if (argc < 4) { printUsage(argv[0]); return 1; }
            return cmdCompare(argv[2], argv[3], detector, extractor);
        }
        if (cmd == "test") {
            if (argc < 3) { printUsage(argv[0]); return 1; }
            return cmdTest(argv[2], detector, extractor);
        }
        return cmdTest(argv[2], detector, extractor);
    }
    catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << std::endl;
        return 1;
    }
}
