"""Face verification: register / identify / compare / live."""
import sys, os, struct, time, math
import numpy as np
import cv2
import onnxruntime as ort

# Suppress onnxruntime warnings
ort.set_default_logger_severity(3)

MODEL_DIR = os.environ.get("FACE_MODEL_DIR", os.path.join(os.path.dirname(os.path.abspath(__file__)), "models", "onnx_models", "buffalo_sc"))
FEAT_DIR  = os.environ.get("FACE_FEAT_DIR", os.path.join(os.path.dirname(os.path.abspath(__file__)), "features"))
STRIDES   = [8, 16, 32]
DET_MAX_SIDE = 480  # detect on max-480px input
NMS_THRESH   = 0.4
MATCH_THRESH = 0.4

# ─── nms ────────────────────────────────────────────────
def nms(faces, thresh=NMS_THRESH):
    if len(faces) == 0: return []
    faces = sorted(faces, key=lambda f: -f[4])
    keep = []
    for i, fi in enumerate(faces):
        if keep and any(
            max(0, min(fi[0], faces[j][2]) - max(fi[1], faces[j][0])) *
            max(0, min(fi[1], faces[j][3]) - max(fi[0], faces[j][1]))
            / ((fi[2]-fi[0])*(fi[3]-fi[1]) + (faces[j][2]-faces[j][0])*(faces[j][3]-faces[j][1]) + 1e-6)
            > thresh for j in keep
        ): continue
        keep.append(i)
    return [faces[i] for i in keep]

# ─── detector (vectorized) ──────────────────────────────
det_sess = ort.InferenceSession(os.path.join(MODEL_DIR, "det_500m.onnx"),
                                providers=['CPUExecutionProvider'])

def detect(img_bgr, score_thresh=0.3):
    h, w = img_bgr.shape[:2]

    # Scale so max side = DET_MAX_SIDE
    scale = 1.0
    if max(h, w) > DET_MAX_SIDE:
        scale = DET_MAX_SIDE / max(h, w)
        nh, nw = int(h * scale), int(w * scale)
        img = cv2.resize(img_bgr, (nw, nh))
    else:
        img = img_bgr
        nh, nw = h, w

    blob = ((img.astype(np.float32) - 127.5) / 128.0).transpose(2, 0, 1)[np.newaxis, :, :, :]
    outputs = det_sess.run(None, {"input.1": blob})
    out_names = [o.name for o in det_sess.get_outputs()]
    out_dict = dict(zip(out_names, outputs))

    faces = []
    for level, s in enumerate(STRIDES):
        scores = out_dict[["443","468","493"][level]].flatten()
        bboxes = out_dict[["446","471","496"][level]]
        kpss   = out_dict[["449","474","499"][level]]

        fm_h = (nh + s - 1) // s
        fm_w = (nw + s - 1) // s
        n_anchors = fm_h * fm_w * 2

        # Find candidates above threshold (vectorized)
        cand_idx = np.where(scores > score_thresh)[0]
        if len(cand_idx) == 0:
            continue

        # Limit candidates to top-500 per stride
        if len(cand_idx) > 500:
            top = np.argpartition(-scores, 500)[:500]
            cand_idx = top[np.isin(top, cand_idx)]

        # Vectorized anchor centers
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
            kps = [[cx[i] + kp[i][p*2]*s, cy[i] + kp[i][p*2+1]*s] for p in range(5)]
            faces.append([x1[i], y1[i], x2[i], y2[i], float(sc[i]), kps])

    # NMS
    faces = nms(faces)

    # Scale bbox/kps back to original image size
    if scale != 1.0:
        inv = 1.0 / scale
        for f in faces:
            f[0] *= inv; f[1] *= inv; f[2] *= inv; f[3] *= inv
            for kp in f[5]:
                kp[0] *= inv; kp[1] *= inv

    return faces

# ─── aligner ─────────────────────────────────────────────
REF = np.array([
    [30.2946, 51.6963], [65.5318, 51.6963], [48.0252, 71.7366],
    [33.5493, 92.3655], [62.7299, 92.3655],
], dtype=np.float32)

def align(img_bgr, kps, out_size=112):
    src = np.array(kps, dtype=np.float32)
    M, _ = cv2.estimateAffinePartial2D(src, REF)
    if M is None:
        return np.zeros((out_size, out_size, 3), dtype=np.uint8)
    return cv2.warpAffine(img_bgr, M, (out_size, out_size))

