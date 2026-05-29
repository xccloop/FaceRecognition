# 指数退避重试

## 在这个项目中的作用

Windows → 树莓派的同步链路可能因网络波动、Pi 重启、Pi 服务未启动等原因失败。指数退避重试让同步在故障恢复后自动完成，不需要人工干预。

对应代码：`Windows/sync.py` — SyncTask Worker（第 174 行起）

## 核心原理

### 为什么不能固定间隔重试

如果 Pi 刚好在重启（需要 30s 恢复），每 5 秒重试一次只会产生 6 条毫无意义的失败请求。更糟的是，多个同时失败的任务会形成"重试风暴"。

### 项目中的退避策略

```python
# Windows/sync.py
RETRY_INTERVALS = [2, 5, 10]  # 秒
MAX_RETRIES = 3
```

- 第 1 次失败 → 等 2 秒重试
- 第 2 次失败 → 等 5 秒重试
- 第 3 次失败 → 等 10 秒重试
- 共 3 次重试（加上首次，总共 4 次尝试）

累计等待时间：2+5+10 = 17 秒。

### 为什么选 2/5/10

- **2s**：先短间隔试一次——可能是临时抖动
- **5s**：短期故障，等一会就好
- **10s**：可能是较严重的问题（Pi 重启中），给足恢复时间
- **总超时 ~17s**：加上 HTTP timeout，单用户最坏 25-30s。对于后台同步场景可以接受

### SyncTask 持久化队列

同步任务不是放在内存队列里的——进程重启后会丢失。项目用 SQLite 表持久化：

```python
# SyncTask 表字段（在 models.py 中定义）
# - user_id: 要同步的用户
# - status: pending / done / failed
# - retry_count: 已重试次数
# - next_retry_at: 下次可重试的时间
# - last_error: 上次失败原因
```

Worker 线程每 30 秒扫描 `status=pending AND next_retry_at <= now` 的任务：

```python
# 伪代码
while True:
    tasks = get_pending_sync_tasks()   # 查 SQLite
    for task in tasks:
        if task.retry_count >= MAX_RETRIES:
            mark_as_failed(task)       # 最终失败，不再重试
        else:
            result = sync_user(task)
            if result.success:
                mark_as_done(task)
            else:
                task.retry_count += 1
                delay = RETRY_INTERVALS[task.retry_count - 1]
                task.next_retry_at = now + delay
    sleep(30)
```

### 退避策略的设计考量

| 考量 | 本项目的处理 |
|------|-------------|
| 最大重试次数 | 3 次（够用但不过度） |
| 退避因子 | 手动指定 2/5/10（不依赖公式，简单可控） |
| 去重 | 注册前 GET /api/features 检查是否已存在，避免重复注册 |
| 持久化 | SyncTask 表 → 进程重启不丢任务 |
| 最终失败 | 超过 3 次标记为 failed，不无限重试（避免僵尸任务） |

## 要学到什么程度

- 理解为什么需要退避而非常数间隔
- 理解退避因子的选择：短→中→长，给不同严重程度的故障适配恢复时间
- 理解重试次数上限的必要性：避免无限重试浪费资源
- 理解持久化队列 vs 内存队列的取舍
