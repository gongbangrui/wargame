# VMF 演示模式 v2

`vmf-demo-v2` 是联网房间的服务端权威演示 profile。它依据
`design/vmf设计.docx` 描述的交互链路，并复用 `design/EncoderDecoder/` 的消息字典、
XML 规范化和 bit 编解码器。旧 `vmf-guided-strike-v1` 继续作为兼容模式存在，两者的状态机、
命令和检查点数据相互隔离。

## 1. 演示边界

- 只有红方参演。蓝方单位作为固定靶，不攻击、不反制，也不能被用户接管。
- 红方包含指挥、侦察、攻击和地面引导四类战位。真人可接管，否则服务器自动完成当前动作。
- 演示不处理通信中间节点被摧毁；单边 profile 的通信范围固定为无限。
- 联网仿真、流程 revision、动作顺序、权限和持久化均由 `game-server` 决定。QML 只发送命令并消费投影。
- 页面展示业务明文、规范 XML 的检查结果和编码元数据，不要求展示密文。

## 2. 六阶段与十七动作

| 阶段 | 顺序 | 动作 | 操作战位 | VMF 领域消息 |
| --- | ---: | --- | --- | --- |
| 目标报告 | 1 | `reportTarget` | 侦察 | `TargetReport` |
| 航路规划 | 2 | `planRoute` | 指挥 | `FlightPlan` |
| 航路规划 | 3 | `acceptRoute` | 攻击 | `RouteAcceptance` |
| 引导命令 | 4 | `issueGuidance` | 指挥 | `GroundGuideOrder` |
| 引导命令 | 5 | `acknowledgeGuidance` | 地面引导 | `Ack` |
| 地面引导 | 6 | `identityHello` | 攻击 | `IdentityReport` |
| 地面引导 | 7 | `identityConfirm` | 地面引导 | `Ack` |
| 地面引导 | 8 | `sendGuidancePackage` | 地面引导 | `GroundTargetReport` |
| 地面引导 | 9 | `acceptGuidance` | 攻击 | `RouteAcceptance` |
| 地面引导 | 10 | `reportAttackReady` | 攻击 | `AttackReadyReport` |
| 地面引导 | 11 | `authorizeAttack` | 地面引导 | `AttackAuthorization` |
| 地面引导 | 12 | `simulateAttack` | 攻击 | `EngagementReport` |
| 地面引导 | 13 | `reportBattleDamage` | 攻击 | `BattleDamageReport` |
| 地面引导 | 14 | `confirmDamageAssessment` | 地面引导 | `DamageAssessmentConfirm` |
| 摧毁确认 | 15 | `confirmTargetDestroyed` | 侦察 | `TargetDestroyed` / `TargetReport` |
| 返航 | 16 | `withdraw` | 指挥 | `WithdrawOrder` |
| 返航 | 17 | `confirmReturned` | 攻击 | `Ack` |

客户端提交 `expectedRevision`、`actionId`、当前阶段和当前战位。服务端拒绝过期 revision、
越序动作和错误战位；已经确认的 `actionId` 以幂等结果返回。未被真人接管的当前战位由服务器
自动执行，相邻自动动作至少间隔一秒，便于演示者观察阶段变化和消息内容。

摧毁确认支持 `destroyed` 和 `notDestroyed` 两种结果。选择 `notDestroyed` 时，服务端保留当前任务、
目标、航路和报告历史，从地面引导阶段的 `identityHello` 动作重新开始；下一次模拟攻击会递增任务的攻击次数。

## 3. 消息输入与检查

工作台提供两种输入方式：

- `template`：通过目标类型下拉、固定靶 ID、航点、毁伤比例和报告说明生成领域消息。
- `xml`：高级输入，与当前动作对应的 VMF 消息类型必须匹配。

两种输入最终调用同一套 `VmfMessageGateway` 字典与 Codec。服务端完成 encode/decode 往返后，
实时 `demoTrace` 可包含 canonical XML、decoded XML、Base64 wire、hex/binary 预览、bit/byte
长度、字段 bit offset/length、ACK 属性、往返一致性和业务明文。普通 snapshot/delta 只携带
最新 trace 摘要，不重复发送完整 XML、wire 或字段列表，以控制消息大小。

完整 trace 只投影给已认证的红方参演者和本房间管理员/导演；观察者和无关房间客户端不会收到。
状态投影仍由既有视野与角色边界控制，前端权限只用于禁用不可用操作，不构成授权。

## 4. 导演控制与固定靶脚本

房间管理员或导演可发送 `demoControl`：

- `pause` / `resume`：暂停或恢复流程自动推进。
- `jump`：跳到指定阶段的第一个动作，并开始新的 generation。
- `reset`：清空本代动作、幂等记录和 trace，从目标报告重新开始。
- `setTargetScript`：校验并加载固定靶脚本，然后从新 generation 开始。

固定靶脚本 schema 为 `version=1`，包含 `targets` 初始状态和按阶段排序的 `timeline`。
允许更新位置、可见性、生命值和 `active/damaged/destroyed` 状态；脚本上限为 256 KiB。
脚本只驱动演示投影，不让蓝方获得攻击或反制行为。

## 5. 持久化与恢复

| 房间 profile | checkpoint schema | checkpoint protocol |
| --- | ---: | ---: |
| `native` | 6 | 6 |
| `vmf-guided-strike-v1` | 7 | 8 |
| `vmf-demo-v2` | 8 | 8 |

schema 8 保存 `demoState`、generation/revision、当前动作、固定靶脚本及游标、目标状态、
有界 trace、动作历史和幂等 ID。`demoAction` 与 `demoControl` 先进入持久事件日志，再更新内存
权威状态；重启时先加载 checkpoint，再重放后续事件。演示 trace 最多保留 200 条，动作历史
和幂等 ID 最多保留 4096 条。

升级或回滚前必须备份 `/data/room-checkpoint.json`、`/data/room-commands.jsonl` 和轮转日志。
不要把 schema 8 检查点交给不支持 `vmf-demo-v2` 的旧服务器。生产恢复继续使用
`deploy/reset-room.sh` 的备份流程，不要直接编辑运行中的检查点。

## 6. 操作与验收

1. 在网页房间配置中选择“演示模式”，确认它与 PVE 互斥。
2. 在桌面房间管理中确认 profile 为 `vmf-demo-v2`，并检查红方四类单位与至少一个蓝方固定靶。
3. 接管需要人工演示的红方战位，其他战位保持自动控制；红方就绪后启动房间。
4. 按六阶段完成十七个动作，逐步检查明文、bit 长度、字段布局、往返一致性和 ACK；也要验证未摧毁回退路径。
5. 验证错误战位、过期 revision 和越序动作均被拒绝；断线重连后从当前权威步骤继续。
6. 使用暂停、恢复、阶段跳转和重置验证导演控制，再重启服务确认 checkpoint 恢复。
7. 确认蓝方不能被接管、不会攻击反制，且 snapshot 中没有完整 trace 或未投影敌情。

本地质量门禁：

```bash
cmake --build --preset debug
ctest --preset debug
cmake --build build/debug --target all_qmllint
UV_CACHE_DIR=/tmp/wargame-uv-cache uv run \
  --with-requirements server/account/requirements.txt \
  python tests/test_account_room_lifecycle.py
cmake -S server -B /tmp/wargame-server-check -GNinja
cmake --build /tmp/wargame-server-check
git diff --check
```

VMF 字典与 XML/bit 的设计回归可额外执行：

```bash
cmake --build build/debug --target vmf_design_regression
```
