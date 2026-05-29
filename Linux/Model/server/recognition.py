"""人脸识别引擎 — ONNX 模型推理管线。

封装 verify.py 验证通过的推理逻辑，作为可复用类：
  RecognitionEngine
    ├── detect(bgr) → List[Face]
    ├── align(bgr, keypoints) → aligned_bgr
    ├── extract(aligned_bgr) → feature (512-dim L2 normalized)
    └── match(feature, feature_db) → (name, similarity)

性能优化（来自 verify.py 已验证策略）：
  - 检测输入缩放至 max_side px
  - 解码完全向量化（numpy）
  - onnxruntime 日志静默
"""

from __future__ import annotations

import os
import struct
import numpy as np
import onnxruntime as ort

# 抑制 onnxruntime 警告
ort.set_default_logger_severity(3)

# SCRFD 三个 stride（特征图下采样倍率）
STRIDES = [8, 16, 32]
# 标准化参数（INSightFace 默认）
MEAN = np.array([127.5, 127.5, 127.5], dtype=np.float32)
STD_INV = np.array([1.0 / 128.0, 1.0 / 128.0, 1.0 / 128.0], dtype=np.float32)

# 仿射对齐参考模板（5 点，112×112）
_REF_5 = np.array([
    [30.2946, 51.6963],
    [65.5318, 51.6963],
    [48.0252, 71.7366],
    [33.5493, 92.3655],
    [62.7299, 92.3655],
], dtype=np.float32)


# ══════════════════════════════════════════════════════════
# 检测结果结构
# ══════════════════════════════════════════════════════════

class Detection:
    """单个人脸检测结果。"""
    __slots__ = ("x1", "y1", "x2", "y2", "score", "keypoints")
    def __init__(self, x1: float, y1: float, x2: float, y2: float,
                 score: float, keypoints: list):
        self.x1 = x1
        self.y1 = y1
        self.x2 = x2
        self.y2 = y2
        self.score = score
        self.keypoints = keypoints  # [[x,y]*5]


# ══════════════════════════════════════════════════════════
# NMS（非极大值抑制）
# ══════════════════════════════════════════════════════════

def _nms(faces: list, thresh: float = 0.4) -> list:
    """按 score 降序做 NMS，返回保留的人脸列表。

    faces 元素格式：[x1, y1, x2, y2, score, keypoints]
    """
    if len(faces) == 0:
        return []

    faces = sorted(faces, key=lambda f: -f[4])
    keep = []
    for i, fi in enumerate(faces):
        if keep and any(
            max(0, min(fi[2], faces[j][2]) - max(fi[0], faces[j][0])) *
            max(0, min(fi[3], faces[j][3]) - max(fi[1], faces[j][1])) /
            ((fi[2] - fi[0]) * (fi[3] - fi[1]) +
             (faces[j][2] - faces[j][0]) * (faces[j][3] - faces[j][1]) + 1e-6)
            > thresh
            for j in keep
        ):
            continue
        keep.append(i)
    return [faces[i] for i in keep]


# ══════════════════════════════════════════════════════════
# RecognitionEngine
# ══════════════════════════════════════════════════════════

