#include "facedetector.h"
#include "facealigner.h"
#include "featureextractor.h"
#include "uart_protocol.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// POSIX serial
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// ============================================================================
// MJPEGServer — lightweight HTTP MJPEG streamer (in-process, no file I/O)
// ============================================================================
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <iconv.h>

// UTF-8 → GBK converter (STM32 LCD uses GBK font)
static std::string utf8_to_gbk(const std::string& utf8) {
    iconv_t cd = iconv_open("GBK", "UTF-8");
    if (cd == (iconv_t)-1) return utf8;
    size_t in_len = utf8.size();
    size_t out_len = in_len * 2 + 8;
    std::vector<char> out(out_len);
    char* in_ptr = const_cast<char*>(utf8.data());
    char* out_ptr = out.data();
    size_t ret = iconv(cd, &in_ptr, &in_len, &out_ptr, &out_len);
    iconv_close(cd);
    if (ret == (size_t)-1) return utf8;
    return std::string(out.data(), out_ptr - out.data());
}

class MJPEGServer {
public:
    void start(int port = 8080) {
        port_ = port;
        running_ = true;
        thread_ = std::thread(&MJPEGServer::serve, this);
    }

    void stop() {
        running_ = false;
        // Connect to unblock accept()
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) {
            struct sockaddr_in addr = {};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port_);
            addr.sin_addr.s_addr = inet_addr("127.0.0.1");
            connect(fd, (struct sockaddr*)&addr, sizeof(addr));
            close(fd);
        }
        if (thread_.joinable()) thread_.join();
    }

    void updateFrame(const std::vector<uchar>& jpg) {
        std::lock_guard<std::mutex> lock(mtx_);
        latestJpeg_ = jpg;
    }

private:
    int port_ = 8080;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex mtx_;
    std::vector<uchar> latestJpeg_;

    void serve() {
        int server = socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1;
        setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        addr.sin_addr.s_addr = INADDR_ANY;
        bind(server, (struct sockaddr*)&addr, sizeof(addr));
        listen(server, 5);

        struct timeval tv = {1, 0};
        std::cout << "[MJPEG] http://0.0.0.0:" << port_ << "/video" << std::endl;

        while (running_) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(server, &fds);
            int n = select(server + 1, &fds, nullptr, nullptr, &tv);
            if (n <= 0 || !FD_ISSET(server, &fds)) continue;

            int client = accept(server, nullptr, nullptr);
            if (client < 0) continue;

            int on = 1;
            setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
            struct timeval ct = {3, 0};
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &ct, sizeof(ct));
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &ct, sizeof(ct));

            // Read HTTP request (ignore content, just drain it)
            char buf[4096];
            recv(client, buf, sizeof(buf), 0);

            // Send header
            const char* header =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
                "Cache-Control: no-cache, no-store\r\n"
                "Pragma: no-cache\r\n"
                "Connection: close\r\n"
                "\r\n";
            send(client, header, strlen(header), MSG_NOSIGNAL);

            // Stream frames
            auto serveClient = [&](int fd) {
                std::vector<uchar> lastSent;
                while (running_) {
                    std::vector<uchar> frame;
                    {
                        std::lock_guard<std::mutex> lock(mtx_);
                        frame = latestJpeg_;
                    }
                    if (!frame.empty() && frame != lastSent) {
                        const char* part = "--frame\r\nContent-Type: image/jpeg\r\n\r\n";
                        send(fd, part, strlen(part), MSG_NOSIGNAL);
                        send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
                        send(fd, "\r\n", 2, MSG_NOSIGNAL);
                        lastSent = frame;
                    }
                    usleep(40000); // ~25fps max
                }
            };
            serveClient(client);
            close(client);
        }
        close(server);
    }
};

