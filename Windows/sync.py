"""
树莓派（Pi 端）同步模块  — HTTP API 版本

功能：
  1. 检测树莓派 HTTP API 是否在线
  2. 通过 HTTP POST 将用户照片+姓名发送到 Pi 端 /api/register
  3. Pi 端自动完成人脸检测→对齐→特征提取→保存 .bin
  4. 批量同步所有未同步用户

与 SSH/SCP 方案的区别：
  - 无需 SSH 密码/密钥，只需 HTTP 可达
  - Pi 端直接返回注册结果（脸部分数、特征维度）
  - 结构化错误反馈（未检测到人脸、图片格式错误等）

配置来源：
  config.json → pi_host, pi_api_port, heartbeat_timeout
"""

import os
import json
from typing import Optional

import requests

from config import EXE_DIR  # DATA_DIR


def _load_config() -> dict:
    """加载运行时配置"""
    config_file = os.path.join(EXE_DIR, "config.json")
    defaults = {
        "pi_host": "192.168.1.100",
        "pi_api_port": 5000,
        "heartbeat_timeout": 5,
    }
    try:
        if os.path.isfile(config_file):
            with open(config_file, "r") as f:
                saved = json.load(f)
            # 兼容旧配置键名
            if "pi_ssh_port" in saved:
                saved.setdefault("pi_api_port", 5000)
            merged = {**defaults, **saved}
            return merged
    except Exception:
        pass
    return dict(defaults)


def _get_pi_url(endpoint: str = "") -> str:
    """构建 Pi 端 API URL"""
    config = _load_config()
    base = f"http://{config['pi_host']}:{config['pi_api_port']}"
    return f"{base}{endpoint}" if endpoint else base


def check_pi_online() -> bool:
    """
    通过 HTTP GET /api/health 检测树莓派是否在线

    Returns:
        bool: 树莓派 HTTP API 是否在线
    """
    config = _load_config()
    timeout = max(2, config.get("heartbeat_timeout", 5))

    try:
        resp = requests.get(
            _get_pi_url("/api/health"),
            timeout=timeout,
        )
        if resp.status_code == 200:
            data = resp.json()
            print(f"[sync] Pi 在线，已注册 {data.get('registered_users', 0)} 人: {data.get('users', [])}")
            return True
        return False
    except requests.ConnectionError:
        print(f"[sync] Pi 连接失败: {config['pi_host']}:{config['pi_api_port']}")
        return False
    except requests.Timeout:
        print(f"[sync] Pi 连接超时 ({timeout}s)")
        return False
    except Exception as e:
        print(f"[sync] Pi 检测异常: {e}")
        return False


def sync_user_photo(user, photo_path: str) -> dict:
    """
    同步单个用户到树莓派（HTTP POST 照片 → Pi 端自动注册）

    Args:
        user: SQLAlchemy User 对象（需有 id, name 属性）
        photo_path: 本地照片文件的绝对路径

    Returns:
        dict: {
            "success": bool,
            "error": str | None,
            "face_score": float | None,
            "feature_dim": int | None,
            "remote_message": str | None,
        }
    """
    if not os.path.isfile(photo_path):
        return {"success": False, "error": f"照片文件不存在: {photo_path}"}

    config = _load_config()
    timeout = max(5, config.get("heartbeat_timeout", 15))

    try:
        with open(photo_path, "rb") as f:
            files = {"photo": (os.path.basename(photo_path), f, "image/jpeg")}
            data = {"name": user.name}

            resp = requests.post(
                _get_pi_url("/api/register"),
                files=files,
                data=data,
                timeout=timeout,
            )

        result = resp.json()

        if resp.status_code == 200 and result.get("success"):
            print(
                f"[sync] 已注册: {user.name} "
                f"→ face_score={result.get('face_score', '?')} "
                f"dim={result.get('feature_dim', '?')}"
            )
            return {
                "success": True,
                "face_score": result.get("face_score"),
                "feature_dim": result.get("feature_dim"),
            }
        else:
            error_msg = result.get("error", f"HTTP {resp.status_code}")
            print(f"[sync] 注册失败: {user.name} → {error_msg}")
            return {
                "success": False,
                "error": error_msg,
                "face_score": result.get("face_score"),
            }

    except requests.ConnectionError as e:
        return {"success": False, "error": f"连接 Pi 失败: {e}"}
    except requests.Timeout:
        return {"success": False, "error": f"Pi 响应超时 ({timeout}s)"}
    except Exception as e:
        return {"success": False, "error": f"同步异常: {e}"}


def sync_all_unsynced(db) -> dict:
    """
    批量同步所有未同步用户

    Args:
        db: SQLAlchemy Session

    Returns:
        dict: {"success": bool, "synced": int, "failed": int, "errors": list}
    """
    from models import User
    import os as _os

    unsynced = db.query(User).filter(User.pi_synced == False).all()
    if not unsynced:
        return {
            "success": True,
            "synced": 0,
            "failed": 0,
            "errors": [],
            "message": "没有需要同步的用户",
        }

    # 先检测 Pi 是否在线
    if not check_pi_online():
        return {
            "success": False,
            "synced": 0,
            "failed": len(unsynced),
            "errors": ["树莓派不在线，请检查 Pi 端 HTTP API 服务是否已启动"],
            "message": "树莓派不在线",
        }

    PHOTOS_DIR = _os.path.join(
        _os.path.dirname(_os.path.abspath(__file__)), "photos"
    )
    synced = 0
    failed = 0
    errors = []
    details = []

    for user in unsynced:
        if not user.photo_url:
            failed += 1
            errors.append(f"{user.name}: 无照片 URL")
            continue

        photo_path = _os.path.join(PHOTOS_DIR, _os.path.basename(user.photo_url))
        result = sync_user_photo(user, photo_path)

        if result.get("success"):
            user.pi_synced = True
            synced += 1
            details.append({
                "name": user.name,
                "face_score": result.get("face_score"),
            })
        else:
            failed += 1
            errors.append(f"{user.name}: {result.get('error', '同步失败')}")

    db.commit()

    return {
        "success": failed == 0,
        "synced": synced,
        "failed": failed,
        "errors": errors,
        "details": details,
        "message": f"同步完成: {synced} 成功, {failed} 失败",
    }
