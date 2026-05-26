#!/usr/bin/env python3
"""FaceRecognition 性能基准评估脚本

用法：
  # 自定义数据集（按人名分目录）
  python benchmark.py --data-dir ./data --seed 42

  # LFW pairs.txt 格式
  python benchmark.py --pairs-file ./lfw/pairs.txt --lfw-root ./lfw

  # 指定阈值范围
  python benchmark.py --data-dir ./data --thresholds 0.3,0.4,0.5

输入要求：
  --data-dir:  data/张三/001.jpg, 002.jpg ...  data/李四/001.jpg ...
              人名=子目录名，同人图片放同一目录下
  --pairs-file: LFW pairs.txt
    同人对（3字段）: name img_num1 img_num2
    异人对（4字段）: name1 img_num1 name2 img_num2

输出：
  results/roc.csv: threshold, far, frr, accuracy, tp, fp, tn, fn
  results/config.json: 种子、划分比例、对数等实验配置
  终端打印: EER、FAR=0.1%/1% 时阈值和FRR

依赖：pip install numpy opencv-python onnxruntime
"""

import sys
import os
import json
import struct
import time
import csv
import random
import argparse
from pathlib import Path

import numpy as np
import cv2
import onnxruntime as ort

ort.set_default_logger_severity(3)

# ── 默认配置 ──
SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_MODEL_DIR = os.environ.get(
    "FACE_MODEL_DIR",
    str(SCRIPT_DIR / "models" / "onnx_models" / "buffalo_sc"),
)
DEFAULT_FEAT_DIR = SCRIPT_DIR / "results" / "bench_features"
STRIDES = [8, 16, 32]
DET_MAX_SIDE = 480
NMS_THRESH = 0.4


# ═══════════════════════════════════════════════════════════
#  NMS
# ═══════════════════════════════════════════════════════════

def nms(faces, thresh=NMS_THRESH):
    if len(faces) == 0:
        return []
    faces = sorted(faces, key=lambda f: -f[4])
    keep = []
    for i, fi in enumerate(faces):
        suppressed = False
        for j in keep:
            inter = (min(fi[2], faces[j][2]) - max(fi[0], faces[j][0])) * \
                    (min(fi[3], faces[j][3]) - max(fi[1], faces[j][1]))
            inter = max(0, inter)
            area_i = (fi[2] - fi[0]) * (fi[3] - fi[1])
            area_j = (faces[j][2] - faces[j][0]) * (faces[j][3] - faces[j][1])
            if inter / (area_i + area_j - inter + 1e-6) > thresh:
                suppressed = True
                break
        if not suppressed:
            keep.append(i)
    return [faces[i] for i in keep]


# ═══════════════════════════════════════════════════════════
#  检测器
# ═══════════════════════════════════════════════════════════

