#!/bin/bash
# FaceRecognition 一键启动（推理 + UART + MJPEG 全部内置）
# 用法: ./User/start.sh

set -e

FACEREC="$(cd "$(dirname "$0")/.." && pwd)"
BINARY="$FACEREC/Model/build/facerec"
UART_DEV=/dev/serial0

echo "========================================"
echo "  FaceRecognition 启动"
echo "========================================"

# ── Check UART ──
if [ ! -e "$UART_DEV" ]; then
    echo "[ERROR] $UART_DEV 不存在！确保 config.txt 有 enable_uart=1 + dtoverlay=disable-bt"
    exit 1
fi
echo "[OK] UART: $UART_DEV"

# ── Kill old ──
echo "[Clean] 停止旧进程..."
pkill -f "facerec run" 2>/dev/null || true
pkill -f mjpeg_stream.py 2>/dev/null || true
sleep 0.5

# ── Start (推理 + UART + MJPEG@8080 全部内置) ──
echo "[Start] ncnn 推理引擎 (MJPEG:8080)..."
cd "$FACEREC/Model"
exec "$BINARY" run
