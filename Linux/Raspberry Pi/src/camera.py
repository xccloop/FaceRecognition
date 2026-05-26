"""摄像头采集模块 — 线程安全帧队列 + 采集循环 + MJPEG 帧生成。

设计：
  FrameQueue   — 容量 1，push 覆盖旧帧，pop 阻塞等待新帧（非空时才阻塞）
  ResultQueue  — 容量 N，FIFO，push 非阻塞（满则丢弃最旧），pop 阻塞等待
  CameraCapture — 封装 cv2.VideoCapture / picamera2，循环读帧

线程安全：使用 threading.Lock + threading.Condition。
支持安全退出：通过 threading.Event 通知各线程停止。
"""

from __future__ import annotations

import threading
import time
from collections import deque
from typing import Optional, Tuple, List

import numpy as np


# ══════════════════════════════════════════════════════════
# FrameQueue — 容量 1，覆盖旧帧
# ══════════════════════════════════════════════════════════

class FrameQueue:
    """容量为 1 的帧队列。

    push(frame) — 覆盖旧帧，通知等待的消费者。
    pop(timeout) — 阻塞等待新帧，返回 (frame, timestamp) 或 None（超时）。
    """

    def __init__(self):
        self._lock = threading.Lock()
        self._cond = threading.Condition(self._lock)
        self._frame: Optional[np.ndarray] = None
        self._jpeg: Optional[bytes] = None
        self._timestamp: float = 0.0
        self._has_new = False

    def push(self, frame: np.ndarray, jpeg: Optional[bytes] = None) -> None:
        """推入一帧（覆盖旧帧）。

        Args:
            frame: BGR 图像 (H, W, 3) uint8 numpy 数组。
            jpeg: 可选的 JPEG 编码字节（用于 MJPEG 流）。
        """
        with self._cond:
            self._frame = frame.copy()  # 防御性拷贝，避免采集线程覆盖
            self._jpeg = jpeg
            self._timestamp = time.time()
            self._has_new = True
            self._cond.notify_all()

    def pop(self, timeout: Optional[float] = None) -> Optional[Tuple[np.ndarray, float, Optional[bytes]]]:
        """阻塞等待新帧。

        Args:
            timeout: 超时秒数，None 表示无限等待。

        Returns:
            (frame, timestamp, jpeg) 或超时返回 None。
        """
        with self._cond:
            if not self._has_new:
                if not self._cond.wait(timeout):
                    return None
                if not self._has_new:
                    return None
            self._has_new = False
            return (self._frame, self._timestamp, self._jpeg)

    def peek(self) -> Optional[Tuple[np.ndarray, float, Optional[bytes]]]:
        """非阻塞获取最新帧（不清除 new 标记，可用于 MJPEG 流）。"""
        with self._lock:
            if self._frame is None:
                return None
            return (self._frame, self._timestamp, self._jpeg)

    def empty(self) -> bool:
        with self._lock:
            return self._frame is None


# ══════════════════════════════════════════════════════════
# ResultQueue — 容量 N，FIFO，满时丢弃最旧
# ══════════════════════════════════════════════════════════

class ResultQueue:
    """容量为 N 的 FIFO 结果队列。

    push(result) — 非阻塞，满则丢弃最旧的。
    pop(timeout) — 阻塞等待新结果。
    """

    def __init__(self, maxsize: int = 16):
        self._lock = threading.Lock()
        self._cond = threading.Condition(self._lock)
        self._queue: deque = deque(maxlen=maxsize)
        self._maxsize = maxsize

    def push(self, item) -> bool:
        """推入结果。满时丢弃最旧的，返回 True；丢弃返回 False。"""
        with self._cond:
            dropped = len(self._queue) >= self._maxsize
            self._queue.append(item)
            self._cond.notify()
            return not dropped

    def pop(self, timeout: Optional[float] = None) -> Optional[dict]:
        """阻塞等待结果。

        Args:
            timeout: 超时秒数，None 表示无限等待。

        Returns:
            结果字典或超时返回 None。
        """
        with self._cond:
            while not self._queue:
                if not self._cond.wait(timeout):
                    return None
            return self._queue.popleft()

    def empty(self) -> bool:
        with self._lock:
            return len(self._queue) == 0


# ══════════════════════════════════════════════════════════
# 帧数据结构
# ══════════════════════════════════════════════════════════

class FrameData:
    """单帧数据。"""
    __slots__ = ("bgr", "jpeg", "timestamp")
    def __init__(self, bgr: np.ndarray, jpeg: Optional[bytes] = None):
        self.bgr = bgr
        self.jpeg = jpeg
        self.timestamp = time.time()


# ══════════════════════════════════════════════════════════
# 摄像头采集器
# ══════════════════════════════════════════════════════════

