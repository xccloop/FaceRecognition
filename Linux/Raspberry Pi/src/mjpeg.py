"""MJPEG 视频流服务 — 供 Windows 管理端"实时画面"查看。

使用 Flask 在独立线程运行，从 FrameQueue 取 JPEG 帧推给 HTTP 客户端。

端点：
  - GET /video         MJPEG 流（multipart/x-mixed-replace）
  - GET /snapshot      单帧 JPEG 快照
  - GET /health        健康检查

用法:
    server = MJPEGServer(frame_q, port=8080)
    server.start()
    # ... 程序运行 ...
    server.stop()
"""

from __future__ import annotations

import threading
import time
from typing import Optional
from wsgiref.simple_server import make_server

from flask import Flask, Response, jsonify


class MJPEGServer:
    """轻量 MJPEG 流服务器（基于 Flask + wsgiref）。

    专为树莓派优化：
    - 单线程 Flask（避免多线程竞争 FrameQueue）
    - 无阻塞地从 FrameQueue 取帧
    - 客户端断线自动清理
    """

    def __init__(self, frame_q, port: int = 8080,
                 bind: str = "0.0.0.0",
                 framerate: int = 15,
                 quality: int = 50):
        self._frame_q = frame_q
        self._port = port
        self._bind = bind
        self._framerate = framerate
        self._quality = quality

        self._app = Flask(f"mjpeg-{port}")
        self._app.debug = False

        import logging
        log = logging.getLogger("werkzeug")
        log.setLevel(logging.WARNING)

        self._register_routes()

        self._server = None
        self._thread: Optional[threading.Thread] = None
        self._running = False
        self._client_count = 0
        self._client_lock = threading.Lock()

    def _register_routes(self) -> None:
        app = self._app

        @app.route("/video")
        def video_stream():
            """MJPEG 流端点。"""
            return Response(
                self._generate_frames(),
                mimetype="multipart/x-mixed-replace; boundary=frame",
            )

        @app.route("/snapshot")
        def snapshot():
            """单帧 JPEG 快照。"""
            frame_data = self._frame_q.peek()
            if frame_data is None:
                return Response("no frame available", status=503)
            _, _, jpeg = frame_data
            if jpeg is None:
                return Response("no frame available", status=503)
            return Response(jpeg, mimetype="image/jpeg")

        @app.route("/health")
        def health():
            frame_data = self._frame_q.peek()
            has_frame = frame_data is not None
            with self._client_lock:
                clients = self._client_count
            return jsonify({
                "status": "ok" if self._running else "stopped",
                "has_frame": has_frame,
                "clients": clients,
                "port": self._port,
            })

    def _generate_frames(self):
        """生成 MJPEG 帧的生成器。"""
        with self._client_lock:
            self._client_count += 1
        frame_interval = 1.0 / max(self._framerate, 1)

        try:
            while self._running:
                frame_data = self._frame_q.peek()
                if frame_data is None:
                    time.sleep(0.05)
                    continue

                _, _, jpeg = frame_data
                if jpeg is not None:
                    yield (b"--frame\r\n"
                           b"Content-Type: image/jpeg\r\n\r\n" +
                           jpeg + b"\r\n")

                time.sleep(frame_interval)
        except GeneratorExit:
            pass
        finally:
            with self._client_lock:
                self._client_count -= 1

    def start(self) -> bool:
        """启动 MJPEG 服务器（后台线程）。

        Returns:
            True 成功。
        """
        self._running = True
        self._thread = threading.Thread(
            target=self._serve,
            daemon=True,
            name=f"MJPEG-{self._port}",
        )
        self._thread.start()

        # 等待服务器就绪
        time.sleep(0.5)
        print(f"[MJPEG] 服务已启动: http://{self._bind}:{self._port}/video")
        return True

    def _serve(self) -> None:
        """Flask WSGI 服务循环。"""
        try:
            self._server = make_server(
                self._bind, self._port, self._app,
            )
            print(f"[MJPEG] 监听 {self._bind}:{self._port}")
            self._server.serve_forever(poll_interval=0.1)
        except Exception as e:
            print(f"[MJPEG] 服务异常: {e}")
            self._running = False

    def stop(self) -> None:
        """停止 MJPEG 服务器。"""
        self._running = False
        if self._server:
            try:
                self._server.shutdown()
            except Exception:
                pass
            self._server = None
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=3.0)
        print("[MJPEG] 服务已停止")

    @property
    def is_running(self) -> bool:
        return self._running
