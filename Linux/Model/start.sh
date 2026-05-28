#!/bin/bash
# FaceRecognition 启动脚本
# 用法: ./start.sh [--no-uart] [--no-mjpeg]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BINARY="$SCRIPT_DIR/build/facerec"
PYTHON=/home/qxc/miniforge3/envs/facerec/bin/python3
UART_DEV=/dev/serial0

echo "========================================"
echo "  FaceRecognition 启动"
echo "========================================"

# ── 1. Check UART ──
if [[ "$*" != *"--no-uart"* ]]; then
    if [ -e "$UART_DEV" ]; then
        echo "[OK] UART: $UART_DEV"
    else
        echo "[WARN] $UART_DEV 不存在！确保 config.txt 有 enable_uart=1 + dtoverlay=disable-bt"
    fi
else
    echo "[SKIP] UART"
fi

# ── 2. Check binary ──
if [ ! -f "$BINARY" ]; then
    echo "[ERROR] $BINARY 不存在，请先编译"
    exit 1
fi

# ── 3. Kill old ──
pkill -f "facerec run" 2>/dev/null || true
pkill -f "mjpeg_stream.py" 2>/dev/null || true
sleep 1

# ── 4. MJPEG (Python, background) ──
if [[ "$*" != *"--no-mjpeg"* ]]; then
    echo "[Start] MJPEG http://$(hostname -I | awk '{print $1}'):8080/video"
    cd "$SCRIPT_DIR"
    $PYTHON mjpeg_stream.py --width 320 --height 240 &
    MJPEG_PID=$!
    sleep 1
fi

# ── 5. FaceRecognition (C++ ncnn) ──
echo "[Start] FaceRecognition (ncnn)"
cd "$SCRIPT_DIR"
"$BINARY" run &
FACEREC_PID=$!

echo ""
echo "========================================"
echo "  MJPEG: http://$(hostname -I | awk '{print $1}'):8080/video"
echo "  PID:   facerec=$FACEREC_PID  mjpeg=$MJPEG_PID"
echo "  停止:  kill $FACEREC_PID $MJPEG_PID"
echo "========================================"

wait $FACEREC_PID 2>/dev/null
kill $MJPEG_PID 2>/dev/null || true
