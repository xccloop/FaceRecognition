# Windows 后台 — 待完成事项

## 当前状态

尚未开始。需要在 Windows 上搭建一个后台服务，承接手机小程序的人脸注册请求，并将特征数据下发到树莓派。

## 1. 技术选型与项目初始化

**推荐技术栈：**

| 层 | 选型 | 理由 |
|----|------|------|
| Web 框架 | FastAPI (Python) | 异步、自带 Swagger 文档、轻量 |
| 数据库 | SQLite + SQLAlchemy | 零配置，人员数据量小无需独立数据库 |
| 文件存储 | 本地文件系统 | 原始照片按 `photos/{user_id}/` 存放 |
| 推理 | 复用现有 verify.py | `detect()` + `extract()` 已验证可用 |
| SSH/SCP | paramiko | 将 .bin 特征文件传到树莓派 |
| 部署 | 直接 python 运行 或 NSSM 注册服务 | Windows 后台自启 |

**项目结构：**
```
Windows/
├── main.py              # FastAPI 入口
├── config.py            # 配置文件
├── models.py            # SQLAlchemy 模型
├── database.py          # 数据库连接
├── routers/
│   ├── users.py         # 用户 CRUD API
│   └── sync.py          # 树莓派同步 API
├── services/
│   ├── face_service.py  # 调用推理脚本提取特征
│   └── pi_service.py    # 与树莓派通信（SCP + SSH）
├── photos/              # 存储原始注册照片
├── features/            # 存储提取的特征 .bin 文件
├── requirements.txt
└── doc/
    └── tasks.md
```

## 2. 数据库设计

```python
# models.py
class User(Base):
    __tablename__ = "users"
    id          = Column(String, primary_key)      # "001"
    name        = Column(String, nullable=False)    # "张三"
    photo_path  = Column(String)                    # 原始照片路径
    feature_path = Column(String)                   # .bin 特征文件路径
    status      = Column(String, default="active")  # active / deleted
    created_at  = Column(DateTime)
    synced_at   = Column(DateTime)                  # 最后同步到 Pi 的时间

class SyncLog(Base):
    __tablename__ = "sync_logs"
    id          = Column(Integer, pk, autoincrement)
    action      = Column(String)                    # register / delete
    user_id     = Column(String)
    success     = Column(Boolean)
    message     = Column(String)
    created_at  = Column(DateTime)
```

## 3. API 接口

### 3.1 用户注册

```
POST /api/users/register
Content-Type: multipart/form-data

参数:
  photo: 图片文件 (jpg/png)
  name:  姓名 (string)
  id:    工号/编号 (string, 可选，不传则自动生成)

流程:
  1. 保存原始照片到 photos/{id}/
  2. 调用 face_service.extract(photo) → 512 维特征向量
  3. 保存 .bin 特征文件到 features/{id}.bin
  4. 写入数据库
  5. 通过 SCP 将 .bin 文件推送到树莓派 features/ 目录
  6. 通过 SSH 执行树莓派上的 reload 命令（或发送信号）
  7. 返回 { id, name, status, message }

错误处理:
  - 照片中无人脸 → 400 "No face detected in photo"
  - 照片人脸太小(<300px) → 400 "Face too small, please retake"
  - Pi 不可达 → 201 但标记 synced_at=null，后台定时重试
```

### 3.2 用户查询

```
GET  /api/users                — 列出所有用户
GET  /api/users/{id}           — 查询单个用户详情
DELETE /api/users/{id}         — 删除用户（软删除，同步删除 Pi 上的 .bin）
```

### 3.3 同步管理

```
POST /api/sync/pi              — 手动触发全量同步到 Pi
GET  /api/sync/status          — 查看同步状态（哪些已同步、哪些待同步）
POST /api/sync/pi/{id}         — 单独同步某个用户
```

### 3.4 健康检查

```
GET  /api/health               — 返回 { status: "ok", pi_connected: true/false }
```

## 4. 核心服务实现

### 4.1 人脸特征提取服务 (`services/face_service.py`)

```python
# 直接复用已验证的 verify.py 中的函数
import sys
sys.path.insert(0, r"D:\FaceRecognition\Linux\Model")
from verify import detect, extract, MATCH_THRESH

def extract_from_photo(photo_path: str) -> list[float]:
    img = cv2.imread(photo_path)
    faces = detect(img, score_thresh=0.3)
    if not faces:
        raise NoFaceError("No face detected")
    faces.sort(key=lambda f: f[4], reverse=True)
    f = faces[0]
    if f[4] < 0.5:
        raise FaceTooSmallError(f"Face score too low: {f[4]:.2f}")
    aligned = align(img, f[5])
    return extract(aligned)
```

### 4.2 树莓派通信服务 (`services/pi_service.py`)

```python
import paramiko

class PiService:
    def __init__(self, host, user, password):
        self.host = host
        self.ssh = paramiko.SSHClient()
        self.ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())

    def push_feature(self, local_path: str, remote_name: str):
        """SCP 上传 .bin 特征文件到 Pi"""
        sftp = self.ssh.open_sftp()
        sftp.put(local_path, f"/home/pi/face_recognition/features/{remote_name}.bin")
        sftp.close()

    def reload_features(self):
        """通知 Pi 重新加载特征库"""
        # 方式1: 发送 SIGUSR1 信号给 face_recog 进程
        self.ssh.exec_command("pkill -USR1 face_recog")
        # 方式2: 写一个 trigger 文件，Pi 端 inotify 监控
        # self.ssh.exec_command("touch /home/pi/face_recognition/data/.reload")

    def is_reachable(self) -> bool:
        """检查 Pi 是否在线"""
        try:
            self.ssh.connect(self.host, username=..., password=..., timeout=3)
            return True
        except:
            return False
```

## 5. 配置文件

```python
# config.py
import os

class Config:
    # 服务
    HOST = "0.0.0.0"
    PORT = 8000

    # 数据库
    DATABASE_URL = "sqlite:///face_recognition.db"

    # 存储路径
    PHOTO_DIR = os.path.join(os.path.dirname(__file__), "photos")
    FEATURE_DIR = os.path.join(os.path.dirname(__file__), "features")

    # 推理脚本路径
    VERIFY_PATH = r"D:\FaceRecognition\Linux\Model"

    # 树莓派连接
    PI_HOST = "192.168.1.100"
    PI_USER = "pi"
    PI_PASSWORD = "raspberry"
    PI_FEATURE_DIR = "/home/pi/face_recognition/features"

    # 识别阈值
    SCORE_THRESH = 0.5
    MATCH_THRESH = 0.4
```

## 6. 运行方式

```bash
cd D:\FaceRecognition\Windows
pip install fastapi uvicorn sqlalchemy paramiko opencv-python onnxruntime numpy
python main.py
```

或打包为 Windows 服务（使用 NSSM）：
```powershell
nssm install FaceRecognitionBackend python main.py
nssm start FaceRecognitionBackend
```

## 7. 后续扩展

- **识别日志回传：** 树莓派定期将识别记录通过 HTTP POST 回传给 Windows 后台，后台存储供查询
- **Web 管理页面：** Vue.js + Element Plus 做简单的管理后台（用户列表、注册、删除、识别日志查看）
- **多 Pi 支持：** 一台 Windows 管理多台树莓派（不同门禁点）