// ============================================================================
// SerialPort — RAII wrapper over POSIX termios
// ============================================================================
class SerialPort {
public:
    bool open(const std::string& device, int baud = 115200) {
        fd_ = ::open(device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd_ < 0) {
            std::cerr << "[Serial] Cannot open " << device << ": " << strerror(errno) << std::endl;
            return false;
        }

        struct termios tty;
        memset(&tty, 0, sizeof(tty));
        if (tcgetattr(fd_, &tty) != 0) { close(); return false; }

        speed_t speed = B115200;
        switch (baud) {
            case 9600:   speed = B9600;   break;
            case 57600:  speed = B57600;  break;
            case 115200: speed = B115200; break;
            default:     speed = B115200; break;
        }

        cfsetospeed(&tty, speed);
        cfsetispeed(&tty, speed);

        tty.c_cflag |= (CLOCAL | CREAD);
        tty.c_cflag &= ~CSIZE;
        tty.c_cflag |= CS8;
        tty.c_cflag &= ~PARENB;
        tty.c_cflag &= ~CSTOPB;
        tty.c_cflag &= ~CRTSCTS;

        tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
        tty.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL);
        tty.c_oflag &= ~OPOST;

        tty.c_cc[VMIN] = 0;
        tty.c_cc[VTIME] = 1;  // 100ms read timeout

        if (tcsetattr(fd_, TCSANOW, &tty) != 0) { close(); return false; }

        std::cout << "[Serial] Opened " << device << " @" << baud << "bps" << std::endl;
        return true;
    }

    void close() {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    ~SerialPort() { close(); }

    bool isOpen() const { return fd_ >= 0; }

    int write(const uint8_t* data, size_t len) {
        if (fd_ < 0) return -1;
        ssize_t n = ::write(fd_, data, len);
        if (n < 0) return -1;
        tcdrain(fd_);
        return (int)n;
    }

    int write(const std::vector<uint8_t>& data) {
        return write(data.data(), data.size());
    }

    // Non-blocking read, returns number of bytes read (0 = none, -1 = error)
    int read(uint8_t* buf, size_t maxlen) {
        if (fd_ < 0) return -1;
        ssize_t n = ::read(fd_, buf, maxlen);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        return (int)n;
    }

    void flush() {
        if (fd_ >= 0) tcflush(fd_, TCIOFLUSH);
    }

private:
    int fd_ = -1;
};

