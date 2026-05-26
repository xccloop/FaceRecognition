"""人脸特征库 — 二进制文件读写、内存缓存、热加载。

二进制格式（与 verify.py 兼容）：
  [4 字节] 特征维度 N (int32, little-endian)
  [N×4 字节] 特征值 (float32[N], little-endian)

每个注册人员对应一个 .bin 文件：features/<name>.bin

功能：
  - 加载全部特征到内存 dict
  - 匹配（余弦相似度）
  - 添加/删除人员
  - 可选 inotify 热加载（监控 features/ 目录变化）
"""

from __future__ import annotations

import os
import struct
import threading
import time
from pathlib import Path
from typing import Dict, List, Optional

import numpy as np


class FeatureDB:
    """人脸特征数据库。

    用法:
        db = FeatureDB("features")
        db.load()
        name, sim = db.match(query_feature, threshold=0.4)
        db.add("张三", feature_array)
        db.remove("张三")
    """

    def __init__(self, features_dir: str, watch_changes: bool = True):
        """
        Args:
            features_dir: 特征文件目录路径。
            watch_changes: 是否启动后台线程监控目录变化（热加载）。
        """
        self._dir = Path(features_dir)
        self._lock = threading.RLock()
        self._registry: Dict[str, np.ndarray] = {}  # name → feature
        self._watch_enabled = watch_changes
        self._watch_thread: Optional[threading.Thread] = None
        self._watch_stop = threading.Event()

        # 记录上次加载的文件 mtime，用于检测变化
        self._file_mtimes: Dict[str, float] = {}

    # ── 持久化 ────────────────────────────────────────────

    def load(self) -> int:
        """从 features/ 目录加载所有 .bin 文件到内存。

        Returns:
            加载的特征数量。
        """
        os.makedirs(self._dir, exist_ok=True)

        registry: Dict[str, np.ndarray] = {}
        mtimes: Dict[str, float] = {}

        for fn in sorted(self._dir.glob("*.bin")):
            name = fn.stem
            try:
                feat, mtime = self._load_one(fn)
                registry[name] = feat
                mtimes[name] = mtime
            except Exception as e:
                print(f"[FeatureDB] 警告: 跳过损坏文件 {fn.name}: {e}")

        with self._lock:
            self._registry = registry
            self._file_mtimes = mtimes

        print(f"[FeatureDB] 已加载 {len(registry)} 个特征 ({self._dir})")
        for name in sorted(registry.keys()):
            print(f"  - {name}")

        return len(registry)

    @staticmethod
    def _load_one(filepath: Path) -> tuple:
        """读取单个 .bin 文件，返回 (feature_array, mtime)。"""
        stat = filepath.stat()
        with open(filepath, "rb") as f:
            dim = struct.unpack("<i", f.read(4))[0]
            if dim < 1 or dim > 2048:
                raise ValueError(f"无效维度: {dim}")
            raw = f.read(dim * 4)
            if len(raw) != dim * 4:
                raise ValueError(f"文件不完整: 期望 {dim*4}B, 实际 {len(raw)}B")
            feat = np.frombuffer(raw, dtype=np.float32).copy()
        return feat, stat.st_mtime

    def save_one(self, name: str, feature: np.ndarray) -> str:
        """保存单个特征到文件。

        Args:
            name: 人员姓名。
            feature: 特征向量 (N,) float32。

        Returns:
            保存的文件路径。
        """
        os.makedirs(self._dir, exist_ok=True)
        path = self._dir / f"{name}.bin"
        dim = len(feature)
        with open(path, "wb") as f:
            f.write(struct.pack("<i", dim))
            f.write(feature.astype(np.float32).tobytes())
        return str(path)

    # ── CRUD ──────────────────────────────────────────────

    def add(self, name: str, feature: np.ndarray) -> bool:
        """添加或更新一个人员特征。

        Args:
            name: 人员姓名。
            feature: 512 维特征向量。

        Returns:
            True 新增，False 覆盖已有。
        """
        path = self.save_one(name, feature)
        is_new = name not in self._registry
        with self._lock:
            self._registry[name] = feature.copy()
            self._file_mtimes[name] = os.path.getmtime(path)
        action = "新增" if is_new else "更新"
        print(f"[FeatureDB] {action}: {name} ({len(feature)} 维)")
        return is_new

    def remove(self, name: str) -> bool:
        """删除一个人员。

        Args:
            name: 人员姓名。

        Returns:
            True 删除成功，False 不存在。
        """
        path = self._dir / f"{name}.bin"
        if path.exists():
            path.unlink()

        with self._lock:
            existed = name in self._registry
            self._registry.pop(name, None)
            self._file_mtimes.pop(name, None)

        if existed:
            print(f"[FeatureDB] 删除: {name}")
        return existed

    def get(self, name: str) -> Optional[np.ndarray]:
        """获取某人特征。"""
        with self._lock:
            return self._registry.get(name)

    def list_names(self) -> List[str]:
        """列出所有注册人员。"""
        with self._lock:
            return sorted(self._registry.keys())

    def count(self) -> int:
        with self._lock:
            return len(self._registry)

    # ── 匹配 ──────────────────────────────────────────────

    def match(self, feature: np.ndarray, threshold: float = 0.4) -> tuple:
        """在注册库中查找最匹配的人员。

        Args:
            feature: 查询特征（已 L2 归一化）。
            threshold: 相似度阈值。

        Returns:
            (name, similarity)。若无匹配返回 ("?", best_sim)。
        """
        with self._lock:
            if not self._registry:
                return ("?", 0.0)

            best_sim = -2.0
            best_name = "?"
            # 批量点积（特征已归一化，点积即余弦相似度）
            for name, ref_feat in self._registry.items():
                sim = float(np.dot(feature, ref_feat))
                if sim > best_sim:
                    best_sim = sim
                    best_name = name

            if best_sim < threshold:
                return ("?", best_sim)
            return (best_name, best_sim)

    def match_all(self, feature: np.ndarray) -> List[tuple]:
        """返回与所有人员的相似度列表（按分数降序）。"""
        with self._lock:
            results = []
            for name, ref_feat in self._registry.items():
                sim = float(np.dot(feature, ref_feat))
                results.append((name, sim))
            results.sort(key=lambda x: -x[1])
            return results

    # ── 热加载（inotify 或轮询） ──────────────────────────

    def start_watching(self, interval: float = 2.0) -> None:
        """启动后台线程监控 features/ 目录变化。

        Args:
            interval: 轮询间隔（秒）。Linux 下可尝试用 inotify。
        """
        if not self._watch_enabled:
            return

        self._watch_stop.clear()
        self._watch_thread = threading.Thread(
            target=self._watch_loop,
            args=(interval,),
            daemon=True,
            name="FeatureDB-Watcher",
        )
        self._watch_thread.start()
        print(f"[FeatureDB] 热加载监控已启动 (间隔={interval}s)")

    def stop_watching(self) -> None:
        """停止热加载监控线程。"""
        self._watch_stop.set()
        if self._watch_thread and self._watch_thread.is_alive():
            self._watch_thread.join(timeout=3.0)

    def _watch_loop(self, interval: float) -> None:
        """轮询监控循环（Linux 下优先 inotify，回退轮询）。"""
        # 尝试用 inotify
        try:
            self._watch_inotify()
            return
        except (ImportError, OSError):
            pass

        # 回退：轮询
        print("[FeatureDB] 使用轮询模式监控特征文件变化")
        while not self._watch_stop.is_set():
            self._poll_changes()
            self._watch_stop.wait(interval)

    def _watch_inotify(self) -> None:
        """Linux inotify 模式（可选，需 pip install inotify）。"""
        try:
            import inotify.adapters
        except ImportError:
            raise ImportError("inotify 未安装")

        i = inotify.adapters.Inotify()
        watch_dir = str(self._dir)
        os.makedirs(watch_dir, exist_ok=True)
        i.add_watch(watch_dir)

        print(f"[FeatureDB] 使用 inotify 监控: {watch_dir}")

        for event in i.event_gen(yield_nones=False):
            if self._watch_stop.is_set():
                break
            (_, type_names, path, filename) = event
            if filename.endswith(".bin"):
                affected = {"IN_CLOSE_WRITE", "IN_MOVED_TO", "IN_DELETE",
                            "IN_MOVED_FROM"}
                if set(type_names) & affected:
                    time.sleep(0.1)  # 防抖
                    self._reload_gently()

    def _poll_changes(self) -> None:
        """轮询检查文件变化并重新加载。"""
        current_files = {}
        changed = False

        for fn in self._dir.glob("*.bin"):
            name = fn.stem
            mtime = fn.stat().st_mtime
            current_files[name] = mtime
            if name not in self._file_mtimes or self._file_mtimes.get(name) != mtime:
                changed = True

        # 检查删除
        with self._lock:
            old_names = set(self._file_mtimes.keys())
        new_names = set(current_files.keys())
        if old_names != new_names:
            changed = True

        if changed:
            print("[FeatureDB] 检测到特征文件变化，重新加载...")
            self.load()

    def _reload_gently(self) -> None:
        """轻量重载：仅重载变化的文件。"""
        current_files = set(self._dir.glob("*.bin"))
        current_names = {f.stem for f in current_files}

        with self._lock:
            old_names = set(self._registry.keys())
            # 删除
            for name in old_names - current_names:
                self._registry.pop(name, None)
                self._file_mtimes.pop(name, None)
                print(f"[FeatureDB] 热加载-删除: {name}")
            # 新增/更新
            for fn in current_files:
                name = fn.stem
                mtime = fn.stat().st_mtime
                if name not in self._file_mtimes or self._file_mtimes[name] != mtime:
                    try:
                        feat, _ = self._load_one(fn)
                        self._registry[name] = feat
                        self._file_mtimes[name] = mtime
                        print(f"[FeatureDB] 热加载-更新: {name}")
                    except Exception as e:
                        print(f"[FeatureDB] 热加载-跳过损坏文件 {fn.name}: {e}")

    # ── 统计 ──────────────────────────────────────────────

    def stats(self) -> dict:
        with self._lock:
            return {
                "count": len(self._registry),
                "names": list(self._registry.keys()),
                "dir": str(self._dir),
            }