class RecognitionEngine:
    """人脸检测 + 对齐 + 特征提取流水线。

    用法:
        engine = RecognitionEngine(model_dir="/path/to/models")
        faces = engine.detect(bgr_image)
        if faces:
            aligned = engine.align(bgr_image, faces[0].keypoints)
            feat = engine.extract(aligned)
    """

    def __init__(self, model_dir: str,
                 det_model: str = "det_500m.onnx",
                 rec_model: str = "w600k_mbf.onnx"):
        """
        Args:
            model_dir: ONNX 模型目录。
            det_model: 检测模型文件名。
            rec_model: 识别/特征提取模型文件名。
        """
        det_path = os.path.join(model_dir, det_model)
        rec_path = os.path.join(model_dir, rec_model)

        if not os.path.exists(det_path):
            raise FileNotFoundError(f"检测模型不存在: {det_path}")
        if not os.path.exists(rec_path):
            raise FileNotFoundError(f"识别模型不存在: {rec_path}")

        # 加载 ONNX 模型
        self._det_sess = ort.InferenceSession(
            det_path, providers=["CPUExecutionProvider"]
        )
        self._rec_sess = ort.InferenceSession(
            rec_path, providers=["CPUExecutionProvider"]
        )

        # 缓存检测输出名称
        self._out_names = [o.name for o in self._det_sess.get_outputs()]

        print(f"[Engine] 模型已加载: {os.path.basename(det_path)}, "
              f"{os.path.basename(rec_path)}")

    # ── 检测 ──────────────────────────────────────────────

    def detect(self, bgr: np.ndarray,
               score_thresh: float = 0.5,
               nms_thresh: float = 0.4,
               max_side: int = 480) -> list:
        """检测图片中的所有人脸。

        Args:
            bgr: BGR 图像 (H,W,3) uint8。
            score_thresh: 检测分数阈值。
            nms_thresh: NMS IoU 阈值。
            max_side: 检测输入最大边长（缩放后）。

        Returns:
            Detection 列表（按 score 降序）。
        """
        h, w = bgr.shape[:2]

        # ── 缩放到 max_side ──
        scale = 1.0
        if max(h, w) > max_side:
            scale = max_side / max(h, w)
            nh, nw = int(h * scale), int(w * scale)
            img = self._cv2_resize(bgr, (nw, nh))
        else:
            img = bgr
            nh, nw = h, w

        # ── 预处理 ──
        blob = ((img.astype(np.float32) - 127.5) / 128.0) \
            .transpose(2, 0, 1)[np.newaxis, :, :, :]

        # ── 推理 ──
        outputs = self._det_sess.run(None, {"input.1": blob})
        out_dict = dict(zip(self._out_names, outputs))

        # ── 解码（向量化） ──
        faces_raw = []
        score_names = ["443", "468", "493"]
        bbox_names = ["446", "471", "496"]
        kps_names = ["449", "474", "499"]

        for level, s in enumerate(STRIDES):
            scores = out_dict[score_names[level]].flatten()
            bboxes = out_dict[bbox_names[level]]
            kpss = out_dict[kps_names[level]]

            fm_h = (nh + s - 1) // s
            fm_w = (nw + s - 1) // s

            cand_idx = np.where(scores > score_thresh)[0]
            if len(cand_idx) == 0:
                continue

            # 限制候选框数量（top-500 per stride）
            if len(cand_idx) > 500:
                top = np.argpartition(-scores, 500)[:500]
                cand_idx = top[np.isin(top, cand_idx)]

            # 向量化 anchor 中心
            cy = (cand_idx // (fm_w * 2)).astype(np.float32) * s + 0.5 * s
            cx = ((cand_idx // 2) % fm_w).astype(np.float32) * s + 0.5 * s

            b = bboxes[cand_idx]
            kp = kpss[cand_idx]
            sc = scores[cand_idx]

            x1 = np.maximum(0, cx - b[:, 0] * s)
            y1 = np.maximum(0, cy - b[:, 1] * s)
            x2 = np.minimum(nw, cx + b[:, 2] * s)
            y2 = np.minimum(nh, cy + b[:, 3] * s)

            for i in range(len(cand_idx)):
                if x2[i] <= x1[i] or y2[i] <= y1[i]:
                    continue
                kps_list = [[cx[i] + kp[i][p * 2] * s,
                             cy[i] + kp[i][p * 2 + 1] * s]
                            for p in range(5)]
                faces_raw.append([float(x1[i]), float(y1[i]),
                                  float(x2[i]), float(y2[i]),
                                  float(sc[i]), kps_list])

        # ── NMS ──
        faces_raw = _nms(faces_raw, nms_thresh)

        # ── 缩放回原图 ──
        detections = []
        inv = 1.0 / scale if scale != 1.0 else 1.0
        for f in faces_raw:
            kps = [[kp[0] * inv, kp[1] * inv] for kp in f[5]]
            detections.append(Detection(
                x1=f[0] * inv, y1=f[1] * inv,
                x2=f[2] * inv, y2=f[3] * inv,
                score=f[4], keypoints=kps
            ))

        return detections

    # ── 对齐 ──────────────────────────────────────────────

    def align(self, bgr: np.ndarray, keypoints: list,
              out_size: int = 112) -> np.ndarray:
        """使用 5 个关键点仿射对齐，输出标准正脸。

        Args:
            bgr: 原始 BGR 图像。
            keypoints: [[x,y]*5] 关键点列表。
            out_size: 输出尺寸（默认 112×112）。

        Returns:
            对齐后的 BGR 图像 (out_size, out_size, 3)。
        """
        src = np.array(keypoints, dtype=np.float32)
        M, _ = self._cv2_estimate_affine(src, _REF_5)
        if M is None:
            return np.zeros((out_size, out_size, 3), dtype=np.uint8)
        aligned = self._cv2_warp_affine(bgr, M, (out_size, out_size))
        return aligned

    # ── 特征提取 ──────────────────────────────────────────

    def extract(self, aligned_bgr: np.ndarray) -> np.ndarray:
        """从对齐人脸提取 L2 归一化的 512 维特征向量。

        Args:
            aligned_bgr: 对齐后的人脸 (112,112,3)。

        Returns:
            (512,) float32 特征向量（L2 norm = 1.0）。
        """
        blob = ((aligned_bgr.astype(np.float32) - 127.5) / 127.5) \
            .transpose(2, 0, 1)[np.newaxis, :, :, :]
        feat = self._rec_sess.run(None, {"input.1": blob})[0].flatten()
        norm = np.linalg.norm(feat) + 1e-8
        return feat / norm

    # ── 匹配 ──────────────────────────────────────────────

    @staticmethod
    def cosine_similarity(feat_a, feat_b):
        """余弦相似度（已归一化时等价于点积）。"""
        return float(np.dot(feat_a, feat_b))

    @staticmethod
    def match(feature: np.ndarray, registry: dict,
              threshold: float = 0.4) -> tuple:
        """在注册库中匹配最佳人脸。

        Args:
            feature: 查询特征 (512,)。
            registry: {name: feature_array} 注册库。
            threshold: 匹配阈值。

        Returns:
            (best_name, best_similarity)。无人匹配时 name="?"。
        """
        best_sim = -2.0
        best_name = "?"
        for name, ref_feat in registry.items():
            sim = float(np.dot(feature, ref_feat))
            if sim > best_sim:
                best_sim = sim
                best_name = name

        if best_sim < threshold:
            return ("?", best_sim)
        return (best_name, best_sim)

    # ── OpenCV 包装（延迟导入，避免硬依赖）───────────────

    @staticmethod
    def _cv2_resize(img, dsize):
        import cv2
        return cv2.resize(img, dsize)

    @staticmethod
    def _cv2_estimate_affine(src, dst):
        import cv2
        return cv2.estimateAffinePartial2D(src, dst)

    @staticmethod
    def _cv2_warp_affine(img, M, dsize):
        import cv2
        return cv2.warpAffine(img, M, dsize)


# ══════════════════════════════════════════════════════════
# 便捷函数（兼容 verify.py 接口）
# ══════════════════════════════════════════════════════════

# 全局单例（由 main.py 初始化）
_engine: RecognitionEngine = None


def init_engine(model_dir: str, det_model: str = "det_500m.onnx",
                rec_model: str = "w600k_mbf.onnx") -> RecognitionEngine:
    """初始化全局识别引擎。"""
    global _engine
    _engine = RecognitionEngine(model_dir, det_model, rec_model)
    return _engine


def get_engine() -> RecognitionEngine:
    """获取全局识别引擎。"""
    if _engine is None:
        raise RuntimeError("识别引擎未初始化，请先调用 init_engine()")
    return _engine
