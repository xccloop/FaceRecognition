# OpenCV / V4L2

## 在这个项目中的作用

OpenCV 是树莓派上的图像处理库，负责摄像头采集、图像缩放、颜色转换、JPEG 编解码。底层通过 V4L2（Video4Linux2）驱动 USB 摄像头。

对应代码：`Linux/Model/src/main.cpp`、`facedetector.cpp`、`facealigner.cpp`

## 项目中用到的具体知识

### 摄像头采集

```cpp
// main.cpp — cmdRun()
cv::VideoCapture cap(0);             // 打开 /dev/video0
cap.set(cv::CAP_PROP_FRAME_WIDTH, 320);
cap.set(cv::CAP_PROP_FRAME_HEIGHT, 240);

cv::Mat frame;
cap >> frame;                        // 读取一帧（BGR 格式）
```

- `cv::VideoCapture(0)` 对应 Linux 下的 `/dev/video0`
- 设置 320×240 分辨率以降低推理计算量
- 读取到的帧是 BGR 三通道格式（OpenCV 默认）

### 摄像头断线恢复

```cpp
if (frame.empty()) {
    cap.release();                                 // 释放设备
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    cap.open(0);                                   // 重新打开
    cap.set(cv::CAP_PROP_FRAME_WIDTH, 320);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 240);
    continue;
}
```

USB 摄像头可能因为接触不良、驱动异常等原因断线，这里做了自动重连。

### 图像缩放

```cpp
// 检测前：max-side 缩放
cv::resize(bgr, resized, cv::Size(nw, nh));

// MJPEG 推流前：放大到 640×480
cv::resize(frame, display, cv::Size(640, 480), 0, 0, cv::INTER_LINEAR);
```

### Padding（检测预处理）

```cpp
// facedetector.cpp
cv::copyMakeBorder(resized, img, pad_top, pad_bottom, pad_left, pad_right,
                   cv::BORDER_CONSTANT, cv::Scalar(128, 128, 128));
```

用灰色（128,128,128）填充到 32 的倍数。

### 仿射变换（人脸对齐）

```cpp
// facealigner.cpp — FaceAligner::align()
cv::Mat M = cv::estimateAffinePartial2D(src_pts, ref);  // 估算仿射矩阵
cv::warpAffine(src, aligned, M, cv::Size(112, 112));     // 执行仿射变换
```

通过 5 个关键点计算仿射变换矩阵，将人脸"摆正"到 112×112。

### JPEG 编码（MJPEG 推流）

```cpp
std::vector<uchar> jpg;
cv::imencode(".jpg", display, jpg, {cv::IMWRITE_JPEG_QUALITY, 30});
```

质量 30 压缩——视频流不需要高画质，减小带宽。

### 图片文件读取

```cpp
// CLI 命令
cv::Mat img = cv::imread(imgPath);   // 读取 JPEG/PNG 文件
```

## 要学到什么程度

- 理解 `cv::VideoCapture` 和 `/dev/video0` 的关系
- 知道 OpenCV 默认使用 BGR 颜色顺序（不是 RGB）
- 理解仿射变换的基本概念：旋转+缩放+平移
- 知道 V4L2 是 Linux 内核的视频设备驱动框架
