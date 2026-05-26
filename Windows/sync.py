"""
树莓派（Pi 端）同步模块 — HTTP API 版本 v2

功能：
  1. 检测树莓派 HTTP API 是否在线
  2. 通过 HTTP POST 将用户照片+姓名发送到 Pi 端 /api/register
  3. 注册前去重：GET /api/features 检查同名用户是否已存在
  4. SyncTask 持久化队列：进程重启不丢失待重试任务
  5. 后台 worker 线程：每 30s 轮询 SyncTask 表，指数退避重试

配置来源：
  config.json → pi_host, pi_api_port, heartbeat_timeout
"""

import os
import json
import logging
import threading
import time
from datetime import datetime
from typing import Optional

import requests

from config import EXE_DIR

logger = logging.getLogger("sync")

# ── 去重降级控制 ──
# True: 注册前必须去重检查（Pi 端接口就绪后启用）
# False: 降级模式 — Pi /api/features 不可用时跳过检查，继续注册
DEDUP_REQUIRED = False


def _load_config() -> dict:
    config_file = os.path.join(EXE_DIR, "config.json")
    defaults = {
        "pi_host": "192.168.137.100",
        "pi_api_port": 5000,
        "heartbeat_timeout": 5,
    }
    try:
        if os.path.isfile(config_file):
            with open(config_file, "r") as f:
                saved = json.load(f)
            if "pi_ssh_port" in saved:
                saved.setdefault("pi_api_port", 5000)
            return {**defaults, **saved}
    except Exception:
        pass
    return dict(defaults)


def _get_pi_url(endpoint: str = "") -> str:
    config = _load_config()
    return f"http://{config['pi_host']}:{config['pi_api_port']}{endpoint}"


def check_pi_online() -> bool:
    """通过 HTTP GET /api/health 检测树莓派是否在线"""
    config = _load_config()
    timeout = max(2, config.get("heartbeat_timeout", 5))
    try:
        resp = requests.get(_get_pi_url("/api/health"), timeout=timeout)
        if resp.status_code == 200:
            data = resp.json()
            logger.info(f"Pi online, registered users: {data.get('registered_users', 0)}")
            return True
        return False
    except requests.ConnectionError:
        logger.warning(f"Pi connection failed: {config['pi_host']}:{config['pi_api_port']}")
        return False
    except requests.Timeout:
        logger.warning(f"Pi connection timeout ({timeout}s)")
        return False
    except Exception as e:
        logger.warning(f"Pi check error: {e}")
        return False


def _check_pi_user_exists(name: str) -> Optional[bool]:
    """检查 Pi 上是否已存在同名用户。

    Returns:
        True: 已存在
        False: 不存在
        None: Pi 不可达或接口不支持（需降级处理）
    """
    try:
        config = _load_config()
        timeout = max(2, config.get("heartbeat_timeout", 5))
        resp = requests.get(_get_pi_url("/api/features"), timeout=timeout)
        if resp.status_code == 200:
            data = resp.json()
            users = data.get("users", [])
            return name in users
        elif resp.status_code == 404:
            logger.warning("Pi /api/features returned 404 — dedup unavailable, proceeding blindly")
            return None
        else:
            logger.warning(f"Pi /api/features returned {resp.status_code}")
            return None
    except (requests.ConnectionError, requests.Timeout) as e:
        logger.warning(f"Pi dedup check failed ({e}) — dedup unavailable")
        return None
    except Exception as e:
        logger.warning(f"Pi dedup check error: {e}")
        return None


