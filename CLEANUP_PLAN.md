# 全面代码审查与清理计划

## 审查日期: 2026-08-01

---

## 1. 需要删除的文件

### 1.1 已删除但仍在 git tracked 的文件
- `Makefile` - 已被删除，但 git 仍然追踪（显示为 Missing）

### 1.2 构建产物（应添加到 .gitignore）
- `build/` - 1004M，已在 .gitignore，但可定期清理旧构建
- `dist/` - 1.4M，包含打包的服务器发布版本，应保留但不提交
- `.omo/` - 1.1M，omo 工具缓存目录

### 1.3 Python 缓存（应删除）
```
./tests/__pycache__/
./server/account/__pycache__/
```

### 1.4 未追踪的辅助文档（评估是否需要）
- `DESIGN.md` - 设计系统文档（2.4K），有价值，应提交
- 多个 `AGENTS.md` 文件 - 部署/QML/服务器的代理工作指南（1.7-2.7K），有价值，应提交
- `deploy/QUICK_START.md` - 快速开始文档，应提交
- `deploy/package-one-click.sh` - 打包脚本，应提交
- `deploy/reset-room.sh` - 房间重置脚本，应提交

### 1.5 可能冗余的文件
- `qml/components/CommandPanel.qml` (320行) - 未被任何视图导入使用，是旧的指挥面板实现，**可以删除**
- `server/game/AuthoritativeRoom.cpp` - 未追踪文件，需检查是否是测试文件或已废弃

### 1.6 不应提交的文件（已在 untracked）
- `.github/` - GitHub CI 配置目录
- `.dockerignore` - Docker 构建忽略规则
- `.gitignore` - Git 忽略规则
- `package.json` - Node.js 依赖文件（用于 tools/ 脚本）

---

## 2. 代码质量问题

### 2.1 超大文件（需要重构或分割）
```
server/game/GameServer.cpp: 3606 行
server/account/app.py: 1840 行
src/core/SimulationEngine.cpp: 1812 行
src/view/SimulationController.cpp: 1713 行
qml/views/OnlineOperationsView.qml: 1367 行
qml/components/MapCanvas.qml: 1184 行
```

**建议**：
- `GameServer.cpp` 应拆分为 GameServer + RoomManager + CommandHandler
- `app.py` 应拆分为 app + routes + auth + admin
- `SimulationController.cpp` 应拆分出 NetworkController
- `OnlineOperationsView.qml` 应拆分为多个子组件

### 2.2 函数数量过多的文件
```
src/view/SimulationController.cpp: 69 函数
src/core/SimulationEngine.cpp: 61 函数
src/network/NetworkClient.cpp: 36 函数
src/core/UnitBase.cpp: 32 函数
```

**建议**：SimulationController 和 SimulationEngine 职责过多，应拆分

### 2.3 调试输出残留
- 19 处 qWarning/qCritical/qFatal 调用（正常，保留用于错误处理）
- 1 处 console.log（已验证：app.js 中无 console.log）

### 2.4 QML 锚点冲突
发现 10 处可能的 `anchors.fill` 与其他锚点冲突：
```
qml/components/SessionDialog.qml
qml/components/SettingsPanel.qml
qml/components/ChatPanel.qml
qml/views/DirectorView.qml
qml/views/ScenarioEditorView.qml
qml/views/RosterEditorDialog.qml
qml/views/CommandPostView.qml
qml/views/OnlineOperationsView.qml
```

**需要逐一检查这些文件的锚点使用是否正确**

---

## 3. 潜在 Bug 和代码异味

### 3.1 未发现的严重问题
- ✓ 无 TODO/FIXME 标记
- ✓ 无明显的死代码标记
- ✓ 无 .cpp 文件被不当 #include
- ✓ 无备份文件残留 (.bak, .old, .backup)

### 3.2 需要注意的模式
1. **CommandPanel.qml** - 320 行未使用的组件，建议删除
2. **AuthoritativeRoom.cpp** - 未追踪的新文件，需确认用途
3. **超大提交** - 上次提交修改了 30+ 文件，应该拆分为多个小提交

---

## 4. .gitignore 改进建议

当前 `.gitignore` 缺少：
```gitignore
# Python
*.pyc
__pycache__/
.pytest_cache/
.mypy_cache/
*.egg-info/

# Build artifacts
dist/
*.tar.gz
*.tar.gz.sha256

# Tool caches
.omo/
node_modules/

# OS
.DS_Store
Thumbs.db

# Editor
.vscode/
*.swp
*.swo
```

