"""Pi 端注册 API 服务 (ONNX 引擎版)

Flask HTTP API，接收 Windows 端发来的照片+姓名，
使用 Python ONNX 引擎完成人脸检测→对齐→特征提取→保存 .bin，
返回注册结果。

端点：
  GET  /api/health                       # 在线检测
  POST /api/register                     # 注册（multipart: photo + name）
  GET  /api/features                     # 列出已注册特征
"""

import sys
import os
import struct
import argparse
import traceback
from pathlib import Path

import cv2
import numpy as np
from flask import Flask, request, jsonify

# ── 配置 ──
SCRIPT_DIR = Path(__file__).resolve().parent
os.chdir(SCRIPT_DIR)

# 模型目录（相对于 Model/ 目录）
MODEL_DIR = os.environ.get("FACE_MODEL_DIR",
    str(SCRIPT_DIR / "models" / "onnx_models" / "buffalo_s"))

# 特征存储目录
FEATURES_DIR = os.environ.get("FACE_FEAT_DIR",
    str(SCRIPT_DIR / "features"))
os.makedirs(FEATURES_DIR, exist_ok=True)

# 添加 Raspberry Pi/src 到 path 以使用 RecognitionEngine
PI_SRC = str(SCRIPT_DIR.parent / "Raspberry Pi" / "src")
if PI_SRC not in sys.path:
    sys.path.insert(0, PI_SRC)

print(f"[pi_server] 脚本目录: {SCRIPT_DIR}")
print(f"[pi_server] 模型目录: {MODEL_DIR}")
print(f"[pi_server] 特征目录: {FEATURES_DIR}")

# ── 加载识别引擎 ──
_engine = None

def _get_engine():
    global _engine
    if _engine is None:
        from recognition import RecognitionEngine
        _engine = RecognitionEngine(
            model_dir=MODEL_DIR,
            det_model="det_500m.onnx",
            rec_model="w600k_mbf.onnx",
        )
    return _engine


# ── 特征持久化 ──

def _save_feature(name: str, feat: np.ndarray) -> str:
    path = os.path.join(FEATURES_DIR, f"{name}.bin")
    with open(path, "wb") as f:
        f.write(struct.pack("<i", len(feat)))
        f.write(feat.astype(np.float32).tobytes())
    return path


def _load_registry() -> dict:
    registry = {}
    if not os.path.exists(FEATURES_DIR):
        return registry
    for fn in os.listdir(FEATURES_DIR):
        if fn.endswith(".bin"):
            name = fn[:-4]
            registry[name] = os.path.join(FEATURES_DIR, fn)
    return registry


# ── 注册逻辑 ──

def _register_from_image(img_bgr: np.ndarray, name: str) -> dict:
    try:
        engine = _get_engine()
    except Exception as e:
        return {"success": False, "error": f"模型加载失败: {e}"}

    try:
        faces = engine.detect(img_bgr, score_thresh=0.3)
        if not faces:
            return {"success": False, "error": "未检测到人脸", "face_score": None}

        # 取置信度最高的人脸
        faces.sort(key=lambda f: f.score, reverse=True)
        best = faces[0]

        aligned = engine.align(img_bgr, best.keypoints)
        feat = engine.extract(aligned)

        _save_feature(name, feat)

        return {
            "success": True,
            "name": name,
            "face_score": round(float(best.score), 4),
            "feature_dim": len(feat),
            "message": f"'{name}' 注册成功, 特征维度={len(feat)}",
        }
    except Exception as e:
        traceback.print_exc()
        return {"success": False, "error": f"注册异常: {e}"}


# ── Flask 应用 ──
app = Flask(__name__)
app.config['MAX_CONTENT_LENGTH'] = 16 * 1024 * 1024


@app.route("/api/health")
def health():
    registry = _load_registry()
    return jsonify({
        "status": "ok",
        "service": "pi-face-register",
        "registered_users": len(registry),
        "users": list(registry.keys()),
    })


@app.route("/api/register", methods=["POST"])
def register():
    if "photo" not in request.files:
        return jsonify({"success": False, "error": "缺少 photo 文件"}), 400

    name = request.form.get("name", "").strip()
    if not name:
        return jsonify({"success": False, "error": "姓名不能为空"}), 400

    photo = request.files["photo"]
    if photo.filename == "":
        return jsonify({"success": False, "error": "未选择文件"}), 400

    try:
        file_bytes = photo.read()
        nparr = np.frombuffer(file_bytes, np.uint8)
        img_bgr = cv2.imdecode(nparr, cv2.IMREAD_COLOR)

        if img_bgr is None:
            return jsonify({"success": False, "error": "无法解码图片"}), 400

        result = _register_from_image(img_bgr, name)
        status_code = 200 if result["success"] else 400
        return jsonify(result), status_code

    except Exception as e:
        traceback.print_exc()
        return jsonify({"success": False, "error": f"注册异常: {e}"}), 500


@app.route("/api/features", methods=["GET", "DELETE"])
def manage_features():
    if request.method == "DELETE":
        name = request.args.get("name", "").strip()
        if not name:
            return jsonify({"success": False, "error": "缺少 name 参数"}), 400
        path = os.path.join(FEATURES_DIR, f"{name}.bin")
        if os.path.exists(path):
            os.remove(path)
            return jsonify({"success": True, "message": f"已删除 {name}"})
        return jsonify({"success": False, "error": f"{name} 不存在"}), 404

    registry = _load_registry()
    return jsonify({
        "count": len(registry),
        "users": list(registry.keys()),
        "feature_dir": FEATURES_DIR,
    })


# ── 入口 ──
if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Pi 端注册 API 服务")
    parser.add_argument("--port", type=int, default=5000)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--debug", action="store_true")
    args = parser.parse_args()

    print(f"[pi_server] 启动: http://{args.host}:{args.port}")
    app.run(host=args.host, port=args.port, debug=args.debug)