def sync_user_photo(user, photo_path: str) -> dict:
    """同步单个用户到树莓派（HTTP POST + 去重检查）

    Args:
        user: SQLAlchemy User 对象
        photo_path: 本地照片文件绝对路径
    """
    if not os.path.isfile(photo_path):
        return {"success": False, "error": f"Photo not found: {photo_path}"}

    # ── 去重检查 ──
    exists = _check_pi_user_exists(user.name)
    if exists is True:
        logger.info(f"User '{user.name}' already exists on Pi, skipping register")
        return {"success": True, "skipped": True, "reason": "already_exists"}
    if exists is None and DEDUP_REQUIRED:
        return {"success": False, "error": "Dedup check failed (Pi unreachable), DEDUP_REQUIRED=True"}

    # ── 注册 ──
    config = _load_config()
    timeout = max(5, config.get("heartbeat_timeout", 15))

    try:
        with open(photo_path, "rb") as f:
            files = {"photo": (os.path.basename(photo_path), f, "image/jpeg")}
            data = {"name": user.name}
            resp = requests.post(_get_pi_url("/api/register"), files=files, data=data, timeout=timeout)

        result = resp.json()
        if resp.status_code == 200 and result.get("success"):
            logger.info(f"Synced: {user.name} face_score={result.get('face_score')} dim={result.get('feature_dim')}")
            return {"success": True, "face_score": result.get("face_score"), "feature_dim": result.get("feature_dim")}
        else:
            error_msg = result.get("error", f"HTTP {resp.status_code}")
            logger.error(f"Register failed: {user.name} -> {error_msg}")
            return {"success": False, "error": error_msg}

    except requests.ConnectionError as e:
        return {"success": False, "error": f"Pi connection failed: {e}"}
    except requests.Timeout:
        return {"success": False, "error": f"Pi response timeout ({timeout}s)"}
    except Exception as e:
        return {"success": False, "error": f"Sync exception: {e}"}


def delete_pi_user(name: str) -> dict:
    """从 Pi 端删除已注册用户的特征文件"""
    config = _load_config()
    timeout = max(5, config.get("heartbeat_timeout", 15))
    try:
        url = _get_pi_url(f"/api/features?name={name}")
        resp = requests.delete(url, timeout=timeout)
        result = resp.json()
        if resp.status_code == 200 and result.get("success"):
            logger.info(f"Deleted from Pi: {name}")
        else:
            logger.warning(f"Pi delete failed: {name} -> {result.get('error', 'unknown')}")
        return result
    except Exception as e:
        logger.warning(f"Pi delete error: {name} -> {e}")
        return {"success": False, "error": str(e)}


# ═══════════════════════════════════════════════════════════════
#  SyncTask Worker — 持久化重试队列
# ═══════════════════════════════════════════════════════════════

RETRY_INTERVALS = [2, 5, 10]  # 秒
MAX_RETRIES = 3

_worker_started = False


def start_sync_worker(app=None):
    """启动后台同步 worker 线程（应用启动时调用一次）"""
    global _worker_started
    if _worker_started:
        return
    _worker_started = True
    t = threading.Thread(target=_sync_worker_loop, daemon=True)
    t.start()
    logger.info("Sync worker started (polling every 30s)")


def _sync_worker_loop():
    """后台 worker：每 30s 轮询 SyncTask 表，处理待重试任务"""
    # 延迟导入避免循环依赖
    from models import SessionLocal, User, SyncTask

    time.sleep(5)  # 启动后等 5s，让 uvicorn 先就绪

    while True:
        try:
            db = SessionLocal()
            try:
                now = datetime.utcnow()
                tasks = (
                    db.query(SyncTask)
                    .filter(SyncTask.status.in_(["pending", "failed"]))
                    .filter(SyncTask.next_retry_at <= now)
                    .all()
                )

                for task in tasks:
                    _process_sync_task(db, task)

            finally:
                db.close()
        except Exception as e:
            logger.error(f"Sync worker error: {e}")

        time.sleep(30)