# ─── feature extractor ───────────────────────────────────
rec_sess = ort.InferenceSession(os.path.join(MODEL_DIR, "w600k_mbf.onnx"),
                                providers=['CPUExecutionProvider'])

def extract(aligned_bgr):
    blob = ((aligned_bgr.astype(np.float32) - 127.5) / 127.5).transpose(2, 0, 1)[np.newaxis, :, :, :]
    feat = rec_sess.run(None, {"input.1": blob})[0].flatten()
    norm = np.linalg.norm(feat) + 1e-8
    return (feat / norm).tolist()

# ─── persistence ─────────────────────────────────────────
def save_feat(name, feat):
    os.makedirs(FEAT_DIR, exist_ok=True)
    path = os.path.join(FEAT_DIR, name + ".bin")
    with open(path, "wb") as f:
        f.write(struct.pack("i", len(feat)))
        f.write(struct.pack(f"{len(feat)}f", *feat))
    print(f"  Saved: {path}")

def load_registry():
    registry = {}
    if not os.path.exists(FEAT_DIR): return registry
    for fn in os.listdir(FEAT_DIR):
        if fn.endswith(".bin"):
            with open(os.path.join(FEAT_DIR, fn), "rb") as f:
                dim = struct.unpack("i", f.read(4))[0]
                feat = list(struct.unpack(f"{dim}f", f.read(dim * 4)))
            registry[fn.replace(".bin", "")] = feat
    return registry

