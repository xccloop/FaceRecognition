"""
Windows 后台 — FastAPI 主入口

第一身份：手机小程序的后台服务器（接收注册）
附加功能：管理面板（原生桌面窗口查看摄像头、管理人员）

启动方式：
  开发：python main.py
  打包：pyinstaller --onefile main.py
"""
import sys
import os
import json
import shutil
import uuid
import threading
from datetime import datetime
from typing import List, Optional

import webview
from fastapi import Body, FastAPI, Depends, HTTPException, Request, UploadFile, File, Form
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
from fastapi.middleware.cors import CORSMiddleware
from sqlalchemy.orm import Session
from sqlalchemy import func

from models import User, SystemStatus, RecognitionLog, SyncTask, engine, Base
from database import get_db
from config import SERVER_HOST, SERVER_PORT

# 初始化数据库
Base.metadata.create_all(bind=engine)

# 兼容旧数据库：为新表/新字段做自动迁移
from sqlalchemy import text as _text
_conn = engine.connect()
try:
    try:
        _conn.execute(_text("ALTER TABLE users ADD COLUMN pi_sync_status VARCHAR(16) DEFAULT 'pending'"))
        _conn.execute(_text("ALTER TABLE users ADD COLUMN pi_sync_error VARCHAR(512) DEFAULT ''"))
    except Exception:
        pass  # 列已存在
    _conn.commit()
finally:
    _conn.close()

# 确保 system_status 有默认行
_init_session = __import__("models").SessionLocal()
try:
    if not _init_session.query(SystemStatus).filter(SystemStatus.id == 1).first():
        _init_session.add(SystemStatus(id=1))
        _init_session.commit()
finally:
    _init_session.close()

# ── 路径常量 ──────────────────────────────────────────────────
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
PHOTOS_DIR = os.path.join(BASE_DIR, "photos")
FRONTEND_DIST = os.path.join(BASE_DIR, "frontend", "dist")
os.makedirs(PHOTOS_DIR, exist_ok=True)

# ── 运行时配置持久化 ──────────────────────────────────────────
CONFIG_FILE = os.path.join(BASE_DIR, "config.json")

DEFAULT_CONFIG = {
    "pi_host": "192.168.137.100",
    "pi_stream_port": 8080,
    "pi_api_port": 5000,
    "pi_ssh_port": 22,
    "pi_user": "qxc",
    "pi_password": "root",
    "pi_features_dir": "/home/qxc/Desktop/FaceRec/Model/features/",
    "server_port": SERVER_PORT,
    "server_host": SERVER_HOST,
    "heartbeat_timeout": 15,
}


def load_runtime_config() -> dict:
    """加载运行时配置，优先读 config.json，回退到默认值"""
    try:
        if os.path.isfile(CONFIG_FILE):
            with open(CONFIG_FILE, "r") as f:
                saved = json.load(f)
            return {**DEFAULT_CONFIG, **saved}
    except Exception:
        pass
    return dict(DEFAULT_CONFIG)


def save_runtime_config(data: dict) -> None:
    """保存配置到 JSON 文件"""
    with open(CONFIG_FILE, "w") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)


# ── 导入 Pi 同步模块 ──────────────────────────────────────────
try:
    from sync import (
        check_pi_online as _check_pi_online,
        sync_user_photo as _sync_user_photo,
        sync_all_unsynced as _sync_all_unsynced,
        enqueue_sync_task as _enqueue_sync_task,
        start_sync_worker as _start_sync_worker,
        delete_pi_user as _delete_pi_user,
    )
    _SYNC_AVAILABLE = True
except ImportError:
    _SYNC_AVAILABLE = False
    print("[main] sync.py 未找到或缺少依赖 (paramiko)，Pi 同步功能不可用")

# 启动后台同步 worker
if _SYNC_AVAILABLE:
    _start_sync_worker()
    print("[main] Sync worker started")

# ── FastAPI 应用 ──────────────────────────────────────────────
app = FastAPI(
    title="FaceRecognition 后台",
    version="1.0.0",
    docs_url="/docs",
    redoc_url=None,
)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# ── 请求日志中间件（诊断用）──────────────────────────────────
LOG_FILE = os.path.join(BASE_DIR, "request.log")


@app.middleware("http")
async def log_requests(request: Request, call_next):
    """记录所有请求到文件，便于排查 --windowed 模式下的问题"""
    import time as _time
    t0 = _time.time()
    response = await call_next(request)
    dt = (_time.time() - t0) * 1000
    line = f"[{datetime.now().strftime('%H:%M:%S')}] {request.method:6s} {request.url.path} → {response.status_code} ({dt:.0f}ms)\n"
    try:
        with open(LOG_FILE, "a") as f:
            f.write(line)
    except Exception:
        pass
    return response


