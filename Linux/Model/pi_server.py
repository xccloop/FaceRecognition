"""Pi 端注册 API 服务

Flask HTTP API，接收 Windows 端发来的照片+姓名，
调用 C++ facerec register 完成人脸检测→对齐→特征提取→保存 .bin，
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
  pip install flask numpy opencv-python
  facerec (C++ 编译产物) 需在 ../build/facerec 或 PATH 中
"""

import sys
import os
import io
import json
import base64
import struct
import argparse
import subprocess
import tempfile
import traceback
from pathlib import Path

import cv2
import numpy as np
from flask import Flask, request, jsonify

# ── 配置 ──
SCRIPT_DIR = Path(__file__).resolve().parent
os.chdir(SCRIPT_DIR)

# C++ facerec 可执行文件路径
FACEREC_BIN = os.environ.get("FACEREC_BIN",
    str(SCRIPT_DIR / "build" / "facerec"))

print(f"[pi_server] 脚本目录: {SCRIPT_DIR}")
print(f"[pi_server] facerec 路径: {FACEREC_BIN}")
print(f"[pi_server] 初始化完成")

# ── Flask 应用 ──
app = Flask(__name__)

# 限制上传大小 16MB
app.config['MAX_CONTENT_LENGTH'] = 16 * 1024 * 1024


def _register_from_image(img_bgr: np.ndarray, name: str) -> dict:
    """将图像保存为临时文件，调用 C++ facerec register 完成注册"""
    # 检查 facerec 是否存在
    if not os.path.exists(FACEREC_BIN):
        return {"success": False, "error": f"facerec 未找到: {FACEREC_BIN}"}

    # 写入临时文件
    with tempfile.NamedTemporaryFile(suffix=".jpg", delete=False) as tmp:
        tmp_path = tmp.name
        cv2.imwrite(tmp_path, img_bgr)

    try:
        # 调用 C++ facerec register
        result = subprocess.run(
            [FACEREC_BIN, "register", tmp_path, name],
            capture_output=True, text=True, timeout=30,
            cwd=str(SCRIPT_DIR / "build")
        )
        stdout = result.stdout.strip()
        stderr = result.stderr.strip()

        if result.returncode != 0:
            error_msg = stderr or stdout or f"退出码 {result.returncode}"
            # 解析常见错误
            if "CMD_NOFACE" in error_msg:
                return {"success": False, "error": "未检测到人脸", "face_score": None}
            return {"success": False, "error": error_msg}

        # 解析输出
        face_score = None
        for line in stdout.split("\n"):
            if "score=" in line:
                try:
                    face_score = float(line.split("score=")[1].split()[0])
                except (ValueError, IndexError):
                    pass

        # 验证特征文件已生成
        feat_path = SCRIPT_DIR / "build" / "features" / f"{name}.bin"
        feature_dim = 0
        if feat_path.exists():
            with open(feat_path, "rb") as f:
                dim_bytes = f.read(4)
                if len(dim_bytes) == 4:
                    feature_dim = struct.unpack("i", dim_bytes)[0]

        return {
            "success": True,
            "name": name,
            "face_score": round(face_score, 4) if face_score else None,
            "feature_dim": feature_dim,
            "message": f"'{name}' 注册成功，特征已保存 (dim={feature_dim})",
        }

    except subprocess.TimeoutExpired:
        return {"success": False, "error": "注册超时（>30s）"}
    finally:
        os.unlink(tmp_path)


def _load_registry():
    """读取 features/ 目录下的 .bin 文件列表"""
    feat_dir = SCRIPT_DIR / "build" / "features"
    if not feat_dir.exists():
        return {}
    registry = {}
    for fn in feat_dir.iterdir():
        if fn.suffix == ".bin":
            registry[fn.stem] = str(fn)
    return registry


@app.route("/api/health")
def health():
    """在线检测"""
    registry = _load_registry()
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
    registry = _load_registry()
    return jsonify({
        "count": len(registry),
        "users": list(registry.keys()),
        "feature_dir": str(SCRIPT_DIR / "build" / "features"),
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
