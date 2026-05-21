"""Pi 端注册 API 服务

Flask HTTP API，接收 Windows 端发来的照片+姓名，
调用本地 verify.py 完成人脸检测→对齐→特征提取→保存 .bin，
返回注册结果（含人脸得分、特征维度等）。

启动方式：
  python pi_server.py                    # 默认端口 5000
  python pi_server.py --port 8080        # 指定端口

端点：
  GET  /api/health                       # 在线检测
  POST /api/register                     # 注册（multipart: photo + name）
  POST /api/register/base64              # 注册（JSON: base64_image + name）
  GET  /api/features                     # 列出已注册特征

依赖（树莓派端）：
  pip install flask numpy opencv-python onnxruntime
  verify.py + models/ 目录
"""

import sys
import os
import io
import json
import base64
import argparse
import traceback
from pathlib import Path

import cv2
import numpy as np
from flask import Flask, request, jsonify

# ── 切换到脚本目录（确保 verify.py 的 MODEL_DIR/FEAT_DIR 相对路径正确）──
SCRIPT_DIR = Path(__file__).resolve().parent
os.chdir(SCRIPT_DIR)

# ── 导入 verify.py 函数（ONNX 模型会在此时加载） ──
print(f"[pi_server] 脚本目录: {SCRIPT_DIR}")
print(f"[pi_server] 加载 ONNX 模型...")

from verify import detect, align, extract, save_feat, load_registry, MATCH_THRESH

print(f"[pi_server] 模型加载完成")

# ── Flask 应用 ──
app = Flask(__name__)

# 限制上传大小 16MB
app.config['MAX_CONTENT_LENGTH'] = 16 * 1024 * 1024


def _register_from_image(img_bgr: np.ndarray, name: str) -> dict:
    """从 OpenCV BGR 图像执行注册流程，返回结果字典"""
    faces = detect(img_bgr)
    if not faces:
        return {"success": False, "error": "未检测到人脸", "face_score": None}

    # 取置信度最高的人脸
    faces.sort(key=lambda f: f[4], reverse=True)
    f = faces[0]
    face_score = float(f[4])
    bbox = [float(f[0]), float(f[1]), float(f[2]), float(f[3])]

    # 对齐 → 提取特征
    aligned = align(img_bgr, f[5])
    feat = extract(aligned)

    # 保存特征
    save_feat(name, feat)

    return {
        "success": True,
        "name": name,
        "face_score": round(face_score, 4),
        "feature_dim": len(feat),
        "bbox": [round(v, 1) for v in bbox],
        "message": f"'{name}' 注册成功，特征已保存",
    }


@app.route("/api/health")
def health():
    """在线检测"""
    registry = load_registry()
    return jsonify({
        "status": "ok",
        "service": "pi-face-register",
        "registered_users": len(registry),
        "users": list(registry.keys()),
    })


@app.route("/api/register", methods=["POST"])
def register():
    """
    注册人脸（multipart/form-data）

    参数：
      photo: 图片文件（jpg/png）
      name:  用户姓名（form field）
    """
    if "photo" not in request.files:
        return jsonify({"success": False, "error": "缺少 photo 文件"}), 400

    name = request.form.get("name", "").strip()
    if not name:
        return jsonify({"success": False, "error": "姓名不能为空"}), 400

    photo = request.files["photo"]
    if photo.filename == "":
        return jsonify({"success": False, "error": "未选择文件"}), 400

    try:
        # 读取图片字节流，解码为 OpenCV 格式
        file_bytes = photo.read()
        nparr = np.frombuffer(file_bytes, np.uint8)
        img_bgr = cv2.imdecode(nparr, cv2.IMREAD_COLOR)

        if img_bgr is None:
            return jsonify({"success": False, "error": "无法解码图片，请确认格式为 JPG/PNG"}), 400

        result = _register_from_image(img_bgr, name)
        status_code = 200 if result["success"] else 400
        return jsonify(result), status_code

    except Exception as e:
        traceback.print_exc()
        return jsonify({"success": False, "error": f"注册异常: {e}"}), 500


@app.route("/api/register/base64", methods=["POST"])
def register_base64():
    """
    注册人脸（JSON，base64 图片）

    请求体：
      {"image": "<base64>", "name": "张三"}
    """
    data = request.get_json(silent=True)
    if not data:
        return jsonify({"success": False, "error": "请求体不是有效 JSON"}), 400

    name = data.get("name", "").strip()
    if not name:
        return jsonify({"success": False, "error": "姓名不能为空"}), 400

    img_b64 = data.get("image", "")
    if not img_b64:
        return jsonify({"success": False, "error": "缺少 image 字段"}), 400

    # 去除可能的 data:image/...;base64, 前缀
    if "," in img_b64:
        img_b64 = img_b64.split(",", 1)[1]

    try:
        img_bytes = base64.b64decode(img_b64)
        nparr = np.frombuffer(img_bytes, np.uint8)
        img_bgr = cv2.imdecode(nparr, cv2.IMREAD_COLOR)

        if img_bgr is None:
            return jsonify({"success": False, "error": "无法解码 base64 图片"}), 400

        result = _register_from_image(img_bgr, name)
        status_code = 200 if result["success"] else 400
        return jsonify(result), status_code

    except Exception as e:
        traceback.print_exc()
        return jsonify({"success": False, "error": f"注册异常: {e}"}), 500


@app.route("/api/features")
def list_features():
    """列出已注册的特征"""
    registry = load_registry()
    return jsonify({
        "count": len(registry),
        "users": list(registry.keys()),
        "feature_dir": str(SCRIPT_DIR / "features"),
    })


# ── 入口 ──
if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Pi 端注册 API 服务")
    parser.add_argument("--port", type=int, default=5000, help="服务端口（默认 5000）")
    parser.add_argument("--host", default="0.0.0.0", help="绑定地址（默认 0.0.0.0）")
    parser.add_argument("--debug", action="store_true", help="调试模式")
    args = parser.parse_args()

    print(f"[pi_server] 启动 HTTP API 服务")
    print(f"[pi_server] 地址: http://{args.host}:{args.port}")
    print(f"[pi_server] 端点:")
    print(f"  GET  /api/health        → 在线检测")
    print(f"  POST /api/register      → 注册（multipart）")
    print(f"  POST /api/register/base64 → 注册（base64）")
    print(f"  GET  /api/features      → 列出注册用户")

    app.run(host=args.host, port=args.port, debug=args.debug)