# ═══════════════════════════════════════════════════════════════
# 手机小程序 API（第一身份）
# ═══════════════════════════════════════════════════════════════

@app.post("/api/register")
async def api_register(
    photo: UploadFile = File(...),
    name: str = Form(...),
    db: Session = Depends(get_db),
):
    """手机小程序注册接口：上传照片 + 姓名"""
    if not name or not name.strip():
        raise HTTPException(400, "姓名不能为空")

    name = name.strip()
    if len(name) > 50:
        raise HTTPException(400, "姓名过长")

    if not photo.filename:
        raise HTTPException(400, "请上传照片")

    ext = os.path.splitext(photo.filename or "photo.jpg")[1].lower()
    if ext not in (".jpg", ".jpeg", ".png", ".bmp"):
        raise HTTPException(400, "仅支持 JPG/PNG/BMP 格式")

    filename = f"{uuid.uuid4().hex}{ext}"
    filepath = os.path.join(PHOTOS_DIR, filename)

    content = await photo.read()
    if len(content) > 5 * 1024 * 1024:
        raise HTTPException(400, "照片过大（最大 5MB）")

    with open(filepath, "wb") as f:
        f.write(content)

    user = User(
        name=name,
        photo_url=f"/photos/{filename}",
        created_at=datetime.utcnow(),
        pi_sync_status="pending",
    )
    db.add(user)
    db.commit()
    db.refresh(user)

    # 注册后创建 SyncTask 持久化任务，后台异步同步到 Pi
    if _SYNC_AVAILABLE:
        _enqueue_sync_task(db, user.id)

    return JSONResponse({
        "success": True,
        "user_id": user.id,
        "name": user.name,
        "photo_url": user.photo_url,
        "pi_sync_status": user.pi_sync_status,
        "privacy_notice": "内网专用系统，人脸照片仅在局域网传输，不经过外网。",
    })


# ═══════════════════════════════════════════════════════════════
# 管理面板 API
# ═══════════════════════════════════════════════════════════════

def _get_pi_status(db: Session):
    """从数据库读取 Pi 在线状态"""
    status = db.query(SystemStatus).filter(SystemStatus.id == 1).first()
    if status:
        return {
            "pi_online": status.pi_online,
            "camera_fps": status.camera_fps or 0,
            "pi_uptime": status.pi_uptime or 0,
        }
    return {"pi_online": False, "camera_fps": 0, "pi_uptime": 0}


@app.get("/api/dashboard")
def api_dashboard(db: Session = Depends(get_db)):
    """仪表盘数据"""
    total = db.query(func.count(User.id)).scalar() or 0

    recent = (
        db.query(User)
        .order_by(User.created_at.desc())
        .limit(5)
        .all()
    )

    pi_status = _get_pi_status(db)

    sync_failed = db.query(func.count(User.id)).filter(User.pi_sync_status == "failed").scalar() or 0
    sync_pending = db.query(func.count(User.id)).filter(User.pi_sync_status == "pending").scalar() or 0

    return {
        "user_count": total,
        "pi_online": pi_status["pi_online"],
        "camera_fps": pi_status["camera_fps"],
        "sync_failed": sync_failed,
        "sync_pending": sync_pending,
        "recent_users": [
            {
                "id": u.id,
                "name": u.name,
                "photo_url": u.photo_url,
                "created_at": (
                    u.created_at.strftime("%Y-%m-%d %H:%M")
                    if u.created_at else ""
                ),
            }
            for u in recent
        ],
    }


@app.get("/api/users")
def api_users(
    page: int = 1,
    page_size: int = 12,
    search: str = "",
    db: Session = Depends(get_db),
):
    """人员列表（分页 + 搜索）"""
    q = db.query(User)
    if search:
        q = q.filter(User.name.contains(search))
    total = q.count()
    users = (
        q.order_by(User.created_at.desc())
        .offset((page - 1) * page_size)
        .limit(page_size)
        .all()
    )

    return {
        "total": total,
        "page": page,
        "page_size": page_size,
        "items": [
            {
                "id": u.id,
                "name": u.name,
                "photo_url": u.photo_url,
                "created_at": (
                    u.created_at.strftime("%Y-%m-%d %H:%M")
                    if u.created_at else ""
                ),
                "pi_synced": u.pi_synced,
                "pi_sync_status": u.pi_sync_status,
            }
            for u in users
        ],
    }


