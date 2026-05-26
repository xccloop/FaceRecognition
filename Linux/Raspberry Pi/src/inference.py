"""推理线程 — 人脸检测 → 跟踪 → 特征提取 → 匹配。

管线：
  FrameQueue.pop() → detect → (IOU 跟踪) → align → extract → FeatureDB.match()
  → ResultQueue.push()

性能优化：
  - 跳帧：每 N 帧推理一次，其余复用跟踪结果
  - IOU 跟踪：用交并比匹配前后帧中同一个人脸，跨帧复用特征
  - 确认机制：连续 K 帧识别同一人才发送结果（防抖）
"""

from __future__ import annotations

import threading
import time
from typing import Optional, List, Dict, Tuple

import numpy as np

from .camera import FrameQueue, ResultQueue
from .recognition import RecognitionEngine, Detection
from .feature_db import FeatureDB


# ══════════════════════════════════════════════════════════
# 跟踪状态
# ══════════════════════════════════════════════════════════

class TrackedFace:
    """跟踪中的一个人脸。"""
    __slots__ = ("track_id", "bbox", "score", "keypoints",
                 "feature", "name", "similarity",
                 "last_seen", "confirm_count", "result_sent",
                 "last_extract_time")

    def __init__(self, track_id: int, detection: Detection,
                 feature: Optional[np.ndarray] = None):
        self.track_id = track_id
        self.bbox = (detection.x1, detection.y1, detection.x2, detection.y2)
        self.score = detection.score
        self.keypoints = detection.keypoints
        self.feature = feature
        self.name = "?"
        self.similarity = 0.0
        self.last_seen = time.time()
        self.confirm_count = 0
        self.result_sent = False
        self.last_extract_time = 0.0

    def update_bbox(self, detection: Detection) -> None:
        self.bbox = (detection.x1, detection.y1, detection.x2, detection.y2)
        self.score = detection.score
        self.keypoints = detection.keypoints
        self.last_seen = time.time()

    @property
    def age(self) -> float:
        """距离上次检测的秒数。"""
        return time.time() - self.last_seen

    @property
    def area(self) -> float:
        x1, y1, x2, y2 = self.bbox
        return max(0, x2 - x1) * max(0, y2 - y1)


# ══════════════════════════════════════════════════════════
# IOU 计算
# ══════════════════════════════════════════════════════════

def _iou(a: tuple, b: tuple) -> float:
    """计算两个边界框的交并比。"""
    x1 = max(a[0], b[0])
    y1 = max(a[1], b[1])
    x2 = min(a[2], b[2])
    y2 = min(a[3], b[3])
    inter = max(0, x2 - x1) * max(0, y2 - y1)
    area_a = max(0, a[2] - a[0]) * max(0, a[3] - a[1])
    area_b = max(0, b[2] - b[0]) * max(0, b[3] - b[1])
    return inter / (area_a + area_b - inter + 1e-6)


def _match_detections_to_tracks(
    detections: List[Detection],
    tracks: List[TrackedFace],
    iou_threshold: float = 0.3,
) -> Tuple[Dict[int, Detection], List[Detection]]:
    """将新检测框匹配到已有跟踪。

    匈牙利风格贪心：对每个 track 找 IOU 最高的 detection；
    如果 IOU > 阈值且未被其他 track 抢走，则匹配。

    Returns:
        (matched: {track_idx → detection}, unmatched: [detection])
    """
    # 构建 IOU 矩阵
    n_dets = len(detections)
    n_tracks = len(tracks)
    iou_matrix = np.zeros((n_dets, n_tracks), dtype=np.float32)
    for di, det in enumerate(detections):
        for ti, track in enumerate(tracks):
            iou_matrix[di, ti] = _iou(
                (det.x1, det.y1, det.x2, det.y2), track.bbox
            )

    matched: Dict[int, Detection] = {}
    used_dets = set()

    # 贪心：每次选全局最大 IOU
    for _ in range(min(n_dets, n_tracks)):
        if iou_matrix.size == 0:
            break
        flat_idx = int(np.argmax(iou_matrix))
        di = flat_idx // n_tracks
        ti = flat_idx % n_tracks
        max_iou = iou_matrix[di, ti]

        if max_iou < iou_threshold:
            break

        matched[ti] = detections[di]
        used_dets.add(di)

        # 将该行/列置为 -1
        iou_matrix[di, :] = -1
        iou_matrix[:, ti] = -1

    unmatched = [d for i, d in enumerate(detections) if i not in used_dets]
    return matched, unmatched


# ══════════════════════════════════════════════════════════
# 推理线程入口
# ══════════════════════════════════════════════════════════

