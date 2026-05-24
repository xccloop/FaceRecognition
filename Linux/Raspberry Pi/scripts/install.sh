#!/bin/bash
# 树莓派人脸识别 一键安装脚本 (C++/ncnn 版)
# 用法: chmod +x install.sh && ./install.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
MODEL_DIR="$PROJECT_DIR/../Model/models/ncnn_models"
BUILD_DIR="$PROJECT_DIR/../Model/build"

echo ""
echo "=============================================="
echo "  树莓派人脸识别 安装脚本 (C++/ncnn)"
echo "  项目目录: $PROJECT_DIR"
echo "=============================================="
echo ""

# ── 步骤1: 安装系统依赖 ──
echo "[1/6] 安装系统依赖..."
sudo apt-get update -qq
sudo apt-get install -y -qq \
    build-essential cmake git \
    libopencv-dev libatlas-base-dev \
    python3 python3-pip \
    lsof net-tools \
    || { echo "错误: apt 安装失败"; exit 1; }
echo "[1/6] ✓ 系统依赖完成"

# ── 步骤2: 编译 ncnn ──
echo ""
echo "[2/6] 编译 ncnn (ARM NEON)..."
NCNN_DIR="$HOME/ncnn"
if [ -f /usr/local/lib/libncnn.a ]; then
    echo "ncnn 已安装，跳过编译"
else
    if [ ! -d "$NCNN_DIR" ]; then
        git clone https://github.com/Tencent/ncnn.git "$NCNN_DIR"
        cd "$NCNN_DIR" && git submodule update --init
    fi
    cd "$NCNN_DIR"
    mkdir -p build && cd build
    cmake -DCMAKE_BUILD_TYPE=Release \
          -DNCNN_VULKAN=OFF \
          -DNCNN_BUILD_EXAMPLES=OFF \
          -DNCNN_BUILD_TOOLS=OFF ..
    make -j$(nproc)
    sudo make install
    sudo ldconfig
fi
echo "[2/6] ✓ ncnn 编译完成"

# ── 步骤3: 编译 facerec ──
echo ""
echo "[3/6] 编译 facerec (C++ 识别引擎)..."
MODEL_SRC="$PROJECT_DIR/../Model"
if [ -f "$MODEL_SRC/CMakeLists.txt" ]; then
    mkdir -p "$BUILD_DIR" && cd "$BUILD_DIR"
    cmake -DCMAKE_BUILD_TYPE=Release "$MODEL_SRC"
    make -j$(nproc)
    echo "[3/6] ✓ facerec 编译完成 → $BUILD_DIR/facerec"
else
    echo "[3/6] ⚠ 未找到 CMakeLists.txt，跳过编译"
    echo "    请将 Model/ 目录放到 $MODEL_SRC 后重新运行"
fi

# ── 步骤4: 检查模型文件 ──
echo ""
echo "[4/6] 检查 ncnn 模型文件..."
MODELS_OK=true
for f in det_500m_dyn.param det_500m_dyn.bin w600k_mbf.param w600k_mbf.bin; do
    if [ ! -f "$MODEL_DIR/$f" ]; then
        echo "  ✗ 缺失: $f"
        MODELS_OK=false
    else
        echo "  ✓ $f"
    fi
done

if [ "$MODELS_OK" = false ]; then
    echo ""
    echo "  模型文件缺失。请从 Windows 传输 ncnn 模型:"
    echo "  scp D:\\FaceRecognition\\Linux\\Model\\models\\ncnn_models\\*.param pi@<IP>:$MODEL_DIR/"
    echo "  scp D:\\FaceRecognition\\Linux\\Model\\models\\ncnn_models\\*.bin   pi@<IP>:$MODEL_DIR/"
fi
echo "[4/6] ✓ 模型检查完成"

# ── 步骤5: 安装 Python 注册 API 依赖 ──
echo ""
echo "[5/6] 安装注册 API 依赖..."
pip3 install flask numpy opencv-python 2>/dev/null || true
echo "[5/6] ✓ Python 依赖完成"

# ── 步骤6: 安装 systemd 服务 ──
echo ""
echo "[6/6] 安装 systemd 服务..."

# 主识别服务 (C++ facerec)
SERVICE_FILE="/etc/systemd/system/face-recog.service"
sudo cp "$SCRIPT_DIR/face-recog.service" "$SERVICE_FILE"
sudo sed -i "s|/home/pi/facerec|$HOME/facerec|g" "$SERVICE_FILE"
sudo sed -i "s|User=pi|User=$(whoami)|g" "$SERVICE_FILE"

# 注册 API 服务 (Python Flask)
REGISTER_SERVICE="/etc/systemd/system/face-register.service"
cat << EOF | sudo tee "$REGISTER_SERVICE" > /dev/null
[Unit]
Description=Face Registration API (Flask)
After=network.target

[Service]
Type=simple
User=$(whoami)
WorkingDirectory=$MODEL_SRC
ExecStart=/usr/bin/python3 $MODEL_SRC/pi_server.py --port 5000
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable face-recog face-register

echo "[6/6] ✓ systemd 服务已安装"
echo ""

# ── 总结 ──
echo "=============================================="
echo "  安装完成！"
echo ""
echo "  下一步:"
echo "  1. 重启或手动启动服务:"
echo "     sudo systemctl start face-recog face-register"
echo "  2. 查看状态:"
echo "     sudo systemctl status face-recog face-register"
echo "  3. 查看日志:"
echo "     tail -f /var/log/face-recog.log"
echo ""
echo "  Windows 管理端配置:"
echo "    设置 pi_host 为树莓派 IP, pi_api_port=5000"
echo "    人员管理 → 注册人脸 → 自动同步到 Pi"
echo ""
echo "  MJPEG 视频流:"
echo "    http://<树莓派IP>:8080/video"
echo "=============================================="