@app.delete("/api/users/{user_id}")
def api_delete_user(user_id: int, db: Session = Depends(get_db)):
    """删除人员（仅删除数据库记录，保留照片文件）"""
    user = db.query(User).filter(User.id == user_id).first()
    if not user:
        raise HTTPException(404, "用户不存在")

    db.delete(user)
    db.commit()
    return {"success": True}


@app.delete("/api/users/{user_id}/pi")
def api_delete_user_from_pi(user_id: int, db: Session = Depends(get_db)):
    """从树莓派删除该用户的特征文件（保留 Windows 端数据）"""
    user = db.query(User).filter(User.id == user_id).first()
    if not user:
        raise HTTPException(404, "用户不存在")

    if not _SYNC_AVAILABLE:
        raise HTTPException(503, "Pi 同步模块不可用")

    result = _delete_pi_user(user.name)
    if result.get("success"):
        user.pi_synced = False
        db.commit()
    return result


@app.get("/api/users/{user_id}/sync-status")
def api_user_sync_status(user_id: int, db: Session = Depends(get_db)):
    """查询单个用户的 Pi 同步状态"""
    user = db.query(User).filter(User.id == user_id).first()
    if not user:
        raise HTTPException(404, "用户不存在")
    return {
        "user_id": user.id,
        "name": user.name,
        "pi_synced": user.pi_synced,
        "pi_sync_status": user.pi_sync_status,
        "pi_sync_error": user.pi_sync_error,
    }


@app.get("/api/privacy")
def api_privacy():
    """隐私声明"""
    return {
        "title": "隐私声明 - FaceRecognition 门禁系统",
        "notice": "本系统仅在局域网内运行。人脸照片通过 HTTP 在内网传输，不会上传到互联网。",
        "data_storage": "照片和特征数据存储在本地设备（Windows 后台 + 树莓派），不经过第三方服务。",
        "last_updated": "2026-05-26",
    }


# ═══════════════════════════════════════════════════════════════
# Pi 同步 API
# ═══════════════════════════════════════════════════════════════

@app.get("/api/pi/status")
def api_pi_status(db: Session = Depends(get_db)):
    """树莓派连接状态"""
    pi_status = _get_pi_status(db)

    # 尝试实时检测 Pi 是否可达
    online = False
    if _SYNC_AVAILABLE:
        try:
            online = _check_pi_online()
            # 更新数据库状态
            status = db.query(SystemStatus).filter(SystemStatus.id == 1).first()
            if status:
                status.pi_online = online
                status.updated_at = datetime.utcnow()
                db.commit()
        except Exception as e:
            print(f"[pi/status] 检测失败: {e}")

    return {
        **pi_status,
        "pi_online": online,
        "sync_available": _SYNC_AVAILABLE,
    }


@app.post("/api/users/{user_id}/sync")
def api_sync_user(user_id: int, db: Session = Depends(get_db)):
    """同步单个用户到树莓派"""
    if not _SYNC_AVAILABLE:
        raise HTTPException(503, "Pi 同步模块未加载，请安装 paramiko 依赖")

    user = db.query(User).filter(User.id == user_id).first()
    if not user:
        raise HTTPException(404, "用户不存在")

    photo_path = os.path.join(PHOTOS_DIR, os.path.basename(user.photo_url)) if user.photo_url else None
    if not photo_path or not os.path.isfile(photo_path):
        raise HTTPException(404, "照片文件不存在")

    result = _sync_user_photo(user, photo_path)
    if result.get("success"):
        user.pi_synced = True
        db.commit()
        return {"success": True, "message": f"用户 {user.name} 已同步到树莓派"}
    else:
        return JSONResponse(
            {"success": False, "error": result.get("error", "同步失败")},
            status_code=500,
        )


@app.post("/api/users/sync-all")
def api_sync_all(db: Session = Depends(get_db)):
    """批量同步所有未同步用户到树莓派"""
    if not _SYNC_AVAILABLE:
        raise HTTPException(503, "Pi 同步模块未加载，请安装 paramiko 依赖")

    result = _sync_all_unsynced(db)
    return result


# ═══════════════════════════════════════════════════════════════
# 系统配置 API
# ═══════════════════════════════════════════════════════════════

@app.get("/api/config")
def api_get_config():
    """获取当前运行时配置"""
    return load_runtime_config()