def inference_loop(
    frame_q: FrameQueue,
    result_q: ResultQueue,
    engine: RecognitionEngine,
    feature_db: FeatureDB,
    stop_event: threading.Event,
    det_threshold: float = 0.5,
    rec_threshold: float = 0.4,
    nms_threshold: float = 0.4,
    max_side: int = 480,
    skip_frames: int = 3,
    confirm_frames: int = 3,
    track_timeout: float = 2.0,
    infer_debug: bool = False,
) -> None:
    """推理主循环（运行在独立线程）。

    Args:
        frame_q: 帧队列（输入）。
        result_q: 结果队列（输出）。
        engine: 识别引擎。
        feature_db: 特征数据库。
        stop_event: 停止信号。
        det_threshold: 检测分数阈值。
        rec_threshold: 识别相似度阈值。
        nms_threshold: NMS IoU 阈值。
        max_side: 检测输入最大边长。
        skip_frames: 跳帧数（每 N 帧推理一次）。
        confirm_frames: 确认帧数（连续识别到同一人才发送）。
        track_timeout: 跟踪超时秒数（超时则丢弃）。
        infer_debug: 是否打印调试信息。
    """

    tracks: List[TrackedFace] = []  # 当前活跃跟踪
    next_track_id = 0
    frame_count = 0
    last_infer_time = 0.0

    print(f"[Inference] 推理线程启动 "
          f"(跳帧={skip_frames}, 确认={confirm_frames}, "
          f"跟踪超时={track_timeout}s)")

    while not stop_event.is_set():
        # ── 取帧 ──
        result = frame_q.pop(timeout=1.0)
        if result is None:
            continue

        bgr, timestamp, jpeg = result
        frame_count += 1

        # ── 是否推理这一帧？ ──
        do_infer = (frame_count % skip_frames == 1) or not tracks
        if not do_infer and tracks:
            if infer_debug:
                print(f"[Inference] 跳帧 #{frame_count}")
            continue

        # ── 清理过期跟踪 ──
        tracks = [t for t in tracks if t.age < track_timeout]
        if not tracks:
            # 无跟踪目标时也可以继续（等下帧检测到新人脸）
            pass

        # ── 检测 ──
        t0 = time.time()
        detections = engine.detect(bgr, score_thresh=det_threshold,
                                   nms_thresh=nms_threshold, max_side=max_side)
        t_detect = time.time() - t0

        if infer_debug:
            print(f"[Inference] #{frame_count}: 检测到 {len(detections)} 张人脸 "
                  f"({t_detect*1000:.0f}ms)")

        # ── 匹配到已有跟踪 ──
        if detections:
            matched, unmatched = _match_detections_to_tracks(
                detections, tracks
            )
        else:
            matched, unmatched = {}, []

        # ── 更新已匹配的跟踪 ──
        for track_idx, det in matched.items():
            track = tracks[track_idx]
            track.update_bbox(det)

            # 是否需要重新提取特征？
            # 策略：跟踪稳定后首次提取，后续复用
            t0_ex = time.time()
            # Re-extract every 2s so identity can change
            if track.feature is None or (t0_ex - track.last_extract_time) > 2.0:
                aligned = engine.align(bgr, det.keypoints)
                feat = engine.extract(aligned)
                track.feature = feat
                track.last_extract_time = t0_ex

                # 匹配
                name, sim = feature_db.match(feat, threshold=rec_threshold)
                if name != "?":
                    if track.name == name:
                        track.confirm_count += 1
                    else:
                        track.name = name
                        track.confirm_count = 1
                        track.result_sent = False
                    track.similarity = sim
                else:
                    track.name = "?"
                    track.similarity = sim
                    track.confirm_count = 0
                    track.result_sent = False

                extract_time = (time.time() - t0_ex) * 1000
                if extract_time > 200:
                    print(f"[Inference] ⚠ 特征提取慢: {extract_time:.0f}ms")

            # ── 确认机制：连续 K 帧识别到同一人 → 发送结果 ──
            if (track.name != "?" and
                track.confirm_count >= confirm_frames and
                    not track.result_sent):
                result_q.push({
                    "type": "identify",
                    "track_id": track.track_id,
                    "name": track.name,
                    "confidence": track.similarity,
                    "bbox": list(track.bbox),
                    "timestamp": time.time(),
                })
                track.result_sent = True
                print(f"[Inference] ✓ 确认识别: {track.name} "
                      f"(置信度={track.similarity:.3f}, "
                      f"确认={track.confirm_count}帧)")

        # ── 新跟踪（未匹配的检测） ──
        for det in unmatched:
            if det.score < det_threshold:
                continue
            track_id = next_track_id
            next_track_id += 1
            track = TrackedFace(track_id, det)
            tracks.append(track)
            if infer_debug:
                print(f"[Inference] 新跟踪 ID={track_id} "
                      f"score={det.score:.3f}")

        # ── 未匹配的旧跟踪：保持上一帧的 bbox ──
        # （不更新 bbox，等超时被清理）

        # ── 性能警告 ──
        total_time = (time.time() - t0) * 1000
        if total_time > 200:
            print(f"[Inference] ⚠ 推理慢: {total_time:.0f}ms "
                  f"(检测={t_detect*1000:.0f}ms)")

        last_infer_time = time.time()

    print("[Inference] 推理线程退出")
