"""加载与校验树莓派端运行时配置。

支持：
- JSON 文件加载
- 环境变量覆盖（PI_CAMERA_DEVICE, PI_UART_DEVICE, PI_MJPEG_PORT 等）
- 路径自动转为绝对路径（相对于 config.json 所在目录）
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional


# ─── 配置结构体 ──────────────────────────────────────────

@dataclass
class CameraConfig:
    device: int = 0
    width: int = 640
    height: int = 480
    fps: int = 15
    backend: str = "opencv"


@dataclass
class RecognitionConfig:
    model_dir: str = "models/onnx_models/buffalo_s"
    det_model: str = "det_500m.onnx"
    rec_model: str = "w600k_mbf.onnx"
    det_threshold: float = 0.5
    rec_threshold: float = 0.4
    nms_threshold: float = 0.4
    max_side: int = 480


@dataclass
class FeaturesConfig:
    features_dir: str = "features"
    watch_changes: bool = True


@dataclass
class RuntimeConfig:
    result_queue_size: int = 16
    skip_frames: int = 3
    confirm_frames: int = 3
    track_timeout: float = 2.0


@dataclass
class UARTConfig:
    enabled: bool = True
    device: str = "/dev/serial0"
    baudrate: int = 115200
    timeout: float = 1.0
    heartbeat_interval: int = 5
    status_pin: Optional[int] = None
    commands: dict = field(default_factory=lambda: {
        "open": "OPEN",
        "close": "CLOSE",
        "reject": "REJECT",
    })


@dataclass
class MJPEGConfig:
    enabled: bool = True
    bind: str = "0.0.0.0"
    port: int = 8080
    framerate: int = 15
    quality: int = 60


@dataclass
class APIConfig:
    enabled: bool = True
    host: str = "0.0.0.0"
    port: int = 5000


@dataclass
class AppConfig:
    camera: CameraConfig = field(default_factory=CameraConfig)
    recognition: RecognitionConfig = field(default_factory=RecognitionConfig)
    features: FeaturesConfig = field(default_factory=FeaturesConfig)
    runtime: RuntimeConfig = field(default_factory=RuntimeConfig)
    uart: UARTConfig = field(default_factory=UARTConfig)
    mjpeg: MJPEGConfig = field(default_factory=MJPEGConfig)
    api: APIConfig = field(default_factory=APIConfig)
    project_root: str = ""


# ─── 环境变量覆盖映射 ───────────────────────────────────

_ENV_OVERRIDES = {
    ("PI_CAMERA_DEVICE", "camera.device"),
    ("PI_CAMERA_WIDTH", "camera.width"),
    ("PI_CAMERA_HEIGHT", "camera.height"),
    ("PI_CAMERA_FPS", "camera.fps"),
    ("PI_CAMERA_BACKEND", "camera.backend"),
    ("PI_MODEL_DIR", "recognition.model_dir"),
    ("PI_DET_THRESHOLD", "recognition.det_threshold"),
    ("PI_REC_THRESHOLD", "recognition.rec_threshold"),
    ("PI_SKIP_FRAMES", "runtime.skip_frames"),
    ("PI_FEATURES_DIR", "features.features_dir"),
    ("PI_UART_DEVICE", "uart.device"),
    ("PI_UART_BAUDRATE", "uart.baudrate"),
    ("PI_MJPEG_PORT", "mjpeg.port"),
    ("PI_MJPEG_QUALITY", "mjpeg.quality"),
    ("PI_API_PORT", "api.port"),
}


def _set_nested(obj: object, path: str, value: str) -> None:
    """按 'section.field' 路径设置嵌套 dataclass 字段，自动转换类型。"""
    parts = path.split(".")
    for part in parts[:-1]:
        obj = getattr(obj, part)
    field_name = parts[-1]
    current = getattr(obj, field_name)
    if isinstance(current, bool):
        val = value.lower() in ("1", "true", "yes")
    elif isinstance(current, int):
        val = int(value)
    elif isinstance(current, float):
        val = float(value)
    else:
        val = value
    setattr(obj, field_name, val)


def _filter_kwargs(raw_dict: dict) -> dict:
    """过滤掉 _ 开头的注释字段，避免传入 dataclass 构造函数。"""
    return {k: v for k, v in raw_dict.items() if not k.startswith("_")}


def load_config(config_path: Optional[str] = None) -> AppConfig:
    """加载配置文件，返回 AppConfig 实例。"""
    if config_path is None:
        candidates = [
            Path.cwd() / "config.json",
            Path(__file__).resolve().parent.parent / "config.json",
        ]
        for c in candidates:
            if c.exists():
                config_path = str(c)
                break
        else:
            raise FileNotFoundError(
                f"未找到 config.json。搜索路径: {[str(c) for c in candidates]}"
            )

    config_file = Path(config_path)
    project_root = config_file.parent.resolve()

    with open(config_file, "r", encoding="utf-8") as f:
        raw = json.load(f)

    cfg = AppConfig(
        camera=CameraConfig(**_filter_kwargs(raw.get("camera", {}))),
        recognition=RecognitionConfig(**_filter_kwargs(raw.get("recognition", {}))),
        features=FeaturesConfig(**_filter_kwargs(raw.get("features", {}))),
        runtime=RuntimeConfig(**_filter_kwargs(raw.get("runtime", {}))),
        uart=UARTConfig(**_filter_kwargs(raw.get("uart", {}))),
        mjpeg=MJPEGConfig(**_filter_kwargs(raw.get("mjpeg", {}))),
        api=APIConfig(**_filter_kwargs(raw.get("api", {}))),
        project_root=str(project_root),
    )

    for env_var, config_path_str in _ENV_OVERRIDES:
        value = os.environ.get(env_var)
        if value is not None:
            _set_nested(cfg, config_path_str, value)

    _normalize_paths(cfg, project_root)

    return cfg


def _normalize_paths(cfg: AppConfig, project_root: Path) -> None:
    """将相对路径转为绝对路径。"""
    cfg.recognition.model_dir = str(project_root / cfg.recognition.model_dir)
    cfg.features.features_dir = str(project_root / cfg.features.features_dir)


def config_summary(cfg: AppConfig) -> str:
    """生成配置摘要（启动时打印）。"""
    lines = [
        "=" * 48,
        "  树莓派人脸识别 — 运行时配置",
        "=" * 48,
        f"  摄像头:  {cfg.camera.backend} device={cfg.camera.device} "
        f"{cfg.camera.width}x{cfg.camera.height} @{cfg.camera.fps}fps",
        f"  模型:    {cfg.recognition.model_dir}",
        f"           检测阈值={cfg.recognition.det_threshold}  "
        f"识别阈值={cfg.recognition.rec_threshold}  "
        f"跳帧={cfg.runtime.skip_frames}",
        f"  特征库:  {cfg.features.features_dir} "
        f"(热加载={'开' if cfg.features.watch_changes else '关'})",
    ]
    if cfg.uart.enabled:
        lines.append(
            f"  串口:    {cfg.uart.device} @{cfg.uart.baudrate}bps "
            f"心跳={cfg.uart.heartbeat_interval}s"
        )
    else:
        lines.append("  串口:    已禁用")
    if cfg.mjpeg.enabled:
        lines.append(
            f"  视频流:  http://{cfg.mjpeg.bind}:{cfg.mjpeg.port}/video "
            f"质量={cfg.mjpeg.quality}"
        )
    else:
        lines.append("  视频流:  已禁用")
    if cfg.api.enabled:
        lines.append(f"  API:     http://{cfg.api.host}:{cfg.api.port}")
    else:
        lines.append("  API:     已禁用")
    lines.append("=" * 48)
    return "\n".join(lines)
