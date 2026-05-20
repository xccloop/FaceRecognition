"""
Windows 后台 — FastAPI 主入口

第一身份：手机小程序的后台服务器（接收注册）
附加功能：管理面板（浏览器查看摄像头、管理人员）

启动方式：
  开发：python main.py
  打包：pyinstaller --onefile main.py
"""
import sys
import os
import shutil
import uuid
import webbrowser
from datetime import datetime
from typing import List, Optional

from fastapi import FastAPI, Depends, HTTPException, UploadFile, File, Form
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
from fastapi.middleware.cors import CORSMiddleware
from sqlalchemy.orm import Session
from sqlalchemy import func

from models import User, engine, Base
from database import get_db
from config import SERVER_HOST, SERVER_PORT

# 初始化数据库
Base.metadata.create_all(bind=engine)

# ── 路径常量 ──────────────────────────────────────────────────
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
PHOTOS_DIR = os.path.join(BASE_DIR, "photos")
FRONTEND_DIST = os.path.join(BASE_DIR, "frontend", "dist")
os.makedirs(PHOTOS_DIR, exist_ok=True)

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
    # 校验
    if not name or not name.strip():
        raise HTTPException(400, "姓名不能为空")

    name = name.strip()
    if len(name) > 50:
        raise HTTPException(400, "姓名过长")

    if not photo.filename:
        raise HTTPException(400, "请上传照片")

    # 读取并保存照片
    ext = os.path.splitext(photo.filename or "photo.jpg")[1].lower()
    if ext not in (".jpg", ".jpeg", ".png", ".bmp"):
        raise HTTPException(400, "仅支持 JPG/PNG/BMP 格式")

    filename = f"{uuid.uuid4().hex}{ext}"
    filepath = os.path.join(PHOTOS_DIR, filename)

    content = await photo.read()
    if len(content) > 5 * 1024 * 1024:  # 5MB
        raise HTTPException(400, "照片过大（最大 5MB）")

    with open(filepath, "wb") as f:
        f.write(content)

    # 写入数据库
    user = User(
        name=name,
        photo_url=f"/photos/{filename}",
        created_at=datetime.utcnow(),
    )
    db.add(user)
    db.commit()
    db.refresh(user)

    return JSONResponse({
        "success": True,
        "user_id": user.id,
        "name": user.name,
        "photo_url": user.photo_url,
    })


# ═══════════════════════════════════════════════════════════════
# 管理面板 API
# ═══════════════════════════════════════════════════════════════

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

    return {
        "user_count": total,
        "pi_online": False,
        "camera_fps": 0,
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
                "pi_synced": False,
            }
            for u in users
        ],
    }


@app.delete("/api/users/{user_id}")
def api_delete_user(user_id: int, db: Session = Depends(get_db)):
    """删除人员（同时删除照片文件）"""
    user = db.query(User).filter(User.id == user_id).first()
    if not user:
        raise HTTPException(404, "用户不存在")

    # 删除照片文件
    if user.photo_url:
        filepath = os.path.join(BASE_DIR, user.photo_url.lstrip("/"))
        if os.path.isfile(filepath):
            try:
                os.remove(filepath)
            except OSError:
                pass

    db.delete(user)
    db.commit()
    return {"success": True}


@app.get("/api/camera/status")
def api_camera_status():
    """摄像头状态（树莓派未上线时返回占位）"""
    return {
        "pi_online": False,
        "stream_url": "",
        "camera_fps": 0,
        "pi_uptime": 0,
    }


# ═══════════════════════════════════════════════════════════════
# 静态文件
# ═══════════════════════════════════════════════════════════════

# 照片目录
app.mount("/photos", StaticFiles(directory=PHOTOS_DIR), name="photos")


@app.get("/{full_path:path}")
async def serve_frontend(full_path: str):
    """前端 SPA：非 API 路径返回 index.html"""
    # API 路径不拦截
    if full_path.startswith("api/") or full_path.startswith("photos/"):
        raise HTTPException(status_code=404)

    filepath = (
        os.path.join(FRONTEND_DIST, full_path)
        if full_path
        else os.path.join(FRONTEND_DIST, "index.html")
    )

    if os.path.isfile(filepath):
        return FileResponse(filepath)
    return FileResponse(os.path.join(FRONTEND_DIST, "index.html"))


# ── 主入口 ──────────────────────────────────────────────────

if __name__ == "__main__":
    import uvicorn

    # 打包后 static 路径修正
    if getattr(sys, "frozen", False):
        # PyInstaller --onefile 环境
        # 重新赋值模块级别变量（PyInstaller 将资源放在 sys._MEIPASS）
        _meipass = sys._MEIPASS  # type: ignore[name-defined]
        new_photos = os.path.join(os.path.dirname(sys.executable), "photos")
        new_dist = os.path.join(_meipass, "frontend", "dist")
        os.makedirs(new_photos, exist_ok=True)

        # 覆盖模块级路径常量
        import main as _main
        _main.PHOTOS_DIR = new_photos
        _main.FRONTEND_DIST = new_dist

    # 自动打开浏览器
    webbrowser.open(f"http://localhost:{SERVER_PORT}")

    print(f"""
╔══════════════════════════════════════════╗
║   FaceRecognition 后台 v1.0             ║
║   管理面板: http://localhost:{SERVER_PORT}    ║
║   API 文档: http://localhost:{SERVER_PORT}/docs ║
╚══════════════════════════════════════════╝
    """)

    uvicorn.run(
        "main:app",
        host=SERVER_HOST,
        port=SERVER_PORT,
        reload=not getattr(sys, "frozen", False),
    )
