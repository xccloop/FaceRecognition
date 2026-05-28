#!/bin/bash
# MJPEG 视频流 — 摄像头画面回传 Windows（前台运行，Ctrl+C 停止）
# 用法: ./mjpeg_start.sh

FACEREC="$(cd "$(dirname "$0")/.." && pwd)"
PYTHON=/home/qxc/miniforge3/envs/facerec/bin/python3
PORT=8080
FPS=25

IP=$(hostname -I | awk '{print $1}')

pkill -f mjpeg_stream.py 2>/dev/null || true
sleep 0.5

echo "========================================"
echo "  MJPEG 视频流"
echo "  http://${IP}:${PORT}/video  (${FPS}fps)"
echo "  Ctrl+C 停止"
echo "========================================"

cd "$FACEREC/Model"
exec "$PYTHON" mjpeg_stream.py --port "$PORT" --fps "$FPS"