def _process_sync_task(db, task):
    """处理单个 SyncTask"""
    from models import User

    task.status = "processing"
    db.commit()

    user = db.query(User).filter(User.id == task.user_id).first()
    if not user:
        task.status = "failed"
        task.error_message = "User deleted"
        db.commit()
        return

    photo_path = None
    if user.photo_url:
        from config import EXE_DIR
        photo_path = os.path.join(EXE_DIR, "photos", os.path.basename(user.photo_url))
        if not os.path.isfile(photo_path):
            photo_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "photos", os.path.basename(user.photo_url))

    if not photo_path or not os.path.isfile(photo_path):
        task.status = "failed"
        task.error_message = "Photo file missing"
        user.pi_sync_status = "failed"
        user.pi_sync_error = task.error_message
        db.commit()
        return

    result = sync_user_photo(user, photo_path)

    if result.get("success"):
        task.status = "completed"
        task.error_message = ""
        user.pi_synced = True
        user.pi_sync_status = "synced"
        user.pi_sync_error = ""
        db.commit()
        logger.info(f"SyncTask #{task.id} ({user.name}) completed")
    else:
        task.retry_count += 1
        if task.retry_count < MAX_RETRIES:
            interval = RETRY_INTERVALS[min(task.retry_count, len(RETRY_INTERVALS) - 1)]
            task.status = "pending"
            task.next_retry_at = datetime.utcfromtimestamp(
                datetime.utcnow().timestamp() + interval
            )
            task.error_message = result.get("error", "")
            user.pi_sync_status = "pending"
            user.pi_sync_error = task.error_message
            db.commit()
            logger.info(f"SyncTask #{task.id} retry {task.retry_count}/{MAX_RETRIES} in {interval}s")
        else:
            task.status = "failed"
            task.error_message = result.get("error", "Max retries exceeded")
            user.pi_sync_status = "failed"
            user.pi_sync_error = task.error_message
            db.commit()
            logger.error(f"SyncTask #{task.id} ({user.name}) failed after {MAX_RETRIES} retries")


def enqueue_sync_task(db, user_id: int):
    """注册时调用：创建 SyncTask 并立即触发一次同步"""
    from models import SyncTask
    now = datetime.utcnow()
    task = SyncTask(user_id=user_id, retry_count=0, next_retry_at=now, status="pending")
    db.add(task)
    db.commit()
    db.refresh(task)
    # 立即在后台线程中处理一次
    t = threading.Thread(target=_sync_once, args=(task.id,), daemon=True)
    t.start()
    return task


def _sync_once(task_id: int):
    """立即执行一次同步（fire-and-forget）"""
    import time as _time
    _time.sleep(0.5)  # 等主事务提交完成
    from models import SessionLocal, SyncTask
    db = SessionLocal()
    try:
        task = db.query(SyncTask).filter(SyncTask.id == task_id).first()
        if task and task.status in ("pending", "failed"):
            _process_sync_task(db, task)
    except Exception as e:
        logger.error(f"_sync_once error: {e}")
    finally:
        db.close()


def sync_all_unsynced(db) -> dict:
    """批量同步所有未同步用户"""
    from models import User

    unsynced = db.query(User).filter(User.pi_synced == False).all()
    if not unsynced:
        return {"success": True, "synced": 0, "failed": 0, "errors": [], "message": "No unsynced users"}

    if not check_pi_online():
        return {"success": False, "synced": 0, "failed": len(unsynced),
                "errors": ["Pi is offline"], "message": "Pi is offline"}

    PHOTOS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "photos")
    synced = 0
    failed = 0
    errors = []
    details = []

    for user in unsynced:
        if not user.photo_url:
            failed += 1
            errors.append(f"{user.name}: no photo URL")
            continue
        photo_path = os.path.join(PHOTOS_DIR, os.path.basename(user.photo_url))
        result = sync_user_photo(user, photo_path)
        if result.get("success"):
            user.pi_synced = True
            user.pi_sync_status = "synced"
            synced += 1
            details.append({"name": user.name, "face_score": result.get("face_score")})
        else:
            failed += 1
            errors.append(f"{user.name}: {result.get('error', 'sync failed')}")

    db.commit()
    return {"success": failed == 0, "synced": synced, "failed": failed,
            "errors": errors, "details": details, "message": f"Sync done: {synced} ok, {failed} failed"}
