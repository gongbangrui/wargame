# Changelog

所有显著的修改记录在此。版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

## [Unreleased — V2.3]

### 修复 (Bug Fixes)

- **F1 致命**：CP 摧毁后所有派单（attackOrder / flightPlan / engage / moveTo / withdraw / pursue / halt）静默丢失。
  `commandSenderIdFor` 现在返回空串表示"无活 CP"，所有命令入口（`cmdAssignTarget` 等 6 处）检测到空 sender 后 `emit errorOccurred("己方指挥所已摧毁，无法派单")`。删除原先对硬编码 `"red_cp"`/`"blue_cp"` 的回退。
- **F2 ECM 一致性**：`applyEcmJamming` 不再用 `baseDetectRange`（不受干扰），改用 `jam->detectRange()`，使干扰机自己被干扰时干扰范围同步缩放。
- **F3 setHp 守卫**：变化 < 0.5 不发射 `hpChanged`，避免高频小伤害触发 QML 重绘。`setParams` 同样优化。
- **F4 停机/胜负单一触发**：`recomputeReadyForSim` 不再直接 `setRunning(false)`，胜负判定独占 `checkWinLoseCondition`；只有"非死亡的就绪丢失"（重复 CP、缺失 CP）才会暂停。修复潜在双重弹窗。
- **F5 Ack 对称性**：`CommandPost::onMessage` 在 `TargetDestroyed` 分支补 `sendAck(m)`，与 `TargetDetect`/`EngagementReport` 对称。
- **F10 schedule/waypoints 冲突**：`cmdSetSchedule` 写入后 `setHasActiveWaypoints(false)`，让 schedule 接管 motion。`AttackUAV::FlightPlan` 检测到已有 schedule 时不再追加 waypoints（保留 schedule）。`stepMotion` 航点耗尽时不再 `clearSchedule()`，正确交接给 schedule。
- **F8 死代码**：删除 `CommandPost::m_reports` 字段（从未使用）。
- **F11 错误队列**：`errorDialog` 改为队列机制，多错误依次弹出而不是互相覆盖。
- **F13 自消息守卫**：`CommandPost` Withdraw 不再构造发送对象后才丢弃。
- **F15 距离语义**：新增 `GeoPos::distanceTo2D`，`canDetect`/`AttackUAV::distanceToTarget`/`applyEcmJamming`/`scanReconDetections`/`refreshDetectionCache`/`applySchedules` 改用 2D 距离，海拔不再稀释攻击/侦察范围。
- **F20 焦点恢复**：`SimulationController::onUnitDestroyed` 调用 `ensureFocusedConsistent`，焦点单元死亡后立即重选。

### 性能 (Performance)

- **F6 索引替换线性扫描**：`m_scenarioIndex` 替换 `std::find_if`，`addOrUpdateUnit`/`removeUnit`/`cmdSetSchedule`/`findScenarioUnit` 全部 O(1)。
- **F3 减少 QML 重绘**：见上。
- **F16 死单元路径采样跳过**：修复 tickUnits 在 dead unit 上还采样 recentPath。

### 阶段 2 联网前置（行为不变）

- **`ITransport` 接口**：定义 `src/core/ITransport.h`，为 `TcpTransport`/`UdpTransport` 预留接入点。当前未注入到 `SimulationEngine`，零行为变化。
- **`LocalTransport`**：in-process 默认实现，1:1 转发到 `MessageBus`。
- **`UnitOwner` 字段**：`enum class UnitOwner { Local, Remote }`，默认 `Local`；`tickUnits` 跳过 `Remote`（联机时 peer tick，local 仅镜像）。
- **`IClock` / `RealTimeClock`**：抽象时钟接口；`SimulationEngine::m_simTime`/`m_speedMul` 字段替换为 `m_clock->{simTime,speedMul}()`。
- **`MessageLogRecorder`**：可选 append-only 日志，默认关闭，零 IO。

### 测试

- **新增 `tests/test_phase1_fixes.cpp`**：覆盖 F1/F2/F3/F4/F5/F10/F11/F15 共 8 个用例。

### 文档

- **`CLAUDE.md` 同步**：
  - Qt 版本改为 6.10+（与 CMakeLists.txt 一致）
  - 主题色改为 `#080b14 / #0e1322 / #4090ff / #f04760`
  - 快捷键文档补全（W/S 加减速、A/D 切换单元等）
  - CP 摧毁行为文档化
  - 距离语义文档化（detectRange/attackRange 是 2D）

## [V2.2] — 历史

- 默认场景含红蓝双方各 1×CP / 1×侦察 / 1×攻击 / 1×地面 / 1×干扰机
- 50ms 实时 tick × speedMul 推进
- 红蓝指挥所视角切换（`commandpost-red`/`commandpost-blue`）+ 编辑器 + 导演席
- 单方指挥所摧毁判定胜负

[Unreleased]: #对比-V22