# ─── main ────────────────────────────────────────────────
if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage:")
        print("  verify.py register <photo.jpg> <name>")
        print("  verify.py identify <photo.jpg>")
        print("  verify.py compare <img1.jpg> <img2.jpg>")
        print("  verify.py live")
        sys.exit(1)

    cmd = sys.argv[1]

    if cmd == "register":
        img = cv2.imread(sys.argv[2])
        name = sys.argv[3]
        print(f"Registering '{name}' from {sys.argv[2]} ({img.shape[1]}x{img.shape[0]})...")
        faces = detect(img)
        if not faces: print("  ERROR: No face detected"); sys.exit(1)
        faces.sort(key=lambda f: f[4], reverse=True)
        f = faces[0]
        print(f"  Face: bbox=[{f[0]:.0f},{f[1]:.0f},{f[2]:.0f},{f[3]:.0f}] score={f[4]:.4f}")
        aligned = align(img, f[5])
        feat = extract(aligned)
        save_feat(name, feat)
        print("  Registered!")

    elif cmd == "identify":
        img = cv2.imread(sys.argv[2])
        print(f"Identifying from {sys.argv[2]} ({img.shape[1]}x{img.shape[0]})...")
        registry = load_registry()
        if not registry: print("  No registered faces."); sys.exit(1)
        print(f"  Registry: {list(registry.keys())}")
        faces = detect(img)
        if not faces: print("  ERROR: No face detected"); sys.exit(1)
        faces.sort(key=lambda f: f[4], reverse=True)
        f = faces[0]
        print(f"  Face: bbox=[{f[0]:.0f},{f[1]:.0f},{f[2]:.0f},{f[3]:.0f}] score={f[4]:.4f}")
        aligned = align(img, f[5])
        feat = extract(aligned)
        best_sim, best_name = -2.0, ""
        for name, rf in registry.items():
            sim = sum(a*b for a,b in zip(feat, rf))
            if sim > best_sim: best_sim, best_name = sim, name
            print(f"    vs {name}: similarity={sim:.4f}{' <- MATCH' if sim>MATCH_THRESH else ''}")
        print()
        if best_sim > MATCH_THRESH:
            print(f"  >>> IDENTIFIED: {best_name} (confidence: {best_sim:.4f})")
        else:
            print(f"  >>> NO MATCH (best: {best_name} at {best_sim:.4f})")

    elif cmd == "compare":
        img1, img2 = cv2.imread(sys.argv[2]), cv2.imread(sys.argv[3])
        if img1 is None or img2 is None: print("Failed to load image"); sys.exit(1)
        print(f"Comparing {sys.argv[2]} vs {sys.argv[3]}...")
        sim_values = []
        for path, img in [(sys.argv[2], img1), (sys.argv[3], img2)]:
            faces = detect(img)
            if not faces: print(f"  No face in {path}"); sys.exit(1)
            faces.sort(key=lambda f: f[4], reverse=True)
            f = faces[0]
            aligned = align(img, f[5])
            feat = extract(aligned)
            print(f"  {os.path.basename(path)}: score={f[4]:.4f}")
            sim_values.append(feat)
        sim = sum(a*b for a,b in zip(*sim_values))
        print(f"  Similarity: {sim:.4f}")
        print(f"  Result: {'SAME person' if sim > MATCH_THRESH else 'DIFFERENT person'}")

    elif cmd == "live":
        registry = load_registry()
        if not registry:
            print("WARNING: No registered faces. Use 'register' first.")
        else:
            print(f"Registry: {list(registry.keys())}")
        print("Starting camera... (press Q to quit)")
        print("  optimizations: 480px detect, skip-5, feature cache, vectorized decode\n")

        cap = cv2.VideoCapture(0)
        cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
        cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)
        if not cap.isOpened(): print("ERROR: Cannot open camera"); sys.exit(1)

        frame_count = 0
        fps_timer = time.time()
        fps = 0.0
        last_faces = []
        last_feats = {}  # track_id -> (feature, name, sim)
        next_track_id = 0

        def iou(a, b):
            x1 = max(a[0], b[0]); y1 = max(a[1], b[1])
            x2 = min(a[2], b[2]); y2 = min(a[3], b[3])
            inter = max(0, x2-x1) * max(0, y2-y1)
            area_a = (a[2]-a[0]) * (a[3]-a[1])
            area_b = (b[2]-b[0]) * (b[3]-b[1])
            return inter / (area_a + area_b - inter + 1e-6)

        while True:
            ret, frame = cap.read()
            if not ret: break

            frame_count += 1
            detect_frame = (frame_count % 5 == 1)

            if detect_frame:
                raw_faces = detect(frame, score_thresh=0.3)

                # Match to previous faces via IOU
                new_faces = []
                matched_old = set()
                for rf in raw_faces:
                    best_iou, best_id = 0, -1
                    for i, lf in enumerate(last_faces):
                        if i in matched_old: continue
                        iou_val = iou(rf, lf)
                        if iou_val > best_iou:
                            best_iou, best_id = iou_val, i
                    if best_iou > 0.3:
                        # Same face — copy cached feature
                        rf.append(last_faces[best_id][6])  # track_id
                        rf.append(last_feats.get(last_faces[best_id][6], (None, "?", 0))[0])  # cached feat
                        matched_old.add(best_id)
                    else:
                        rf.append(next_track_id)
                        rf.append(None)  # will compute
                        next_track_id += 1
                    new_faces.append(rf)

                # Compute features for new faces
                for rf in new_faces:
                    if rf[7] is not None: continue  # already cached
                    aligned = align(frame, rf[5], 112)
                    feat = extract(aligned)
                    rf[7] = feat
                    # Identify
                    best_sim, best_name = -2.0, "?"
                    for name, rf_ in registry.items():
                        s = sum(a*b for a,b in zip(feat, rf_))
                        if s > best_sim: best_sim, best_name = s, name
                    last_feats[rf[6]] = (feat, best_name, best_sim)

                last_faces = new_faces
            else:
                # Reuse last detection; update feature cache timestamps
                new_faces = last_faces

            # Draw
            for f in last_faces:
                x1, y1, x2, y2 = int(f[0]), int(f[1]), int(f[2]), int(f[3])
                tid = f[6]
                _, name, sim = last_feats.get(tid, (None, "?", 0))

                color = (0, 255, 0) if sim > MATCH_THRESH else (0, 165, 255)
                label = f"{name} {sim:.2f}" if registry else f"score:{f[4]:.2f}"
                cv2.rectangle(frame, (x1, y1), (x2, y2), color, 2)
                cv2.putText(frame, label, (x1, y1-8), cv2.FONT_HERSHEY_SIMPLEX, 0.55, color, 2)
                for kp in f[5]:
                    cv2.circle(frame, (int(kp[0]), int(kp[1])), 2, (0, 255, 255), -1)

            # FPS (update every 15 frames)
            if frame_count % 15 == 0:
                fps = 15 / (time.time() - fps_timer)
                fps_timer = time.time()
            cv2.putText(frame, f"FPS: {fps:.1f}", (10, 25),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 0), 2)

            cv2.imshow("Face Recognition", frame)
            if cv2.waitKey(1) & 0xFF == ord('q'):
                break

        cap.release()
        cv2.destroyAllWindows()