// ============================================================================
// FeatureDB — persistent feature database (.bin files)
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

    void reload() {
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

    bool empty() const { std::lock_guard<std::mutex> lock(mtx_); return registry_.empty(); }
    size_t size() const { std::lock_guard<std::mutex> lock(mtx_); return registry_.size(); }
    const std::vector<float>& feature(size_t idx) const {
        std::lock_guard<std::mutex> lock(mtx_); return registry_[idx].feature;
    }
    const std::string& name(size_t idx) const {
        std::lock_guard<std::mutex> lock(mtx_); return registry_[idx].name;
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
// Tracking state
// ============================================================================
struct TrackedFace {
    int    id;
    float  x1, y1, x2, y2;
    float  score;
    float  keypoints[5][2];
    std::vector<float> feature;
    std::string name;
    float  similarity = 0;
    double lastSeen = 0;
    int    confirmCount = 0;
    bool   resultSent = false;
};

static float iou(const TrackedFace& a, const TrackedFace& b) {
    float x1 = std::max(a.x1, b.x1), y1 = std::max(a.y1, b.y1);
    float x2 = std::min(a.x2, b.x2), y2 = std::min(a.y2, b.y2);
    float inter = std::max(0.f, x2 - x1) * std::max(0.f, y2 - y1);
    float areaA = (a.x2 - a.x1) * (a.y2 - a.y1);
    float areaB = (b.x2 - b.x1) * (b.y2 - b.y1);
    return inter / (areaA + areaB - inter + 1e-6f);
}

// ============================================================================
// Helpers
// ============================================================================

static void printUsage(const char* prog) {
    std::cerr << "Usage:" << std::endl;
    std::cerr << "  " << prog << " run                  (production: camera + UART)" << std::endl;
    std::cerr << "  " << prog << " register <img> <name>" << std::endl;
    std::cerr << "  " << prog << " identify <img>" << std::endl;
    std::cerr << "  " << prog << " compare <img1> <img2>" << std::endl;
    std::cerr << "  " << prog << " test <img>" << std::endl;
}

static FaceInfo requireSingleFace(const std::vector<FaceInfo>& faces) {
    if (faces.empty()) throw std::runtime_error("No face detected");
    if (faces.size() > 1) {
        std::cout << "[INFO] " << faces.size() << " faces, using best" << std::endl;
    }
    auto best = faces[0];
    for (size_t i = 1; i < faces.size(); i++)
        if (faces[i].score > best.score) best = faces[i];
    return best;
}

// ============================================================================
// CLI: register / identify / compare / test
// ============================================================================
static int cmdRegister(const std::string& imgPath, const std::string& name,
                       FaceDetector& det, FeatureExtractor& ext, FeatureDB& db) {
    cv::Mat img = cv::imread(imgPath);
    if (img.empty()) throw std::runtime_error("Failed to load: " + imgPath);
    auto faces = det.detect(img, 0.3f);
    auto face = requireSingleFace(faces);
    cv::Mat aligned = FaceAligner::align(img, face.keypoints);
    auto feat = ext.extract(aligned);
    db.save(name, feat);
    std::cout << "[OK] Registered: " << name << std::endl;
    return 0;
}

static int cmdIdentify(const std::string& imgPath,
                       FaceDetector& det, FeatureExtractor& ext, FeatureDB& db) {
    db.load();
    if (db.empty()) throw std::runtime_error("No registered faces.");
    cv::Mat img = cv::imread(imgPath);
    if (img.empty()) throw std::runtime_error("Failed to load: " + imgPath);
    auto faces = det.detect(img, 0.3f);
    auto face = requireSingleFace(faces);
    cv::Mat aligned = FaceAligner::align(img, face.keypoints);
    auto feat = ext.extract(aligned);
    auto match = db.identify(feat, 0.4f);
    if (match.name != "")
        std::cout << "[OK] " << match.name << " (" << match.confidence << ")" << std::endl;
    else
        std::cout << "[NO MATCH] best=" << match.confidence << std::endl;
    return 0;
}

static int cmdCompare(const std::string& p1, const std::string& p2,
                      FaceDetector& det, FeatureExtractor& ext) {
    cv::Mat i1 = cv::imread(p1), i2 = cv::imread(p2);
    if (i1.empty() || i2.empty()) throw std::runtime_error("Failed to load image.");
    auto f1 = requireSingleFace(det.detect(i1, 0.3f));
    auto f2 = requireSingleFace(det.detect(i2, 0.3f));
    float sim = FeatureExtractor::cosineSimilarity(
        ext.extract(FaceAligner::align(i1, f1.keypoints)),
        ext.extract(FaceAligner::align(i2, f2.keypoints)));
    std::cout << "Similarity: " << sim << " [" << (sim > 0.4f ? "SAME" : "DIFF") << "]" << std::endl;
    return 0;
}

static int cmdTest(const std::string& imgPath,
                   FaceDetector& det, FeatureExtractor& ext) {
    cv::Mat img = cv::imread(imgPath);
    if (img.empty()) throw std::runtime_error("Failed to load: " + imgPath);
    std::cout << "Image: " << img.cols << "x" << img.rows << std::endl;
    auto faces = det.detect(img, 0.3f);
    std::cout << "Detected " << faces.size() << " face(s)" << std::endl;
    return 0;
}

// ============================================================================
// cmdRun — headless production mode (camera → ncnn → UART → STM32)
// ============================================================================
static int cmdRun(FaceDetector& detector, FeatureExtractor& extractor, FeatureDB& db,
                  const std::string& uartDev, int uartBaud, int confirmFrames,
                  float recThreshold, int skipFrames, int detMaxSide) {
    // ── MJPEG server ──
    MJPEGServer mjpeg;
    mjpeg.start(8080);

    // ── UART ──
    SerialPort uart;
    if (!uart.open(uartDev, uartBaud)) {
        std::cerr << "[Run] UART failed. Running without STM32 communication." << std::endl;
    }
    uart.flush();

    // ── Camera ──
    cv::VideoCapture cap(0);
    if (!cap.isOpened()) {
        std::cerr << "[Run] Cannot open camera." << std::endl;
        return 1;
    }
    cap.set(cv::CAP_PROP_FRAME_WIDTH, 320);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 240);

    cv::Mat frame;
    cap >> frame;
    if (frame.empty()) { std::cerr << "[Run] Cannot read camera." << std::endl; return 1; }
    std::cout << "[Run] Camera: " << frame.cols << "x" << frame.rows << std::endl;

    // ── Feature DB ──
    db.load();
    auto lastDbReload = Clock::now();

    // ── State ──
    std::vector<TrackedFace> tracks;
    int nextTrackId = 0;
    int frameCount = 0;
    std::string lastUartCmd = "";       // debounce: avoid re-sending same cmd
    auto lastUartTime = Clock::now();
    auto lastHeartbeat = Clock::now();
    auto lastNoFaceSent = Clock::now();

    // UART RX state (read in main loop via non-blocking serial read)
    FrameParser rxParser;

    // Performance tracking
    std::deque<double> detectMs, extractMs;
    const size_t PERF_WINDOW = 30;
    auto lastLogTime = Clock::now();
    int lastLogFrame = 0;
    clock_t lastCpuTicks = 0;  // for CPU% calc

    // UART is non-blocking, RX runs in main loop

    std::cout << "[Run] Running. Press Ctrl+C to stop." << std::endl;
    std::cout << "      confirm=" << confirmFrames
              << " skip=" << skipFrames
              << " rec_thresh=" << recThreshold << std::endl;

    std::atomic<bool> running{true};

    while (running) {
        try {
        auto t0 = Clock::now();

        // ── Read frame ──
        cap >> frame;
        if (frame.empty()) {
            std::cerr << "[Run] Camera empty frame, reconnecting..." << std::endl;
            cap.release();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            cap.open(0);
            cap.set(cv::CAP_PROP_FRAME_WIDTH, 320);
            cap.set(cv::CAP_PROP_FRAME_HEIGHT, 240);
            continue;
        }
        frameCount++;

        // Send frame to in-process MJPEG server (no file I/O)
        {
            cv::Mat display;
            cv::resize(frame, display, cv::Size(640, 480), 0, 0, cv::INTER_LINEAR);
            std::vector<uchar> jpg;
            cv::imencode(".jpg", display, jpg, {cv::IMWRITE_JPEG_QUALITY, 30});
            mjpeg.updateFrame(jpg);
        }

        // ── Periodic DB reload (every 5s) ──
        {
            auto now = Clock::now();
            double dt = std::chrono::duration<double>(now - lastDbReload).count();
            if (dt > 5.0) {
                size_t before = db.size();
                db.reload();
                if (db.size() != before)
                    std::cout << "[Run] FeatureDB reloaded: " << db.size() << " faces" << std::endl;
                lastDbReload = now;
            }
        }

        // ── Inference every N frames ──
        bool doInfer = (frameCount % skipFrames == 1) || (tracks.empty() && frameCount % 2 == 1);
        double dMs = 0, eMs = 0;

        if (doInfer) {
            auto t1 = Clock::now();
            auto detections = detector.detect(frame, 0.3f, 0.4f, detMaxSide);
            dMs = std::chrono::duration<double, std::milli>(Clock::now() - t1).count();

            // ── Match detections to existing tracks (IOU) ──
            int nDet = (int)detections.size();
            int nTrk = (int)tracks.size();
            std::vector<int> matchedTo(nTrk, -1);
            std::vector<bool> detUsed(nDet, false);

            // Simple greedy IOU matching
            for (int di = 0; di < nDet; di++) {
                float bestIou = 0.3f;
                int bestTi = -1;
                for (int ti = 0; ti < nTrk; ti++) {
                    if (matchedTo[ti] >= 0) continue;
                    TrackedFace tmp;
                    tmp.x1 = detections[di].x1; tmp.y1 = detections[di].y1;
                    tmp.x2 = detections[di].x2; tmp.y2 = detections[di].y2;
                    float i = iou(tracks[ti], tmp);
                    if (i > bestIou) { bestIou = i; bestTi = ti; }
                }
                if (bestTi >= 0) { matchedTo[bestTi] = di; detUsed[di] = true; }
            }

            // ── Update matched tracks ──
            for (int ti = 0; ti < nTrk; ti++) {
                if (matchedTo[ti] < 0) continue;
                auto& det = detections[matchedTo[ti]];
                auto& trk = tracks[ti];
                trk.x1 = det.x1; trk.y1 = det.y1;
                trk.x2 = det.x2; trk.y2 = det.y2;
                trk.score = det.score;
                for (int p = 0; p < 5; p++) {
                    trk.keypoints[p][0] = det.keypoints[p][0];
                    trk.keypoints[p][1] = det.keypoints[p][1];
                }
                trk.lastSeen = std::chrono::duration<double>(
                    Clock::now().time_since_epoch()).count();

                // Re-extract every 2s
                auto tEx = Clock::now();
                double elapsed = std::chrono::duration<double>(tEx.time_since_epoch()).count();
                double lastEx = std::chrono::duration<double>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                // Always extract on update for simplicity
                {
                    cv::Mat aligned = FaceAligner::align(frame, det.keypoints);
                    auto feat = extractor.extract(aligned);
                    trk.feature = feat;
                    auto match = db.identify(feat, recThreshold);
                    if (match.name != "") {
                        if (trk.name == match.name) trk.confirmCount++;
                        else { trk.name = match.name; trk.confirmCount = 1; trk.resultSent = false; }
                        trk.similarity = match.confidence;
                    } else {
                        trk.name = match.name;  // ""
                        trk.confirmCount = 0;
                        trk.resultSent = false;
                    }
                }
                eMs += std::chrono::duration<double, std::milli>(
                    Clock::now() - tEx).count();
            }

            // ── Create new tracks for unmatched detections ──
            for (int di = 0; di < nDet; di++) {
                if (detUsed[di]) continue;
                auto& det = detections[di];
                TrackedFace trk;
                trk.id = nextTrackId++;
                trk.x1 = det.x1; trk.y1 = det.y1; trk.x2 = det.x2; trk.y2 = det.y2;
                trk.score = det.score;
                for (int p = 0; p < 5; p++) { trk.keypoints[p][0] = det.keypoints[p][0]; trk.keypoints[p][1] = det.keypoints[p][1]; }
                trk.lastSeen = std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
                tracks.push_back(std::move(trk));
            }
        }

        // ── Clean up stale tracks (>2s unseen) ──
        double nowSec = std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
        tracks.erase(std::remove_if(tracks.begin(), tracks.end(),
            [nowSec](const TrackedFace& t) { return nowSec - t.lastSeen > 2.0; }),
            tracks.end());

        // ── Determine UART command ──
        std::string uartCmd;
        std::string uartName;
        int faceState = 0; // 0=none, 1=identified, 2=unknown, 3=multi

        if (tracks.empty()) {
            faceState = 0;
        } else if (tracks.size() > 1) {
            faceState = 3;
        } else {
            auto& trk = tracks[0];
            if (trk.confirmCount >= confirmFrames && trk.name != "" && !trk.resultSent) {
                faceState = 1;
                uartName = trk.name;
                trk.resultSent = true;
            } else if (trk.name == "" && trk.confirmCount == 0) {
                faceState = 2;
            } else if (trk.name != "" && trk.resultSent) {
                faceState = 1; // already sent, don't re-send
            }
        }

        // Record state string for logging
        switch (faceState) {
        case 1: uartCmd = "IDENTIFY:" + uartName; break;
        case 2: uartCmd = "UNKNOWN"; break;
        case 3: uartCmd = "MULTIFACE"; break;
        default: uartCmd = "NOFACE"; break;
        }

        // ── Send UART frame (debounced) ──
        if (uart.isOpen()) {
            auto now = Clock::now();
            double sinceLastCmd = std::chrono::duration<double>(now - lastUartTime).count();

            if (uartCmd != lastUartCmd || sinceLastCmd > 3.0) {
                bool shouldSend = false;
                std::vector<uint8_t> frame;

                if (faceState == 1 && !uartName.empty() &&
                    (uartCmd != lastUartCmd || sinceLastCmd > 3.0)) {
                    std::string gbkName = utf8_to_gbk(uartName);
                    frame = frame_encode(CMD_IDENTIFY, DIR_PI_TO_STM32,
                                         (const uint8_t*)gbkName.c_str(),
                                         (uint16_t)gbkName.size());
                    shouldSend = true;
                    std::cout << "[UART] → IDENTIFY " << uartName << std::endl;
                } else if (faceState == 2 && lastUartCmd != "UNKNOWN") {
                    frame = frame_encode(CMD_UNKNOWN, DIR_PI_TO_STM32, nullptr, 0);
                    shouldSend = true;
                    std::cout << "[UART] → UNKNOWN" << std::endl;
                } else if (faceState == 3 && lastUartCmd != "MULTIFACE") {
                    frame = frame_encode(CMD_MULTIFACE, DIR_PI_TO_STM32, nullptr, 0);
                    shouldSend = true;
                    std::cout << "[UART] → MULTIFACE" << std::endl;
                } else if (faceState == 0) {
                    double sinceNoFace = std::chrono::duration<double>(now - lastNoFaceSent).count();
                    if (sinceNoFace > 2.0) {
                        frame = frame_encode(CMD_NOFACE, DIR_PI_TO_STM32, nullptr, 0);
                        shouldSend = true;
                        lastNoFaceSent = now;
                        std::cout << "[UART] → NOFACE" << std::endl;
                    }
                }

                if (shouldSend && !frame.empty()) {
                    uart.write(frame);
                    lastUartTime = now;
                    if (faceState != 0) lastUartCmd = uartCmd;
                }
            }

            // ── Heartbeat every 5s ──
            double sinceHb = std::chrono::duration<double>(now - lastHeartbeat).count();
            if (sinceHb >= 5.0) {
                auto hbFrame = frame_encode(CMD_HEARTBEAT, DIR_PI_TO_STM32, nullptr, 0);
                uart.write(hbFrame);
                lastHeartbeat = now;
            }

            // ── Read UART RX ──
            uint8_t rxBuf[256];
            int nr = uart.read(rxBuf, sizeof(rxBuf));
            for (int i = 0; i < nr; i++) {
                ParsedFrame pf;
                if (rxParser.feed(rxBuf[i], pf)) {
                    std::cout << "[UART] ← " << cmd_name(pf.cmd);
                    if (pf.len > 0) {
                        std::string dataStr((char*)pf.data, pf.len);
                        std::cout << " data=" << dataStr;
                    }
                    std::cout << std::endl;
                }
            }
        }

        // ── Performance ──
        detectMs.push_back(dMs);
        extractMs.push_back(eMs);
        if (detectMs.size() > PERF_WINDOW) detectMs.pop_front();
        if (extractMs.size() > PERF_WINDOW) extractMs.pop_front();

        double avgDet = std::accumulate(detectMs.begin(), detectMs.end(), 0.0) / detectMs.size();
        double avgExt = std::accumulate(extractMs.begin(), extractMs.end(), 0.0) / extractMs.size();
        double totalMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();

        // Log every 30 frames (FPS + CPU temp + CPU%)
        if (frameCount % 30 == 0) {
            auto now = Clock::now();
            double elapsed = std::chrono::duration<double>(now - lastLogTime).count();
            double fps = (frameCount - lastLogFrame) / std::max(elapsed, 0.001);

            // CPU temperature (Pi: /sys/class/thermal/thermal_zone0/temp, millidegrees)
            double cpuTemp = 0;
            {
                std::ifstream tf("/sys/class/thermal/thermal_zone0/temp");
                int raw; tf >> raw; cpuTemp = raw / 1000.0;
            }

            // CPU% from /proc/self/stat
            double cpuPct = 0;
            {
                std::ifstream sf("/proc/self/stat");
                std::string line; std::getline(sf, line);
                // Find closing ')' after process name, then read utime(14) stime(15)
                size_t rp = line.rfind(')');
                if (rp != std::string::npos) {
                    std::istringstream iss(line.substr(rp + 2));
                    std::string state; unsigned long utime = 0, stime = 0;
                    iss >> state; // state
                    for (int i = 0; i < 10; i++) { std::string x; iss >> x; } // skip state..cmajflt
                    iss >> utime >> stime;
                    auto ticks = utime + stime;
                    if (lastCpuTicks > 0) {
                        long hz = sysconf(_SC_CLK_TCK);
                        cpuPct = 100.0 * (ticks - lastCpuTicks) / (hz * elapsed);
                    }
                    lastCpuTicks = ticks;
                }
            }

            lastLogTime = now;
            lastLogFrame = frameCount;
            std::cout << "[Run] #" << frameCount
                      << " fps=" << (int)fps
                      << " detect=" << (int)avgDet << "ms"
                      << " cpu=" << (int)cpuTemp << "C " << (int)cpuPct << "%"
                      << " tracks=" << tracks.size()
                      << " uart=" << (uart.isOpen() ? uartCmd : "OFF")
                      << std::endl;
        }

        // Brief yield when idle to reduce CPU
        if (tracks.empty() && !doInfer) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        } catch (const std::exception& e) {
            std::cerr << "[Run] Error: " << e.what() << " — recovering..." << std::endl;
            // Try to reopen camera if needed
            if (!cap.isOpened()) {
                cap.open(0);
                cap.set(cv::CAP_PROP_FRAME_WIDTH, 640);
                cap.set(cv::CAP_PROP_FRAME_HEIGHT, 480);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    cap.release();
    uart.close();
    mjpeg.stop();
    std::cout << "[Run] Stopped." << std::endl;
    return 0;
}

// ============================================================================
// Main
// ============================================================================
int main(int argc, char** argv) {
    if (argc < 2) { printUsage(argv[0]); return 1; }

    try {
        std::string cmd = argv[1];

        // Model path discovery
        std::string modelDir = "models/ncnn_models/";
        std::string detParam = modelDir + "det_500m_dyn.param";
        std::string detBin   = modelDir + "det_500m_dyn.bin";
        std::string recParam = modelDir + "w600k_mbf_opt.param";
        std::string recBin   = modelDir + "w600k_mbf_opt.bin";

        if (!fs::exists(detParam)) detParam = modelDir + "det_500m.param";
        if (!fs::exists(detBin))   detBin   = modelDir + "det_500m.bin";
        if (!fs::exists(recParam)) recParam = modelDir + "w600k_mbf.param";
        if (!fs::exists(recBin))   recBin   = modelDir + "w600k_mbf.bin";

        std::cout << "[Init] Loading models..." << std::endl;
        std::cout << "  det: " << detParam << std::endl;
        std::cout << "  rec: " << recParam << std::endl;

        FaceDetector detector(detParam.c_str(), detBin.c_str());
        FeatureExtractor extractor(recParam.c_str(), recBin.c_str());
        FeatureDB db;
        std::cout << "[Init] Models loaded." << std::endl;

        // ── Command dispatch ──
        if (cmd == "run") {
            return cmdRun(detector, extractor, db, "/dev/serial0", 115200, 3, 0.4f, 3, 480);
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
        // Default: treat as test
        return cmdTest(argv[1], detector, extractor);
    }
    catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << std::endl;
        return 1;
    }
}