@app.put("/api/config")
async def api_save_config(request: Request):
    """保存运行时配置（立即生效，服务器参数需重启）

    使用 request.json() 直接读取请求体，避免 Body() 注解与 dict 类型
    在特定 FastAPI 版本中的兼容性问题。
    """
    payload = await request.json()
    cfg = load_runtime_config()
    # 只允许更新已知字段
    allowed = set(DEFAULT_CONFIG.keys())
    updated_count = 0
    for key in allowed:
        if key in payload:
            val = payload[key]
            if isinstance(DEFAULT_CONFIG[key], int):
                try:
                    val = int(val)
                except (TypeError, ValueError):
                    continue
            cfg[key] = val
            updated_count += 1
    try:
        save_runtime_config(cfg)
        print(f"[config] 已保存 {updated_count} 项配置到 {CONFIG_FILE}")
    except Exception as e:
        print(f"[config] 保存失败: {e}")
        raise HTTPException(500, f"写入配置文件失败: {e}")
    return {"success": True, "config": cfg, "config_file": CONFIG_FILE}


@app.get("/api/camera/status")
def api_camera_status(db: Session = Depends(get_db)):
    """摄像头状态（从运行时配置读取树莓派信息）"""
    cfg = load_runtime_config()
    pi_status = _get_pi_status(db)
    return {
        "pi_online": pi_status["pi_online"],
        "stream_url": f"http://{cfg['pi_host']}:{cfg['pi_stream_port']}/video",
        "camera_fps": pi_status["camera_fps"],
        "pi_uptime": pi_status["pi_uptime"],
    }


# ═══════════════════════════════════════════════════════════════
# 诊断端点
# ═══════════════════════════════════════════════════════════════

@app.get("/api/debug/routes")
def api_debug_routes():
    """列出所有已注册路由（用于诊断路由冲突）"""
    routes = []
    for r in app.routes:
        routes.append({
            "path": getattr(r, "path", str(r)),
            "methods": list(getattr(r, "methods", [])) if hasattr(r, "methods") else None,
            "name": getattr(r, "name", ""),
        })
    return {"routes": routes}


# ═══════════════════════════════════════════════════════════════
# 静态文件挂载（放在最后，API 路由优先匹配）
# ═══════════════════════════════════════════════════════════════

app.mount("/photos", StaticFiles(directory=PHOTOS_DIR), name="photos")

# 前端静态资源（JS/CSS/字体等）— 仅挂载 assets 子目录，避免根路径挂载拦截 API 路由
app.mount("/assets", StaticFiles(directory=os.path.join(FRONTEND_DIST, "assets")), name="assets")

# 前端入口 — SPA 使用 hash 路由（/#/settings 等），只需提供 index.html
@app.get("/")
async def serve_index():
    return FileResponse(os.path.join(FRONTEND_DIST, "index.html"))


# ── 原生桌面窗口 ────────────────────────────────────────────

def start_server():
    """在后台线程启动 FastAPI 服务器"""
    import uvicorn
    uvicorn.run(
        app,
        host=SERVER_HOST,
        port=SERVER_PORT,
        log_level="warning",
    )


def main():
    """主入口：启动服务器 + 打开原生桌面窗口"""
    # 打包后 static 路径修正
    if getattr(sys, "frozen", False):
        _meipass = sys._MEIPASS  # type: ignore[name-defined]
        exe_dir = os.path.dirname(sys.executable)
        new_photos = os.path.join(exe_dir, "photos")
        new_dist = os.path.join(_meipass, "frontend", "dist")
        new_log = os.path.join(exe_dir, "request.log")
        new_config = os.path.join(exe_dir, "config.json")
        os.makedirs(new_photos, exist_ok=True)

        # 使用 sys.modules['__main__'] 直接修改当前模块的全局变量
        # 不能用 import main as _xxx，因为 __name__ 是 "__main__" 时会导入另一个模块副本
        _self = sys.modules["__main__"]
        _self.PHOTOS_DIR = new_photos
        _self.FRONTEND_DIST = new_dist
        _self.LOG_FILE = new_log
        _self.CONFIG_FILE = new_config

        # 同步更新 StaticFiles 挂载的目录（模块加载时用旧路径已初始化）
        for route in app.routes:
            if getattr(route, "name", "") == "photos":
                route.app.directory = new_photos
                break

    # 后台线程启动 FastAPI
    server_thread = threading.Thread(target=start_server, daemon=True)
    server_thread.start()

    # 等待服务器就绪
    import time
    import urllib.request
    url = f"http://127.0.0.1:{SERVER_PORT}"
    for _ in range(30):
        try:
            urllib.request.urlopen(url, timeout=1)
            break
        except Exception:
            time.sleep(0.3)

    # 打开原生桌面窗口
    webview.create_window(
        title="人脸识别门禁系统 - 管理面板",
        url=url,
        width=1200,
        height=800,
        min_size=(900, 600),
        resizable=True,
        fullscreen=False,
    )
    webview.start()


if __name__ == "__main__":
    main()
