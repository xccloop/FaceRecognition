#!/usr/bin/env python3
"""树莓派人脸识别运行时 — 主入口。

四线程管线：
  1. 采集线程   — 从摄像头读取帧 → FrameQueue
  2. 推理线程   — 从 FrameQueue 取帧 → 检测/识别 → ResultQueue
  3. UART 控制   — 从 ResultQueue 取结果 → 发送开门指令给 STM32
  4. UART 监听   — 接收 STM32 反馈/状态

附加：
  - MJPEG 服务器（Flask） — 视频流供 Windows 端"实时画面"查看
  - FeatureDB 热加载      — 监控 features/ 目录，支持运行时添加人员

退出：Ctrl+C 或 SIGTERM → 优雅关闭所有线程。

用法:
    python main.py                          # 使用默认 config.json
    python main.py --config my_config.json  # 指定配置
    python main.py --list-ports             # 列出可用串口
"""

from __future__ import annotations

import os
import sys
import time
import signal
import threading
import argparse
from pathlib import Path

from src.config import AppConfig, load_config
from src.camera import FrameQueue, ResultQueue, capture_loop
from src.recognition import init_engine, get_engine
from src.feature_db import FeatureDB
from src.inference import inference_loop
from src.uart import (
    UARTClient, uart_control_loop, uart_receive_monitor
)
from src.mjpeg import MJPEGServer


# ══════════════════════════════════════════════════════════
# Banner
# ══════════════════════════════════════════════════════════

BANNER = r"""
   ╔══════════════════════════════════════════╗
   ║     四端协同人脸识别 — 树莓派端      ║
   ║     Face Recognition Pi Runtime     ║
   ╚══════════════════════════════════════════╝
"""