class FaceDetector:
    def __init__(self, model_dir=DEFAULT_MODEL_DIR):
        det_path = os.path.join(model_dir, "det_500m.onnx")
        if not os.path.isfile(det_path):
            raise FileNotFoundError(f"Detection model not found: {det_path}")
        self.sess = ort.InferenceSession(det_path, providers=["CPUExecutionProvider"])

    def detect(self, img_bgr, score_thresh=0.3):
        h, w = img_bgr.shape[:2]
        scale = 1.0
        if max(h, w) > DET_MAX_SIDE:
            scale = DET_MAX_SIDE / max(h, w)
            nh, nw = int(h * scale), int(w * scale)
            img = cv2.resize(img_bgr, (nw, nh))
        else:
            img = img_bgr
            nh, nw = h, w

        blob = ((img.astype(np.float32) - 127.5) / 128.0).transpose(2, 0, 1)[np.newaxis]
        outputs = self.sess.run(None, {"input.1": blob})
        out_dict = dict(zip([o.name for o in self.sess.get_outputs()], outputs))

        faces = []
        for level, s in enumerate(STRIDES):
            scores = out_dict[["443", "468", "493"][level]].flatten()
            bboxes = out_dict[["446", "471", "496"][level]]
            kpss = out_dict[["449", "474", "499"][level]]
            fm_h = (nh + s - 1) // s
            fm_w = (nw + s - 1) // s

            cand_idx = np.where(scores > score_thresh)[0]
            if len(cand_idx) == 0:
                continue
            if len(cand_idx) > 500:
                top = np.argpartition(-scores, 500)[:500]
                cand_idx = top[np.isin(top, cand_idx)]

            cy = (cand_idx // (fm_w * 2)) * s + 0.5 * s
            cx = ((cand_idx // 2) % fm_w) * s + 0.5 * s
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
                kps = [[cx[i] + kp[i][p * 2] * s, cy[i] + kp[i][p * 2 + 1] * s] for p in range(5)]
                faces.append([x1[i], y1[i], x2[i], y2[i], float(sc[i]), kps])

        faces = nms(faces)
        if scale != 1.0:
            inv = 1.0 / scale
            for f in faces:
                f[0] *= inv; f[1] *= inv; f[2] *= inv; f[3] *= inv
                for kp in f[5]:
                    kp[0] *= inv; kp[1] *= inv
        return faces


# ═══════════════════════════════════════════════════════════
#  对齐 + 特征提取
# ═══════════════════════════════════════════════════════════

REF = np.array([
    [30.2946, 51.6963], [65.5318, 51.6963], [48.0252, 71.7366],
    [33.5493, 92.3655], [62.7299, 92.3655],
], dtype=np.float32)


class FeatureExtractor:
    def __init__(self, model_dir=DEFAULT_MODEL_DIR):
        rec_path = os.path.join(model_dir, "w600k_mbf.onnx")
        if not os.path.isfile(rec_path):
            raise FileNotFoundError(f"Recognition model not found: {rec_path}")
        self.sess = ort.InferenceSession(rec_path, providers=["CPUExecutionProvider"])

    def extract(self, aligned_bgr):
        blob = ((aligned_bgr.astype(np.float32) - 127.5) / 127.5).transpose(2, 0, 1)[np.newaxis]
        feat = self.sess.run(None, {"input.1": blob})[0].flatten()
        return feat / (np.linalg.norm(feat) + 1e-8)


def align(img_bgr, kps, out_size=112):
    src = np.array(kps, dtype=np.float32)
    M, _ = cv2.estimateAffinePartial2D(src, REF)
    if M is None:
        return np.zeros((out_size, out_size, 3), dtype=np.uint8)
    return cv2.warpAffine(img_bgr, M, (out_size, out_size))


# ═══════════════════════════════════════════════════════════
#  特征持久化 (.bin)
# ═══════════════════════════════════════════════════════════

def save_feat(feat_dir, name, feat):
    os.makedirs(feat_dir, exist_ok=True)
    path = os.path.join(feat_dir, name + ".bin")
    with open(path, "wb") as f:
        f.write(struct.pack("i", len(feat)))
        f.write(struct.pack(f"{len(feat)}f", *feat))
    return path


def load_feat(feat_dir, name):
    path = os.path.join(feat_dir, name + ".bin")
    if not os.path.isfile(path):
        return None
    with open(path, "rb") as f:
        dim = struct.unpack("i", f.read(4))[0]
        return np.array(struct.unpack(f"{dim}f", f.read(dim * 4)))


# ═══════════════════════════════════════════════════════════
#  数据集加载
# ═══════════════════════════════════════════════════════════

def load_dataset_from_dir(data_dir):
    """按人名目录加载，返回 {name: [image_paths]}"""
    data_path = Path(data_dir)
    if not data_path.is_dir():
        raise FileNotFoundError(f"Data directory not found: {data_dir}")
    dataset = {}
    for person_dir in sorted(data_path.iterdir()):
        if person_dir.is_dir():
            imgs = sorted([
                str(p) for p in person_dir.iterdir()
                if p.suffix.lower() in (".jpg", ".jpeg", ".png", ".bmp")
            ])
            if len(imgs) >= 2:
                dataset[person_dir.name] = imgs
    if len(dataset) < 2:
        raise ValueError(f"Need >= 2 people with >= 2 images each, got {len(dataset)}")
    return dataset


def load_lfw_pairs(pairs_file, lfw_root):
    """解析 LFW pairs.txt"""
    pairs = []
    with open(pairs_file, "r") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) == 3:
                name, n1, n2 = parts
                pairs.append((name, int(n1), name, int(n2), 1))
            elif len(parts) == 4:
                name1, n1, name2, n2 = parts
                pairs.append((name1, int(n1), name2, int(n2), 0))
    return pairs, lfw_root


# ═══════════════════════════════════════════════════════════
#  评估主逻辑
# ═══════════════════════════════════════════════════════════

def run_benchmark(args):
    detector = FaceDetector(args.model_dir)
    extractor = FeatureExtractor(args.model_dir)
    feat_dir = args.feat_dir or str(DEFAULT_FEAT_DIR)

    # ── 加载数据集 ──
    if args.pairs_file:
        print(f"[1/6] Loading LFW pairs from: {args.pairs_file}")
        pairs, lfw_root = load_lfw_pairs(args.pairs_file, args.lfw_root)
        # 从 pairs 中提取所有人名和图片编号
        all_images = set()
        for name1, n1, name2, n2, _ in pairs:
            all_images.add((name1, n1))
            all_images.add((name2, n2))
        people = {}
        for name, n in all_images:
            img_path = os.path.join(lfw_root, name, f"{name}_{n:04d}.jpg")
            if os.path.isfile(img_path):
                if name not in people:
                    people[name] = []
                people[name].append(img_path)
        dataset = {k: v for k, v in people.items() if len(v) >= 2}
        print(f"    {len(dataset)} people, {sum(len(v) for v in dataset.values())} images")

    else:
        print(f"[1/6] Loading dataset from: {args.data_dir}")
        dataset = load_dataset_from_dir(args.data_dir)
        print(f"    {len(dataset)} people, {sum(len(v) for v in dataset.values())} images")

    # ── 按人划分注册集/测试集 (80/20) ──
    random.seed(args.seed)
    people_names = sorted(dataset.keys())
    random.shuffle(people_names)
    split_idx = int(len(people_names) * 0.8)
    split_idx = max(1, min(split_idx, len(people_names) - 1))
    register_names = set(people_names[:split_idx])
    test_names = set(people_names[split_idx:])

    print(f"[2/6] Split: {len(register_names)} register / {len(test_names)} test (seed={args.seed})")

    # ── 注册：从每人的图片中取第一张提取特征 ──
    print("[3/6] Extracting register features...")
    registry = {}
    for name in sorted(register_names):
        img_path = dataset[name][0]
        img = cv2.imread(img_path)
        if img is None:
            print(f"    WARNING: Cannot read {img_path}")
            continue
        faces = detector.detect(img, score_thresh=0.3)
        if not faces:
            print(f"    WARNING: No face in {img_path}")
            continue
        faces.sort(key=lambda f: f[4], reverse=True)
        aligned = align(img, faces[0][5])
        feat = extractor.extract(aligned)
        save_feat(feat_dir, name, feat)
        registry[name] = feat
        print(f"    Registered: {name}")

    print(f"    Registry: {len(registry)} people")

    # ── 生成测试对 (1:1 正负比) ──
    print("[4/6] Generating test pairs...")
    test_pairs = []

    # 正对：同人不同图
    for name in sorted(test_names):
        imgs = dataset[name]
        if len(imgs) < 2:
            continue
        # 用 2 张形成 pair
        test_pairs.append((name, imgs[0], name, imgs[1], 1))

    # 负对：不同人
    test_names_list = sorted(test_names)
    for i, name_a in enumerate(test_names_list):
        name_b = test_names_list[(i + 1) % len(test_names_list)]
        if name_a != name_b:
            test_pairs.append((name_a, dataset[name_a][0], name_b, dataset[name_b][0], 0))

    # 平衡到 1:1
    pos = [p for p in test_pairs if p[4] == 1]
    neg = [p for p in test_pairs if p[4] == 0]
    min_count = min(len(pos), len(neg))
    pos = pos[:min_count]
    neg = neg[:min_count]
    test_pairs = pos + neg
    random.shuffle(test_pairs)

    print(f"    Total pairs: {len(test_pairs)} (positive={len(pos)}, negative={len(neg)})")

    # ── 跑测试对，计算相似度 ──
    print("[5/6] Computing similarities...")
    similarities = []  # (sim, label)

    for name_a, img_a, name_b, img_b, label in test_pairs:
        img1 = cv2.imread(img_a)
        img2 = cv2.imread(img_b)
        if img1 is None or img2 is None:
            continue

        def get_feat(img):
            faces = detector.detect(img, score_thresh=0.3)
            if not faces:
                return None
            faces.sort(key=lambda f: f[4], reverse=True)
            aligned = align(img, faces[0][5])
            return extractor.extract(aligned)

        feat1 = get_feat(img1)
        feat2 = get_feat(img2)
        if feat1 is None or feat2 is None:
            continue

        sim = float(np.dot(feat1, feat2))
        similarities.append((sim, label))

    print(f"    Computed {len(similarities)} similarities")

    # ── 计算 ROC ──
    print("[6/6] Computing ROC metrics...")
    thresholds = np.linspace(args.thresh_start, args.thresh_end, args.thresh_steps)

    results = []
    total_pos = sum(1 for _, l in similarities if l == 1)
    total_neg = sum(1 for _, l in similarities if l == 0)

    if total_pos == 0 or total_neg == 0:
        print("    ERROR: No positive or negative samples!")
        return

    eer_threshold = 0.4
    eer_value = 1.0

    for thr in thresholds:
        tp = fp = tn = fn = 0
        for sim, label in similarities:
            pred_pos = sim >= thr
            if label == 1:
                if pred_pos: tp += 1
                else: fn += 1
            else:
                if pred_pos: fp += 1
                else: tn += 1

        far = fp / total_neg
        frr = fn / total_pos
        accuracy = (tp + tn) / len(similarities)
        results.append({
            "threshold": round(float(thr), 4),
            "far": round(float(far), 6),
            "frr": round(float(frr), 6),
            "accuracy": round(float(accuracy), 6),
            "tp": tp, "fp": fp, "tn": tn, "fn": fn,
        })

        # Track EER (where FAR ≈ FRR)
        diff = abs(far - frr)
        if diff < eer_value:
            eer_value = diff
            eer_threshold = float(thr)

    # ── 写结果 ──
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    csv_path = output_dir / "roc.csv"
    with open(csv_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["threshold", "far", "frr", "accuracy", "tp", "fp", "tn", "fn", "eer"])
        writer.writeheader()
        for row in results:
            row["eer"] = round(abs(row["far"] - row["frr"]), 6)
            writer.writerow(row)

    config_path = output_dir / "config.json"
    with open(config_path, "w") as f:
        json.dump({
            "seed": args.seed,
            "split_ratio": "80/20",
            "register_count": len(registry),
            "test_count": len(test_names),
            "total_pairs": len(test_pairs),
            "positive_pairs": total_pos,
            "negative_pairs": total_neg,
            "eer_threshold": eer_threshold,
            "eer_value": round(eer_value, 6),
            "model_dir": args.model_dir,
        }, f, indent=2)

    print(f"\n{'='*60}")
    print("RESULTS")
    print(f"{'='*60}")
    print(f"  Total pairs:     {len(test_pairs)}")
    print(f"  Positive pairs:  {total_pos}")
    print(f"  Negative pairs:  {total_neg}")
    print(f"  Random seed:     {args.seed}")
    print()

    # EER
    eer_row = min(results, key=lambda r: abs(r["far"] - r["frr"]))
    print(f"  EER:            {eer_row['far']:.6f} @ threshold={eer_row['threshold']:.2f}")

    # FAR=0.1%
    far_0001 = next((r for r in results if r["far"] <= 0.001), results[-1])
    print(f"  FAR=0.1%:       threshold={far_0001['threshold']:.2f}, FRR={far_0001['frr']:.4f}")

    # FAR=1%
    far_001 = next((r for r in results if r["far"] <= 0.01), results[-1])
    print(f"  FAR=1%:         threshold={far_001['threshold']:.2f}, FRR={far_001['frr']:.4f}")

    # 推荐阈值 (FAR/FRR 交点)
    crossover = min(results, key=lambda r: abs(r["far"] - r["frr"]))
    print(f"  Recommended:    threshold={crossover['threshold']:.2f} (FAR={crossover['far']:.4f} FRR={crossover['frr']:.4f})")
    print()
    print(f"  Results saved to: {output_dir}/")
    print(f"    {csv_path}")
    print(f"    {config_path}")


# ═══════════════════════════════════════════════════════════
#  入口
# ═══════════════════════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(description="FaceRecognition Benchmark")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--data-dir", help="Custom dataset directory (person subdirs)")
    group.add_argument("--pairs-file", help="LFW pairs.txt path")
    parser.add_argument("--lfw-root", default=".", help="LFW images root directory")
    parser.add_argument("--model-dir", default=DEFAULT_MODEL_DIR, help="ONNX model directory")
    parser.add_argument("--feat-dir", default=str(DEFAULT_FEAT_DIR), help="Temp feature storage directory")
    parser.add_argument("--output-dir", default=str(SCRIPT_DIR / "results"), help="Results output directory")
    parser.add_argument("--seed", type=int, default=42, help="Random seed")
    parser.add_argument("--thresh-start", type=float, default=0.1, help="Threshold range start")
    parser.add_argument("--thresh-end", type=float, default=0.9, help="Threshold range end")
    parser.add_argument("--thresh-steps", type=int, default=81, help="Number of threshold steps")
    args = parser.parse_args()

    run_benchmark(args)


if __name__ == "__main__":
    main()