class CameraCapture:
    """摄像头采集器。

    支持两种后端：
    - opencv: cv2.VideoCapture（适用于所有 Pi 型号，Buster/Bullseye）
    - picamera2: libcamera-based（Pi 5 / Bookworm+ 推荐）

    用法：
        cap = CameraCapture(config.camera)
        cap.open()
        for frame_data in cap:
            frame_q.push(frame_data.bgr, frame_data.jpeg)
            if stop_event.is_set():
                break
        cap.release()
    """

    def __init__(self, camera_config):
        """
        Args:
            camera_config: CameraConfig dataclass 实例。
        """
        self.cfg = camera_config
        self._cap = None
        self._picam2 = None
        self._running = False

    def open(self) -> bool:
        """打开摄像头设备。

        Returns:
            True 成功，False 失败。
        """
        backend = self.cfg.backend.lower()
        if backend == "picamera2":
            return self._open_picamera2()
        else:
            return self._open_opencv()

    def _open_opencv(self) -> bool:
        import cv2
        self._cap = cv2.VideoCapture(self.cfg.device)
        if not self._cap.isOpened():
            print(f"[Camera] 错误: 无法打开摄像头 device={self.cfg.device}")
            return False

        self._cap.set(cv2.CAP_PROP_FRAME_WIDTH, self.cfg.width)
        self._cap.set(cv2.CAP_PROP_FRAME_HEIGHT, self.cfg.height)
        self._cap.set(cv2.CAP_PROP_FPS, self.cfg.fps)

        # 验证实际分辨率
        actual_w = int(self._cap.get(cv2.CAP_PROP_FRAME_WIDTH))
        actual_h = int(self._cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
        print(f"[Camera] opencv 后端, device={self.cfg.device}, "
              f"分辨率={actual_w}x{actual_h}")
        self._running = True
        return True

    def _open_picamera2(self) -> bool:
        try:
            from picamera2 import Picamera2
        except ImportError:
            print("[Camera] 错误: picamera2 未安装。请执行: pip install picamera2")
            print("[Camera] 回退到 opencv 后端...")
            return self._open_opencv()

        self._picam2 = Picamera2()
        config = self._picam2.create_still_configuration(
            main={"size": (self.cfg.width, self.cfg.height)},
        )
        self._picam2.configure(config)
        self._picam2.start()
        print(f"[Camera] picamera2 后端, 分辨率={self.cfg.width}x{self.cfg.height}")
        self._running = True
        return True

    def read(self) -> Optional[FrameData]:
        """读取一帧（含 JPEG 编码）。

        Returns:
            FrameData 或 None（读取失败/设备未打开）。
        """
        if not self._running:
            return None

        if self._picam2 is not None:
            return self._read_picamera2()
        else:
            return self._read_opencv()

    def _read_opencv(self) -> Optional[FrameData]:
        import cv2
        ret, bgr = self._cap.read()
        if not ret or bgr is None:
            return None

        # JPEG 编码
        ok, jpeg_buf = cv2.imencode(".jpg", bgr, [cv2.IMWRITE_JPEG_QUALITY, 60])
        jpeg = jpeg_buf.tobytes() if ok else None
        return FrameData(bgr, jpeg)

    def _read_picamera2(self) -> Optional[FrameData]:
        import cv2
        try:
            arr = self._picam2.capture_array()
            # picamera2 返回 RGB，转 BGR
            bgr = cv2.cvtColor(arr, cv2.COLOR_RGB2BGR)
            ok, jpeg_buf = cv2.imencode(".jpg", bgr, [cv2.IMWRITE_JPEG_QUALITY, 60])
            jpeg = jpeg_buf.tobytes() if ok else None
            return FrameData(bgr, jpeg)
        except Exception as e:
            print(f"[Camera] picamera2 读取错误: {e}")
            return None

    def release(self) -> None:
        """释放摄像头资源。"""
        self._running = False
        if self._cap is not None:
            self._cap.release()
            self._cap = None
        if self._picam2 is not None:
            try:
                self._picam2.stop()
            except Exception:
                pass
            self._picam2 = None
        print("[Camera] 已释放")

    def __iter__(self):
        return self

    def __next__(self) -> FrameData:
        if not self._running:
            raise StopIteration
        frame = self.read()
        if frame is None:
            raise StopIteration
        return frame


# ══════════════════════════════════════════════════════════
# 采集线程入口
# ══════════════════════════════════════════════════════════

def capture_loop(
    frame_q: FrameQueue,
    camera_cfg,
    stop_event: threading.Event,
    fps_limit: Optional[int] = None,
) -> None:
    """摄像头采集主循环（运行在独立线程）。

    Args:
        frame_q: FrameQueue 实例。
        camera_cfg: CameraConfig 实例。
        stop_event: 停止信号。
        fps_limit: 帧率上限（None=使用配置值）。
    """
    cap = CameraCapture(camera_cfg)
    if not cap.open():
        print("[Capture] 摄像头打开失败，采集线程退出")
        return

    target_fps = fps_limit if fps_limit else camera_cfg.fps
    frame_interval = 1.0 / target_fps if target_fps > 0 else 0.0
    last_push = 0.0

    print(f"[Capture] 采集线程启动, 目标={target_fps} FPS, 间隔={frame_interval:.3f}s")

    try:
        while not stop_event.is_set():
            frame_data = cap.read()
            if frame_data is None:
                time.sleep(0.01)
                continue

            # 帧率限制
            now = time.time()
            if now - last_push < frame_interval:
                time.sleep(max(0, frame_interval - (now - last_push)))
                now = time.time()

            frame_q.push(frame_data.bgr, frame_data.jpeg)
            last_push = now

    except Exception as e:
        print(f"[Capture] 采集线程异常: {e}")
    finally:
        cap.release()
        print("[Capture] 采集线程退出")
