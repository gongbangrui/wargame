# 会话交接摘要（2026-07-17）

## 下次会话先做什么

在仓库根目录开始，先阅读：

1. `AGENTS.md`
2. `docs/CODE_REVIEW_2026-07-17.md`
3. `docs/NETWORKING_PLAN.md`

工作区在本轮开始前就有大量未提交修改、删除和新增文件。不要执行清理、重置、checkout 或回滚；
修改重叠文件前必须先阅读当前内容并保留用户改动。当前分支为 `develop`。

建议下一步只实施联网计划的阶段 0：引入结构化 `CommandResult`，显式拒绝未知 action，补充固定
种子/固定 tick 的确定性回放测试，然后运行完整测试、ASan/UBSan 和 QML 检查。不要直接把
`MessageBus` 或 `LocalTransport` 替换为 WebSocket。

## 用户原始目标

- 全面审查现有代码并修正确认的 bug。
- 根据当前项目状态，提供面向联网项目新手的开发和服务器部署计划。
- 将会话压缩导出，以便下次快速续接。

## 本轮完成情况

已修复：

- 场景加载、撤销和重做改为先完整校验再原子替换，失败不再破坏当前世界。
- `SimulationEngine::setScenario` 返回成功或失败；场景编辑器导出标准完整场景 JSON。
- 同一 tick 双方指挥所同时被摧毁时判平局，结果不再依赖哈希遍历顺序。
- engine 析构时解除外部 `ITransport` sink，避免悬空回调。
- `LockedStepClock` 在释放互斥锁后发信号，避免直接连接槽自锁。
- 机动及攻击单元在接近最终航点时精确吸附到目标点。
- `SnapshotCodec::diffUnitIds` 能检测左右任一侧新增的单元。
- 修正 CP 通信距离旁路测试中缺失的 CP 标记。
- 修复推演结束重复模态框和平局文案。
- 清除 51 个 QML Layout 尺寸警告，并修正 `EventDialog` 的含糊 `parent` 绑定。
- 新增 controller 回归测试并注册到 CMake。

主要涉及：

- `src/core/SimulationEngine.{h,cpp}`
- `src/core/LockedStepClock.cpp`
- `src/core/SnapshotCodec.cpp`
- `src/units/MobileUnitBase.cpp`
- `src/units/AttackUAV.cpp`
- `src/view/SimulationController.{h,cpp}`
- `qml/views/ScenarioEditorView.qml`
- `qml/SimulationRoot.qml`
- `tests/test_controller.cpp`

完整问题、原因、修复和残余风险见 `docs/CODE_REVIEW_2026-07-17.md`。

## 验证基线

本轮已经完成并通过：

- CMake/Ninja Debug 构建。
- `ctest`：120/120 通过。
- ASan + UBSan：120/120 通过。
- 选定的 Clang analyzer 检查无遗留发现。
- `qmllint` 退出码 0。
- QML Layout 警告 0，未使用 import 0。
- 离屏软件渲染启动 8 秒，无运行时输出或错误。
- `cmake --install` 可暂存程序、36 个地图瓦片和地图元数据。
- `git diff --check` 通过。

LeakSanitizer 未运行成功：受管执行环境使用 `ptrace`，LSan 会主动拒绝该环境。这不代表发现了泄漏；
地址和未定义行为检查已完成。

## 联网架构结论

当前程序仍是单进程 Qt 桌面应用，不是可部署的联网服务器。联网边界必须位于
`SimulationEngine` 外部：

```text
QML
 -> SimulationController
 -> LocalSessionAdapter 或 RemoteSessionAdapter
 -> ClientStateStore / WebSocketClient
 -> WSS
 -> SessionGateway
 -> RoomManager / AuthPolicy
 -> 权威 SimulationEngine + LocalTransport
```

必须保持以下规则：

- `MessageBus` 表达战场域通信规则，包括距离、ECM 和指挥所旁路；它不是互联网传输层。
- 服务器是唯一权威仿真节点，远程客户端不能各自运行权威 engine。
- 敌方状态必须在服务器序列化前按角色和视野裁剪，不能把全图发给客户端再由 QML 隐藏。
- `LockedStepClock` 用于测试和回放，不应等待客户端后再推进权威服务器。
- 身份、角色和阵营由服务器会话绑定，不能信任客户端提交的 `role` 或 `side`。

完整的新手开发、协议、安全、Docker Compose、Caddy、备份、升级和回滚步骤见
`docs/NETWORKING_PLAN.md`。

## 联网前 P0 工作

1. 让 `SimulationEngine::command` 返回结构化 `CommandResult`，稳定定义错误码并拒绝未知 action。
2. 增加 schema/protocol version、revision、tick、sequence 和命令幂等键。
3. 建立确定性回放测试，消除会影响结果的无序遍历和真实时间依赖。
4. 将现有 `corelib` 拆成 domain、client 和无头 server 依赖边界。
5. 增加 `ISessionAdapter`、`ClientStateStore` 和本地/远程适配器。
6. 构建不链接 Qt Quick 的 `wargame_server`。
7. 实现认证、服务器绑定角色/阵营、权限矩阵和服务端视野投影。
8. 实现包大小/速率限制、重连、缺口检测、重同步和非可信输入校验。
9. 增加持久化、健康检查、指标、备份、升级及回滚。

## 仓库关键约束

- CMake 要求 Qt 6.10+；忽略遗留 `.pro`，只用 CMake 构建。
- 可执行文件名为 `appindex`。
- C++ 类位于 `gbr` 命名空间。
- `SimulationController` 是唯一 C++/QML 桥梁；QML 不直接访问 engine、bus 或 unit。
- 视图模式只有 `editor`、`commandpost-red`、`commandpost-blue`、`director`。
- 单元类型只有 `CommandPost`、`ReconUAV`、`AttackUAV`、`GroundScout`、`JammerUAV`。
- 每方必须恰好有一个存活 CP 才能运行；场景 CP ID 应为 `red_cp` 和 `blue_cp`。
- 所有机动单元使用 `UnitFsm`；UI 文本、注释和状态消息保持中文。
- QML 使用 `QtQuick.Controls.Basic`；重计算查询应放进 `SimulationController`。

## 可直接粘贴给下次会话的提示词

```text
请读取 AGENTS.md、docs/SESSION_HANDOFF_2026-07-17.md、
docs/CODE_REVIEW_2026-07-17.md 和 docs/NETWORKING_PLAN.md。
保留当前 dirty worktree，不要 clean、reset、checkout 或回滚已有改动。

继续联网开发阶段 0，且只做这一阶段：
1. 为 SimulationEngine::command 引入结构化 CommandResult；
2. 稳定定义 action 和错误码，显式拒绝未知 action；
3. 更新 SimulationController/QML 调用链并保持现有中文提示兼容；
4. 增加固定种子、固定 tick 的确定性回归测试；
5. 运行完整测试、ASan/UBSan、qmllint 和 git diff --check。

不要用 WebSocket 替换 MessageBus/LocalTransport。互联网网关必须位于
SimulationEngine 外部。开始修改前先检查当前工作区和重叠文件。
```
