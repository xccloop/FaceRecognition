"""
树莓派（Pi 端）同步模块

功能：
  1. 检测树莓派是否在线（SSH 连接测试）
  2. 通过 SCP 将用户照片发送到树莓派的特征目录
  3. 批量同步所有未同步用户

依赖：
  pip install paramiko scp

配置来源：
  config.json → pi_host, pi_ssh_port, pi_user, pi_password, pi_features_dir
"""
import os
import json
import socket
from typing import Optional

import paramiko
from scp import SCPClient, SCPException

from config import EXE_DIR  # DATA_DIR

# ── 加载配置 ──────────────────────────────────────────────────
def _load_config() -> dict:
    """加载运行时配置"""
    config_file = os.path.join(EXE_DIR, "config.json")
    defaults = {
        "pi_host": "192.168.1.100",
        "pi_ssh_port": 22,
        "pi_user": "pi",
        "pi_password": "",
        "pi_features_dir": "/home/pi/features/",
        "heartbeat_timeout": 15,
    }
    try:
        if os.path.isfile(config_file):
            with open(config_file, "r") as f:
                saved = json.load(f)
            return {**defaults, **saved}
    except Exception:
        pass
    return defaults


def _get_ssh_client() -> paramiko.SSHClient:
    """创建并配置 SSH 客户端"""
    config = _load_config()
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())

    connect_kwargs = {
        "hostname": config["pi_host"],
        "port": config["pi_ssh_port"],
        "username": config["pi_user"],
        "timeout": config.get("heartbeat_timeout", 15),
    }

    if config.get("pi_password"):
        connect_kwargs["password"] = config["pi_password"]

    client.connect(**connect_kwargs)
    return client, config


def check_pi_online() -> bool:
    """
    检测树莓派是否在线

    策略：
      1. 先尝试 TCP 端口连接（快速判断）
      2. 再尝试 SSH 连接（确认服务可用）

    Returns:
        bool: 树莓派是否在线
    """
    config = _load_config()
    host = config["pi_host"]
    port = config["pi_ssh_port"]
    timeout = config.get("heartbeat_timeout", 15) / 3  # 快速超时

    # 快速 TCP 检测
    try:
        sock = socket.create_connection((host, port), timeout=timeout)
        sock.close()
    except (socket.timeout, ConnectionRefusedError, OSError):
        return False

    # SSH 连接确认
    try:
        client, _ = _get_ssh_client()
        client.close()
        return True
    except Exception:
        return False


def sync_user_photo(user, photo_path: str) -> dict:
    """
    同步单个用户的照片到树莓派

    Args:
        user: SQLAlchemy User 对象（需要有 id, name, photo_url 属性）
        photo_path: 本地照片文件的绝对路径

    Returns:
        dict: {"success": bool, "error": str | None}
    """
    if not os.path.isfile(photo_path):
        return {"success": False, "error": f"照片文件不存在: {photo_path}"}

    try:
        client, config = _get_ssh_client()
    except Exception as e:
        return {"success": False, "error": f"SSH 连接失败: {e}"}

    try:
        # 确保 Pi 端目录存在
        features_dir = config["pi_features_dir"]
        client.exec_command(f"mkdir -p {features_dir}")

        # 确定目标文件名（使用 user_id + 原始扩展名或用 UUID 文件名）
        ext = os.path.splitext(photo_path)[1]
        remote_filename = f"user_{user.id}_{user.name}{ext}"
        remote_path = os.path.join(features_dir, remote_filename).replace("\\", "/")

        # SCP 传输
        with SCPClient(client.get_transport()) as scp:
            scp.put(photo_path, remote_path)

        # 发送用户元信息（JSON 文件，包含 id、name）
        meta_path = os.path.join(features_dir, f"user_{user.id}_meta.json").replace("\\", "/")
        import tempfile
        with tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False) as tmp:
            json.dump({
                "user_id": user.id,
                "name": user.name,
                "photo_file": remote_filename,
                "synced_at": __import__("datetime").datetime.utcnow().isoformat(),
            }, tmp, ensure_ascii=False)
            tmp_path = tmp.name

        try:
            with SCPClient(client.get_transport()) as scp:
                scp.put(tmp_path, meta_path)
        finally:
            os.unlink(tmp_path)

        print(f"[sync] 已同步: {user.name} → {remote_path}")
        return {"success": True, "remote_path": remote_path}

    except SCPException as e:
        return {"success": False, "error": f"SCP 传输失败: {e}"}
    except Exception as e:
        return {"success": False, "error": f"同步异常: {e}"}
    finally:
        try:
            client.close()
        except Exception:
            pass


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
        return {"success": True, "synced": 0, "failed": 0, "errors": [], "message": "没有需要同步的用户"}

    # 先检测 Pi 是否在线
    if not check_pi_online():
        return {"success": False, "synced": 0, "failed": len(unsynced), "errors": ["树莓派不在线"], "message": "树莓派不在线，请检查网络连接"}

    PHOTOS_DIR = _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "photos")
    synced = 0
    failed = 0
    errors = []

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
        else:
            failed += 1
            errors.append(f"{user.name}: {result.get('error')}")

    db.commit()

    return {
        "success": failed == 0,
        "synced": synced,
        "failed": failed,
        "errors": errors,
        "message": f"同步完成: {synced} 成功, {failed} 失败",
    }