---

## 5. 架构改进建议

### 5.1 立即执行（低风险）
1. **删除未使用的 CommandPanel.qml**
2. **清理 Python 缓存目录**
3. **提交有价值的文档**（DESIGN.md, AGENTS.md, 部署脚本）
4. **更新 .gitignore** 添加 Python 和构建产物规则
5. **修复 git 状态** - 从索引中删除已删除的 Makefile

### 5.2 中期重构（中等风险）
1. **拆分 GameServer.cpp**
   - 提取 CommandHandler.cpp/h
   - 提取 RoomLifecycle.cpp/h
   - 保留核心 GameServer 逻辑

2. **拆分 app.py**
   - 提取 routes/admin.py
   - 提取 routes/rooms.py
   - 提取 routes/users.py
   - 提取 auth.py

3. **拆分 OnlineOperationsView.qml**
   - 提取 CommanderPanel.qml
   - 提取 ParticipantCommandFeed.qml
   - 提取 DeploymentPanel.qml

### 5.3 长期优化（需要全面测试）
1. 将 SimulationController 拆分为多个专注类
2. 引入单元测试覆盖核心逻辑
3. 添加代码复杂度监控

---

## 6. 执行优先级

### P0 - 立即执行（今天）
- [ ] 删除 `qml/components/CommandPanel.qml`
- [ ] 删除 Python `__pycache__` 目录
- [ ] 更新 `.gitignore` 添加缺失规则
- [ ] 从 git 索引删除 `Makefile`: `git rm Makefile`
- [ ] 提交 `DESIGN.md` 和各 `AGENTS.md`

### P1 - 本周内
- [ ] 审查 `AuthoritativeRoom.cpp` 用途，决定保留或删除
- [ ] 修复 QML 文件中的锚点冲突（逐一检查）
- [ ] 清理 `dist/` 中的旧发布包，仅保留最新版

### P2 - 下个迭代
- [ ] 拆分 GameServer.cpp（3606行 → 3个文件）
- [ ] 拆分 app.py（1840行 → 多个模块）
- [ ] 为 SimulationEngine 添加单元测试

---

## 7. 清理脚本

```bash
#!/bin/bash
# cleanup.sh - 执行 P0 优先级清理

set -e

echo "=== 清理 Python 缓存 ==="
find . -type d -name "__pycache__" -exec rm -rf {} + 2>/dev/null || true
find . -type f -name "*.pyc" -delete 2>/dev/null || true

echo "=== 删除未使用的 CommandPanel.qml ==="
git rm qml/components/CommandPanel.qml

echo "=== 从 git 索引删除 Makefile ==="
git rm Makefile 2>/dev/null || true

echo "=== 添加文档到 git ==="
git add DESIGN.md
git add deploy/AGENTS.md
git add qml/AGENTS.md
git add qml/components/AGENTS.md
git add qml/views/AGENTS.md
git add server/AGENTS.md
git add server/account/AGENTS.md
git add server/game/AGENTS.md
git add deploy/QUICK_START.md
git add deploy/package-one-click.sh
git add deploy/reset-room.sh

echo "=== 更新 .gitignore ==="
cat >> .gitignore << 'EOF'

# Python caches
*.pyc
.pytest_cache/
.mypy_cache/
*.egg-info/

# Build artifacts not already covered
dist/
node_modules/

# Tool caches
.omo/

# OS files
.DS_Store
Thumbs.db

# Editor files not already covered
*.swp
*.swo
EOF

echo "=== 清理完成 ==="
echo "建议执行: git status"
echo "然后提交: git commit -m 'chore: cleanup unused files and update .gitignore'"
```

---

## 8. 验证清单

清理后验证：
- [ ] `git status` 无意外修改
- [ ] `cmake --build --preset debug` 编译通过
- [ ] `cmake --build build/debug --target all_qmllint` QML 检查通过
- [ ] `docker compose -f deploy/compose.yml up -d --build` 容器启动成功
- [ ] 运行应用确认核心功能正常

---

## 总结

**发现的主要问题**：
1. 一个未使用的大型 QML 组件（CommandPanel.qml）
2. Python 缓存目录未清理
3. 有价值的文档未提交
4. .gitignore 不完整
5. 部分文件过大需要重构

**代码健康度**: 7.5/10
- 无严重 bug
- 无死代码标记
- 但有技术债务需要逐步偿还

**立即行动价值**: 高
- P0 清理可立即改善代码库卫生
- 无破坏性变更
- 10 分钟内完成
