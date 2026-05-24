# CLAUDE.md — Git 与 GitHub 日常编码规范

> 本项目使用 Git + GitHub 管理完整项目生命周期。以下规则对应 [Git 与 GitHub 项目管理指南](docs/Git与GitHub项目管理指南.md)，Claude 在日常编码中必须遵守。

---

## 一、铁律

1. **永不直接在 main 上开发。** 任何代码改动都必须在功能分支上进行。
2. **原子化提交。** 一次 commit 只做一件事。commit message 中出现"和"、"以及"、"另外"意味着应该拆成多条。
3. **Commit message 遵循 Conventional Commits。** 格式：`<type>(<scope>): <subject>`
4. **禁止提交密码、Token、密钥。** 发现后立即吊销并清理历史。
5. **禁止 force push 到 main。** 永远不要。

---

## 二、分支策略

```
main ─────●────────────●──────────●──────  （始终可部署）
           \          /          /
            ●──●──●──●          /         feature/xxx
                                /
                               ●──●──●    fix/yyy
```

### 分支命名规范

| 类型 | 格式 | 示例 |
|------|------|------|
| 新功能 | `feat/<描述>` | `feat/add-relay-gpio` |
| 修 Bug | `fix/<描述>` | `fix/usart-ore-deadlock` |
| 文档 | `docs/<描述>` | `docs/uart-protocol` |
| 重构 | `refactor/<描述>` | `refactor/extract-matcher` |

### 分支操作流程

```bash
# 1. 开始工作前，切回 main 拉最新
git checkout main && git pull

# 2. 创建功能分支
git checkout -b feat/<功能名>

# 3. 开发、多次原子 commit、推送
git push origin feat/<功能名>

# 4. 在 GitHub 创建 PR → Squash and merge

# 5. 合并后删本地分支，切回 main，pull
git checkout main && git pull
git branch -d feat/<功能名>
```

---

## 三、Commit Message 规范

### 格式

```
<type>(<scope>): <subject>

<body>（可选，说明为什么改、怎么改）
```

### Type 必须用以下之一

| type | 含义 | 示例 |
|------|------|------|
| `feat` | 新功能 | `feat(stm32): 添加继电器 GPIO 控制` |
| `fix` | 修复 bug | `fix(linux): 修复 ncnn Shape 层维度计算错误` |
| `docs` | 文档变更 | `docs(project): 补充串口通信协议文档` |
| `refactor` | 重构（不改功能） | `refactor(win): 提取特征匹配逻辑为独立模块` |
| `test` | 添加/修改测试 | `test(pi): 添加 FrameQueue 单元测试` |
| `chore` | 构建、依赖等杂项 | `chore(stm32): 更新 HAL 库版本` |

### Scope 使用以下之一

- `stm32` — STM32 固件
- `linux` / `pi` — 树莓派端
- `windows` / `win` — Windows 后台
- `project` — 跨端/项目级

### 正确 vs 错误

```
❌ fix bug
❌ update
❌ feat: 实现队列和修复串口bug和更新文档

✅ feat(pi): 实现 FrameQueue 模板类
✅ fix(pi): 修复 result queue pop 超时返回值
✅ docs(pi): 补充队列模块使用说明
```

---

## 四、每日开发流程

Claude 在接到编码任务时应遵循以下流程：

```
1. 理解需求 → 确认对应的 GitHub Issue（如有）
2. 创建分支 → git checkout -b feat/<描述>
3. 编码 → 按原子粒度多次 commit
4. 推送 → git push origin <branch>
5. 提醒用户 → 去 GitHub 创建 PR，选 Squash and merge
```

### 代码审查自检清单（提交前自查）

- [ ] 改动的文件是否符合预期？（无不相关的误改）
- [ ] 有没有遗留调试代码？（`printf("test\n")` 等）
- [ ] 有没有硬编码的密码/Token？
- [ ] commit message 是否规范？

---

## 五、Issue 与 Commit 关联

- 在 commit body 或 PR 描述中写 `Closes #N` 或 `Fixes #N`，合并后自动关闭对应 Issue
- 在 commit 或 PR 中引用另一个 Issue 时写 `#N`，GitHub 自动生成链接

```
✅ feat(stm32): 添加继电器控制 Closes #1
```

---

## 六、合并策略

- **单人/小团队项目默认使用 Squash and Merge**
- 一个 PR 压成一条干净的 commit
- 合并后在 GitHub 上删除远程分支

---

## 七、Release 规范

代码达到可交付里程碑时打 tag 并发布 Release：

```bash
git tag -a v0.1.0 -m "v0.1.0: <简短描述>"
git push origin v0.1.0
```

版本号语义：`v<主要>.<次要>.<修订>`（如 v0.1.0、v0.2.0）

Release Notes 按 `Added / Fixed / Changed` 分类写。

---

## 八、安全红线

1. **绝不提交 `.env`、`config.local.json`、密钥文件。** 这些必须在 `.gitignore` 中。
2. **push 前用 `git status` 确认暂存区。** 避免误提交敏感文件。
3. **如果意外提交了秘密：**
   - 立即在 GitHub Settings → Developer settings 吊销 Token
   - 用 `git filter-repo` 清理历史
   - 强制推送
4. **清理后仍需创建新的 commit 来覆盖历史，绝不能 amend 已推送的包含敏感信息的 commit。**

---

## 九、Claude 行为约束

执行编码任务时：

1. **不主动 commit。** 除非用户明确要求，否则只写代码、不创建 commit。
2. **不主动 push。** 除非用户明确要求，否则只做本地操作。
3. **不执行破坏性 git 命令**（`push --force`、`reset --hard`、`git clean -f`）除非用户明确要求。
4. **创建新 commit 而非 amend。** 除非用户明确要求 amend，否则始终创建新 commit。
5. **使用 HEREDOC 传递 commit message**，确保格式正确：

```bash
git commit -m "$(cat <<'EOF'
feat(stm32): 添加继电器 GPIO 控制

- PB12 设为推挽输出控制继电器
- 添加 relay_on() / relay_off() 函数
EOF
)"
```

6. **提交前检查：**
   - `git status` — 确认提交范围
   - `git diff --stat` — 确认改动文件符合预期
   - `.gitignore` 是否覆盖了所有不应提交的文件
