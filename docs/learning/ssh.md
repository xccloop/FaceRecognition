# SSH

## 在这个项目中的作用

树莓派 4B 部署后不接显示器、不接键盘，SSH 是你对它唯一的日常操作入口。所有代码部署、调试、日志查看都通过 SSH 完成。

## 项目中用到的具体操作

### 基础连接

Pi 的 IP 在项目中硬编码为 `192.168.137.100`（Windows 的 config.json 中的 `pi_host`），你需要通过 SSH 连上去：

```bash
ssh qxc@192.168.137.100
```

### 免密登录

配置 SSH Key 后不用每次输密码：

```bash
# 在本机生成密钥对（如果还没有）
ssh-keygen -t ed25519

# 把公钥拷贝到 Pi
ssh-copy-id qxc@192.168.137.100
```

### 传文件

部署新编译的程序或模型文件：

```bash
# 上传
scp facerec qxc@192.168.137.100:~/facerec/
scp models/ncnn_models/*.bin qxc@192.168.137.100:~/facerec/models/

# 下载（拉日志）
scp qxc@192.168.137.100:~/facerec/log.txt .
```

### SSH Config 简化

在 `~/.ssh/config` 中配置别名：

```
Host pi
    HostName 192.168.137.100
    User qxc
    IdentityFile ~/.ssh/id_ed25519
```

之后直接 `ssh pi`、`scp file pi:~/` 即可。

## 要学到什么程度

- 免密登录配置——日常使用的基本要求
- scp / rsync 传文件——部署新版本的必备操作
- SSH config 文件的基本写法
- 理解 SSH 是加密隧道，传输过程安全
