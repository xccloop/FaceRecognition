# systemd 服务

## 在这个项目中的作用

树莓派程序需要满足：开机自启、崩溃自动重启、后台运行（不占用终端）。Linux 的 systemd 是标准的服务管理器，通过写一个 service 文件即可实现。

对应文件：`tools/face-recognition.service`

## 项目中用到的具体配置

### Service 文件解析

```ini
# tools/face-recognition.service
[Unit]
Description=FaceRecognition Pi Runtime          # 服务描述
After=network.target                            # 等网络就绪后再启动

[Service]
Type=simple                                     # 启动后不等待，直接认为已运行
User=qxc                                        # 以哪个用户身份运行
WorkingDirectory=/home/qxc/Desktop/Raspberry Pi # 工作目录
ExecStart=/home/qxc/miniforge3/envs/facerec/bin/python3 ...   # 启动命令
Restart=on-failure                              # 仅在异常退出时重启
RestartSec=10                                   # 重启前等 10 秒
StandardOutput=journal                          # 标准输出写入 journal 日志
StandardError=journal                           # 标准错误也写入 journal

[Install]
WantedBy=multi-user.target                      # 多用户模式下启动
```

### 关键字段说明

| 字段 | 值 | 含义 |
|------|-----|------|
| `After` | `network.target` | 等网络初始化完再启动——程序依赖网络 |
| `Restart` | `on-failure` | 进程 crash（非 0 退出码）或超时被 kill 时才重启，通过 `systemctl stop` 正常停止不会重启 |
| `RestartSec` | `10` | 崩溃后等 10s 再重启，防止频繁 crash 时反复重启耗尽资源 |
| `Type` | `simple` | 最简单的类型——ExecStart 启动后 systemd 就认为服务在运行了 |

### 常用命令

```bash
# 安装服务
sudo cp face-recognition.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable face-recognition    # 开机自启
sudo systemctl start face-recognition     # 立即启动

# 日常管理
sudo systemctl status face-recognition    # 查看状态
sudo systemctl stop face-recognition      # 停止
sudo systemctl restart face-recognition   # 重启

# 查看日志
journalctl -u face-recognition -f         # 实时跟踪
journalctl -u face-recognition -n 100     # 最近 100 行
```

## 要学到什么程度

- 能写一个基础的 service 文件（Description、ExecStart、Restart 三个字段最常用）
- 理解 `systemctl enable/start/stop/status` 四个常用操作
- 会用 `journalctl -u <服务名> -f` 实时看日志
- 理解 `Restart=on-failure` 和 `Restart=always` 的区别