# ══════════════════════════════════════════════════════════
# main
# ══════════════════════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(
        description="树莓派人脸识别运行时"
    )
    parser.add_argument(
        "--config", "-c", default="config.json",
        help="配置文件路径 (默认: config.json)"
    )
    parser.add_argument(
        "--list-ports", action="store_true",
        help="列出可用串口后退出"
    )
    parser.add_argument(
        "--no-uart", action="store_true",
        help="禁用 UART（仅推理 + 视频流，调试用）"
    )
    parser.add_argument(
        "--debug", action="store_true",
        help="启用推理调试输出"
    )
    args = parser.parse_args()

    # ── 列出串口 ──
    if args.list_ports:
        ports = UARTClient.list_ports()
        if ports:
            print("\n可用串口:")
            for p in ports:
                print(f"  {p['device']:<20} {p['description']}")
        else:
            print("未检测到串口设备")
        return

    # ── 加载配置 ──
    print(BANNER)

    config_path = os.path.abspath(args.config)
    if not os.path.exists(config_path):
        print(f"错误: 配置文件不存在: {config_path}")
        print("请复制 config.json.example → config.json 并修改")
        sys.exit(1)

    config = load_config(config_path)
    print(f"配置: {config_path}")
    print(f"  摄像头: device={config.camera.device} "
          f"{config.camera.width}x{config.camera.height} "
          f"@{config.camera.fps}fps ({config.camera.backend})")
    print(f"  模型:   {config.recognition.model_dir}")
    print(f"  特征库: {config.features.features_dir}")
    print(f"  串口:   {config.uart.device} @{config.uart.baudrate}bps")
    print(f"  MJPEG:  http://0.0.0.0:{config.mjpeg.port}/video")

    # ── 初始化引擎 ──
    print("\n[Init] 加载识别模型...")
    try:
        engine = init_engine(
            model_dir=config.recognition.model_dir,
            det_model=config.recognition.det_model,
            rec_model=config.recognition.rec_model,
        )
        print("[Init] ✓ 模型就绪")
    except FileNotFoundError as e:
        print(f"\n[Init] ✗ 模型文件缺失: {e}")
        print("  请参考 doc/ 下的使用说明下载模型文件")
        sys.exit(1)
    except Exception as e:
        print(f"\n[Init] ✗ 模型加载失败: {e}")
        sys.exit(1)

    # ── 初始化特征库 ──
    print("\n[Init] 加载特征库...")
    feats_dir = config.features.features_dir
    # 优先使用绝对路径，其次相对于 config.json 所在目录
    if not os.path.isabs(feats_dir):
        feats_dir = os.path.join(os.path.dirname(config_path), feats_dir)
    print(f"  目录: {feats_dir}")
    os.makedirs(feats_dir, exist_ok=True)

    feature_db = FeatureDB(feats_dir, watch_changes=True)
    count = feature_db.load()
    if count == 0:
        print("  ⚠ 特征库为空 — 请通过 Windows 端同步人员特征")

    # ── 队列 ──
    frame_q = FrameQueue()
    result_q = ResultQueue(maxsize=config.runtime.result_queue_size)

    # ── 停止信号 ──
    stop_event = threading.Event()

    # ── 线程列表 ──
    threads: list[threading.Thread] = []

    # ── MJPEG 服务器 ──
    mjpeg_server = MJPEGServer(
        frame_q=frame_q,
        port=config.mjpeg.port,
        bind=config.mjpeg.bind,
        framerate=config.mjpeg.framerate,
        quality=config.mjpeg.quality,
    )
    mjpeg_server.start()

    # ── 启动采集线程 ──
    t_capture = threading.Thread(
        target=capture_loop,
        args=(frame_q, config.camera, stop_event, config.camera.fps),
        daemon=True,
        name="Capture",
    )
    t_capture.start()
    threads.append(t_capture)
    time.sleep(0.3)  # 等摄像头初始化

    # ── 启动推理线程 ──
    t_infer = threading.Thread(
        target=inference_loop,
        args=(
            frame_q, result_q, engine, feature_db, stop_event,
            config.recognition.det_threshold,
            config.recognition.rec_threshold,
            config.recognition.nms_threshold,
            config.recognition.max_side,
            config.runtime.skip_frames,
            config.runtime.confirm_frames,
            config.runtime.track_timeout,
            args.debug,
        ),
        daemon=True,
        name="Inference",
    )
    t_infer.start()
    threads.append(t_infer)

    # ── UART ──
    uart = UARTClient(config.uart)
    if not args.no_uart:
        if uart.open():
            # 启动 UART 控制线程
            t_uart_ctrl = threading.Thread(
                target=uart_control_loop,
                args=(result_q, uart, stop_event, config.uart.heartbeat_interval),
                daemon=True,
                name="UART-Ctrl",
            )
            t_uart_ctrl.start()
            threads.append(t_uart_ctrl)

            # 启动 UART 监听线程
            t_uart_mon = threading.Thread(
                target=uart_receive_monitor,
                args=(uart, stop_event),
                daemon=True,
                name="UART-Mon",
            )
            t_uart_mon.start()
            threads.append(t_uart_mon)
        else:
            print("[UART] 串口不可用，UART 控制功能已禁用")
    else:
        print("[UART] --no-uart，串口功能已手动禁用")

    # ── 启动特征库热加载 ──
    feature_db.start_watching(interval=2.0)

    # ── 状态报告 ──
    print(f"\n{'═'*50}")
    print(f"  所有线程已启动 [{len(threads)} 个工作线程]")
    print(f"  MJPEG 流: http://{config.mjpeg.bind}:{config.mjpeg.port}/video")
    print(f"  按 Ctrl+C 退出")
    print(f"{'═'*50}\n")

    # ── 信号处理 ──
    def shutdown(signum=None, frame=None):
        print("\n[Main] 收到退出信号，正在关闭...")
        stop_event.set()

    signal.signal(signal.SIGINT, shutdown)
    signal.signal(signal.SIGTERM, shutdown)

    # ── 主循环：等待退出 ──
    try:
        while not stop_event.is_set():
            time.sleep(1.0)
    except KeyboardInterrupt:
        pass
    finally:
        shutdown()

    # ── 优雅关闭 ──
    print("[Main] 等待线程退出...")
    for t in threads:
        if t.is_alive():
            t.join(timeout=5.0)
    feature_db.stop_watching()
    mjpeg_server.stop()
    uart.close()
    print("[Main] 已退出\n")


if __name__ == "__main__":
    main()
