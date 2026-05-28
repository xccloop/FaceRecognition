"""
Windows 后台 — 全局配置
"""
import os
import sys

# 项目根目录
# 开发时 = 本文件所在目录
# 打包后 sys._MEIPASS 不可写，数据文件应放到 exe 同目录
IS_FROZEN = getattr(sys, 'frozen', False)
if IS_FROZEN:
    EXE_DIR = os.path.dirname(sys.executable)
    _MEIPASS = sys._MEIPASS  # type: ignore[name-defined]
else:
    EXE_DIR = os.path.dirname(os.path.abspath(__file__))
    _MEIPASS = EXE_DIR

# BASE_DIR：只读资源路径（MEIPASS），前端 dist、模型文件等
BASE_DIR = _MEIPASS

# DATA_DIR：可写数据路径（exe 同目录），DB、照片、日志、配置
DATA_DIR = EXE_DIR
os.makedirs(DATA_DIR, exist_ok=True)

# 数据库
DATABASE_URL = f"sqlite:///{os.path.join(DATA_DIR, 'face_recognition.db')}"

# 照片存储目录
PHOTOS_DIR = os.path.join(DATA_DIR, "photos")
os.makedirs(PHOTOS_DIR, exist_ok=True)

# 树莓派配置
PI_HOST = os.getenv("PI_HOST", "192.168.137.100")
PI_STREAM_URL = os.getenv("PI_STREAM_URL", f"http://{PI_HOST}:8080/video")
PI_SSH_PORT = int(os.getenv("PI_SSH_PORT", "22"))
PI_USER = os.getenv("PI_USER", "pi")
PI_FEATURES_DIR = os.getenv("PI_FEATURES_DIR", "/home/pi/features/")

# 服务器
SERVER_HOST = os.getenv("SERVER_HOST", "0.0.0.0")
SERVER_PORT = int(os.getenv("SERVER_PORT", "8081"))

# 心跳超时（秒）
HEARTBEAT_TIMEOUT = int(os.getenv("HEARTBEAT_TIMEOUT", "15"))

# 上传限制
MAX_PHOTO_SIZE = 5 * 1024 * 1024  # 5MB
ALLOWED_PHOTO_TYPES = ["image/jpeg", "image/png", "image/jpg"]
