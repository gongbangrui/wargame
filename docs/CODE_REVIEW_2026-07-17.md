# 代码审查报告（2026-07-17）

## 审查结论

当前版本已经是可构建、可运行、单元测试较完整的单机推演程序，但仍不是可联网部署的服务器。
本轮修复了会破坏场景、产生错误胜负、造成悬空回调或死锁的确定性缺陷，并将联网前的架构风险
集中记录到 `docs/NETWORKING_PLAN.md`。

审查后的验证结果：

- CMake/Ninja Debug 构建成功。
- `ctest --test-dir build --output-on-failure`：120/120 通过。
- ASan + UBSan：120/120 通过，无地址或未定义行为报告。
- LeakSanitizer：受管环境使用 ptrace，LSan 本身拒绝运行，因此仅关闭 leak 检测。
- `qmllint`：退出码 0；51 个布局未定义行为已全部修复。
- Qt 离屏软件渲染启动 8 秒，无 QML 加载错误或运行时告警。
- `git diff --check` 通过。

## 已修复问题

### 1. 高：场景读取、撤销和重做可能先清空世界再部分恢复

原行为在 QML 中逐个删除现有单元，再逐个调用 `upsertUnit`。只要文件中间出现一个非法单元，
当前世界已经被清空或只恢复一部分；空 `units` 数组也能绕过引擎的整场景保护。

修复：

- `SimulationEngine::setScenario` 返回是否完整应用成功。
- 新增 `SimulationController::replaceUnits` 和 `replaceScenario`。
- 整份数据先解析、校验，再一次性替换；失败保持原世界不变。
- `unitsJson()` 改为标准 `ScenarioIo` JSON，保存文件包含完整地图对象和备注。
- 撤销、重做和读取统一走原子替换接口。

回归测试：`SimulationControllerTest.ReplaceUnitsRejectsInvalidInputAtomically`。

### 2. 高：同一 tick 双方指挥所被摧毁时，胜者取决于哈希遍历顺序

原行为在第一个 CP 的 HP 变为 0 时立即结算。第二个攻击单元仍会在当前 tick 执行并摧毁另一方
CP，最终世界是双方 CP 都死亡，但已经对先执行的一方宣告胜利。

修复：tick 内只更新就绪状态，等全部单元完成该 tick 后统一结算。双方均无存活 CP 时返回平局。

回归测试：`EngineTest.SimultaneousCommandPostKillsProduceDraw`。

### 3. 高：外部 transport 比 engine 存活更久时保留悬空 sink

`SimulationEngine` 把捕获 `this` 的 lambda 写入外部 `ITransport`，析构时没有清除。外部 transport
随后收到消息会调用已经销毁的 engine。

修复：engine 析构时先清除 message sink，再由成员析构清理单元订阅。

回归测试：`LocalTransportTest.ExternalTransportOwnerNotDeleted`，并由 ASan 覆盖释放后投递。

### 4. 中：锁步时钟持有互斥锁发信号，可直接自锁

`LockedStepClock::advance/step` 在持有 `QMutex` 时发出 `stepped`。直接连接的槽只要调用
`simTime()` 就会再次申请同一把非递归锁。

修复：锁内只更新并复制时间，解锁后再发信号。

回归测试：`LockedStepClockTest.SignalHandlerCanReadClockWithoutDeadlock`。

### 5. 中：机动单元距航点小于 50 米时直接结束，但位置没有落到航点

地面、侦察、干扰和攻击单元共用或复制了这一逻辑。例如单航点在 25 米外时，单元会立即报告
完成并留在原地。

修复：进入 snap 阈值后先将 XY 精确设置为目标，再推进下一个航点或结束。

回归测试：

- `EngineTest.GroundRouteSnapsToNearbyFinalWaypoint`
- `EngineTest.AttackFlightPlanSnapsToNearbyFinalWaypoint`

### 6. 中：快照差异比较不对称

`SnapshotCodec::diffUnitIds(a, b)` 只报告 a 中存在、b 中缺失的 ID，不报告 b 新增的 ID。
这会让同步验证把“远端多出单元”误判为一致。

修复：比较两侧 ID 并集。

回归测试：`SnapshotCodecTest.DiffDetectsUnitsOnlyPresentOnRight`。

### 7. 中：CP 距离旁路测试自身缺少 CP 注册

失败测试只注册了名为 `red_cp` 的普通单元，没有调用接口要求的 `setUnitCommandPost`，因此测试
期望与生产语义不一致。

修复：测试显式标记 CP。生产引擎创建 CP 时原本已正确标记。

### 8. 中：推演结束可能出现两个模态框，平局文字也会拼成“获胜”

全局根组件和指挥所视图都可能处理同一次 CP 死亡。平局时 `loser` 为空，原模板仍拼接“获胜”。

修复：停止后的本地 CP 检查不再打开第二个对话框；全局弹窗对无 loser 的平局直接显示结果。

### 9. 低：QML Layout 中 51 处固定尺寸属于未定义行为

在 `RowLayout/ColumnLayout/GridLayout` 管理的直接子项上同时使用 `width/height`，不同 Qt 版本可能
忽略尺寸或产生跳动。

修复：全部改为 `Layout.preferredWidth/Height`。`EventDialog` 也改为显式按钮 ID，不再通过动态
`parent` 读取 `text/hovered`。

## 仍需处理的风险

以下不是本轮可以用小补丁安全完成的事项，必须按联网计划分阶段处理。

### P0：联网前必须完成

1. `ITransport` 当前包装的是仿真域 `MessageBus`。非本地实现若 `bus()` 返回空，单元不会注册、
   更新位置或发送内部战术消息。不能直接把它替换为 WebSocket。
2. 远程客户端仍没有 `ClientStateStore`，`SimulationController` 仍直接拥有本地 engine。
3. `SimulationEngine::command` 仍返回 `void`，未知 action 会静默忽略，无法形成可靠网络回执。
4. 没有身份、角色、阵营权限、服务器视野裁剪、幂等命令、包大小和速率限制。
5. `corelib` 混有 Qt Quick 和视图代码，不能形成干净的无头服务器依赖。
6. 快照没有协议版本、revision、tick、sequence 和严格的非可信输入 schema。

### P1：公开测试前完成

1. `std::unordered_map` 遍历和真实时间戳会让消息日志顺序及字节级快照哈希不完全可重现。
2. `MessageLogRecorder` 没有把磁盘写满或后续 flush 失败反馈给 engine。
3. 设置文件不是 `QSaveFile` 原子写，写入失败也没有用户提示。
4. QML 使用运行时 context property 和运行时 `qmlRegisterType`，`qmllint` 仍有静态类型告警。
5. 缺少真实窗口交互自动化、不同 DPI/分辨率截图回归和 Windows 打包测试。
6. 地图哈希尚未绑定场景和会话，客户端无法拒绝错误地图版本。

### P2：运维成熟度

1. 没有服务器健康检查、指标、结构化审计、备份和恢复工具。
2. 没有弱网、慢客户端、长稳、压力和协议模糊测试。
3. 没有发布镜像、SBOM、漏洞扫描、staging 和回滚流水线。

## 推荐下一步

1. 先把命令结果结构化并补确定性回放测试。
2. 拆分纯 domain 库和无头 `wargame_server` 空壳。
3. 在内存回环中完成 `ClientStateStore`、权限和视野投影。
4. 再加入本机 WebSocket，最后才配置公网 WSS。

完整实施和服务器操作步骤见 `docs/NETWORKING_PLAN.md`。
