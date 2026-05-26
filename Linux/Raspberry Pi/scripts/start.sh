#!/bin/bash
# ═══════════════════════════════════════════════════════════════
# 树莓派人脸识别 — 快速启动脚本
#
# 用于手动启动识别程序（调试/前台运行）。
# systemd 用户请使用 sudo systemctl start face-recog。
#
# 用法:
#   chmod +x scripts/start.sh
#   ./scripts/start.sh              # 前台运行
#   ./scripts/start.sh --debug      # 调试模式
#   ./scripts/start.sh --help       # 更多选项
# ═══════════════════════════════════════════════════════════════

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

VENV_DIR="$PROJECT_DIR/venv"
PYTHON="$VENV_DIR/bin/python"

# ── 检查虚拟环境 ──
if [ ! -f "$PYTHON" ]; then
    echo "错误: 虚拟环境不存在，请先运行 scripts/install.sh"
    echo "  期望: $VENV_DIR"
    exit 1
fi

# ── 检查配置文件 ──
if [ ! -f "$PROJECT_DIR/config.json" ]; then
    echo "错误: config.json 不存在"
    echo "  参考: config.json.example"
    exit 1
fi

# ── 启动 ──
cd "$PROJECT_DIR"
echo "启动人脸识别程序..."
echo "  项目目录: $PROJECT_DIR"
echo "  日志:     tail -f /var/log/face-recog.log"
echo ""

exec "$PYTHON" main.py "$@"
