"""
Windows 后台 — 全局配置
"""
import os

# 项目根目录（开发时为本文件所在目录，打包时为 sys._MEIPASS）
BASE_DIR = os.path.dirname(os.path.abspath(__file__))

# 数据库
DATABASE_URL = f"sqlite:///{os.path.join(BASE_DIR, 'face_recognition.db')}"

# 照片存储目录
PHOTOS_DIR = os.path.join(BASE_DIR, "photos")
os.makedirs(PHOTOS_DIR, exist_ok=True)

# 树莓派配置
PI_HOST = os.getenv("PI_HOST", "192.168.1.100")
PI_STREAM_URL = os.getenv("PI_STREAM_URL", f"http://{PI_HOST}:8080/stream")
PI_SSH_PORT = int(os.getenv("PI_SSH_PORT", "22"))
PI_USER = os.getenv("PI_USER", "pi")
PI_FEATURES_DIR = os.getenv("PI_FEATURES_DIR", "/home/pi/features/")

# 服务器
SERVER_HOST = os.getenv("SERVER_HOST", "0.0.0.0")
SERVER_PORT = int(os.getenv("SERVER_PORT", "8000"))

# 心跳超时（秒）
HEARTBEAT_TIMEOUT = int(os.getenv("HEARTBEAT_TIMEOUT", "15"))

# 上传限制
MAX_PHOTO_SIZE = 5 * 1024 * 1024  # 5MB
ALLOWED_PHOTO_TYPES = ["image/jpeg", "image/png", "image/jpg"]
