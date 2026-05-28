#!/usr/bin/env python3
"""轻量 MJPEG 服务器 — 从 /dev/shm/latest_frame.jpg 读取帧。

用法: python3 mjpeg_stream.py [--port 8080] [--fps 15]
"""

import argparse
import os
import socket
import time

BOUNDARY = b"--frame"
HEADER = (
    b"HTTP/1.1 200 OK\r\n"
    b"Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
    b"Cache-Control: no-cache, no-store\r\n"
    b"Pragma: no-cache\r\n"
    b"Connection: close\r\n"
    b"\r\n"
)
FRAME_PATH = "/dev/shm/latest_frame.jpg"


def cpu_temp():
    try:
        with open("/sys/class/thermal/thermal_zone0/temp") as f:
            return int(f.read().strip()) / 1000.0
    except Exception:
        return 0

def handle_client(conn, fps):
    """单客户端 MJPEG 流 — 简单循环发帧"""
    try:
        # 读 HTTP 请求
        conn.recv(4096)
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        # 发头
        conn.sendall(HEADER)

        interval = 1.0 / max(fps, 1)
        last_mtime = 0
        last_data = b""
        frame_count = 0
        last_log = time.time()
        sent_count = 0

        while True:
            try:
                mtime = os.path.getmtime(FRAME_PATH)
            except OSError:
                time.sleep(0.05)
                continue

            if mtime != last_mtime:
                try:
                    with open(FRAME_PATH, "rb") as f:
                        data = f.read()
                    if data and data != last_data:
                        conn.sendall(
                            BOUNDARY + b"\r\nContent-Type: image/jpeg\r\n\r\n"
                            + data + b"\r\n"
                        )
                        last_data = data
                        sent_count += 1
                    last_mtime = mtime
                except Exception:
                    pass

            frame_count += 1
            now = time.time()
            if now - last_log >= 10:
                fps_actual = sent_count / (now - last_log)
                print(f"[MJPEG] sent={sent_count} fps={fps_actual:.0f} cpu={cpu_temp():.0f}C")
                sent_count = 0
                last_log = now

            time.sleep(interval)
    except (BrokenPipeError, ConnectionResetError, OSError):
        pass
    finally:
        try:
            conn.close()
        except Exception:
            pass


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--port", type=int, default=8080)
    p.add_argument("--fps", type=int, default=15)
    args = p.parse_args()

    # 等待帧文件出现
    print(f"[MJPEG] 等待 {FRAME_PATH} ...")
    for _ in range(30):
        if os.path.exists(FRAME_PATH):
            break
        time.sleep(1)
    if not os.path.exists(FRAME_PATH):
        print("[MJPEG] 警告: 帧文件未出现，继续尝试...")

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.settimeout(1.0)
    server.bind(("0.0.0.0", args.port))
    server.listen(5)

    print(f"[MJPEG] http://0.0.0.0:{args.port}/video  ({args.fps}fps)")

    while True:
        try:
            conn, addr = server.accept()
        except socket.timeout:
            continue
        except KeyboardInterrupt:
            break

        print(f"[MJPEG] 连接: {addr[0]}")
        # 每个客户端一个线程，简单可靠
        import threading
        t = threading.Thread(target=handle_client, args=(conn, args.fps), daemon=True)
        t.start()

    server.close()
    print("[MJPEG] 已停止")


if __name__ == "__main__":
    main()
