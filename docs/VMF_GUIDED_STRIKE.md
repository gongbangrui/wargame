# VMF 引导打击实现说明

本文档把 `design/vmf设计.docx` 中的通信和流程要求，映射到当前
Qt/C++20 兵棋系统的实现。VMF 是场景级通信策略，不会改变旧的原生
JSON 消息场景；只有 `communicationPolicy.format` 明确设置为
`vmf-design-v1` 时才启用 XML 字典和 bit wire。

## 1. 端到端流程

每一方维护一个 `GuidedStrikeWorkflow`，阶段是：

```text
idle
  -> targetReported
  -> dispatchPending
  -> strikeDispatched
  -> groundGuidancePending
  -> engaging
  -> targetDestroyed
  -> withdrawPending
  -> withdrawn
```

阶段推进来自 `MessageBus::messagePosted` 的权威消息流，而不是来自
QML 自己修改的阶段字段：

1. 侦察无人机发送 `TargetReport`（兼容 `TargetDetect`），接收方必须是本方指挥所。
2. 指挥所发送 `StrikePlan` 和 `AttackOrder`，航点进入 `Land Route`。
3. 指挥所发送 `GroundGuideOrder`。
4. 地面分队发送 `GroundAttackConfirm`，攻击机进入交战阶段。
5. 攻击无人机的命中确认或已登记、仍在探测范围且能回传指挥所的侦察无人机发送
   `TargetDestroyed`；只有带有当前任务 `targetId` 和 `attackerId` 的报告才能确认摧毁。
6. 指挥所发送 `WithdrawOrder`，攻击机进入撤离阶段。

同一个消息 ID、重试消息和错误阵营消息不会再次写入工作流事件。工作流快照
保留任务 ID、阶段、侦察机、目标、攻击机、引导单元、关联 ID、时间戳、序列和最近
200 条阶段事件，能够随房间 checkpoint 一起恢复。

## 2. XML profile 与 bit 编码

`design/EncoderDecoder` 是 profile 的权威源。`VmfProfile::load()` 会加载：

* `dic.xml`：消息、Header/Body、Group/Field 层级和 GPI/GRI/FPI/FRI；
* `dic_content.xml`：DFI/DUI、字段位宽、枚举、范围和保留值；
* `msgStruct/*.xml`：具体消息结构和重复组布局。

`vmf::Codec` 采用 MSB-first bit stream：先解析结构和内容约束，再编码字段，
回填 Header 的 `length`（字节数），最后补零到字节边界。解码会检查截断、
长度、尾部 padding bit、重复组终止位和逻辑规则。编码后的消息会立即做一次
解码回环验证，未通过则不会进入 ACK 或传输队列。

字典中的 `Rules` 在加载时先做定义校验，再对具体消息求值：`CaseRules` 只有在
消息显式提供非空 `case` 时参与匹配；缺省 `case` 不会被 CaseRules 拒绝，但全局
`ConditionRules` 仍然执行。路径支持 `H/B`、`G/F/D/I`、负下标、通配下标、
`.iteration` 和 alias；下标只能用于 repeatable Group/Field，`.iteration` 必须
指向带 GRI/FRI 的重复容器。为兼容 `msg0_1.xml` 中已有的“先选定父实例、再统计
子重复组”规则，iteration 允许挂在已显式下标的 repeatable 父路径上；新字典仍应
优先使用无下标容器路径。`targetPath` 不允许 `in/not_in`。Header 标量可用
设计文档规定的 `H.D(name)` 简写。规则定义错误会在 profile 加载阶段返回诊断，
不会等到线上消息触发后才静默失效。

## 3. 领域消息到 VMF 字段

| 领域类型 | VMF 消息 | 承载内容 |
| --- | --- | --- |
| `TargetReport`、`TargetDetect`、`TargetTrack`、`PositionReport`、侦察确认 `TargetDestroyed` | `Target Report` | 目标类型、数量、敌我属性、22 bit 纬度、23 bit 经度、状态、可选方向、观测时间 |
| `StrikePlan`、`FlightPlan`、`GroundAttackConfirm`、`Guidance`、`WithdrawOrder`、`Withdraw` | `Land Route` | `RouteExtremeties` 重复航点、`CriticalPoints` 重复关键点、报告时间 |
| `AttackOrder`（带坐标/航点） | `Land Route` | 单点或多点航路 |
| `AttackOrder`（只有目标 ID）、ACK 及其他控制消息 | `NetworkMonitoring` | 有界消息类型、网络标识、通信状态和时间元数据 |

`Target Report` 的目标数组最多取 8 个目标组，每组用 GRI 终止；运动方向由
FPI 表示是否存在。`Land Route` 的航点最多 32 个，关键点最多 32 个，
每个重复组均有独立 GRI，关键点组由 GPI 表示存在。

### 3.1 消息目录、47001 与信息价值

`design/EncoderDecoder/message_catalog.json` 是 XML 字典之上的语义目录。它为
每条领域消息声明 `catalogId`、发送/接收战位、触发条件、前置条件、允许的下一
阶段、ACK 策略和信息价值估算。`47001` 是本项目目标报告的设计层编号，表示
“侦察发现目标 -> 指挥所接收”的高价值消息（评分 90）；它不是 `4200/001`
等 XML DFI/DUI，也不会替换 XML 字典的位宽或枚举定义。

目录同时描述一对多映射：带 `waypoints`/坐标的 `AttackOrder` 使用 `Land Route`，
只有目标标识的 `AttackOrder` 使用 `NetworkMonitoring`。编码前由
`VmfMessageCatalog` 解析该条件，不能由 QML 自行选择消息名。网关会把目录编号、
触发原因和信息价值写入受控 trace 元数据；服务器重算这些字段，客户端提供的同名
字段不会被当作授权依据。

### 3.2 消息收发与触发图

```text
TargetDetect/TargetReport (recon -> commander, 47001)
  -> StrikePlan + AttackOrder (commander -> attack, 47006/47007)
  -> GroundGuideOrder (commander -> ground, 47009)
  -> GroundAttackConfirm (ground -> attack, 47010)
  -> EngagementReport/TargetDestroyed (attack/recon -> commander, 47005/47004)
  -> WithdrawOrder (commander -> attack, 47012)
```

每条边都要求同一 `targetId`，摧毁确认还要保持 `attackerId`，并沿用
当前任务的 `correlationId`。关键边的 ACK 由目录决定；重试只重发同一 wire，
不会重新触发领域状态。服务器会在字典校验后再执行目录的角色、收发方和通信链
检查，并在 durable event 写入前执行工作流阶段预检；乱序、错误目标、错误攻击机
或伪造 correlation 的消息以 `VMF_SEQUENCE_INVALID` 拒绝，不会写入事件日志或给出
ACK。重放时再次执行同一预检，保证 checkpoint 恢复后的阶段边与在线处理一致。

坐标优先使用 payload 的 `latitude`/`longitude`（纬度范围 -90..90，
经度范围 -180..180）。若只有仿真逻辑坐标 `x/y`，当前使用
`0..1,000,000 m` bounded local-grid 映射到对应 VMF 位宽；接入真实地图时，
应在消息构造处注入 `MapProvider::logicalToGeo()` 的结果，避免把逻辑米制坐标
误当作地理度数。

## 4. ACK、通信和重试

`CommunicationPolicy` 示例：

```json
{
  "communicationPolicy": {
    "format": "vmf-design-v1",
    "vmfProfile": "vmf-design-v1",
    "ackTimeoutSec": 3.0,
    "maxRetries": 2,
    "automaticAck": true
  }
}
```

ACK 计时使用仿真时间而非墙上时钟。默认在 3 秒、6 秒重试，9 秒终止；
通信暂时中断时，pending ACK 保留原始 wire，链路恢复后重试发送完全相同的
VMF 字节。自动 ACK 只对有效且可达的接收方生成，重复消息只再次 ACK，不重复
调用单元处理器。pending ACK、自动 ACK 去重 ID 和脱敏 trace 摘要均可持久化。

所有 base64 必须是 canonical Qt base64（编码后再次编码必须完全相等），不能
有多余 padding、URL-safe 字符或空字符串；`wireBitLength` 必须是正整数，
未使用的尾部 bit 必须为零。

## 5. 本地与在线数据流

本地模式：

```text
QML -> SimulationController::command()/VMF API
    -> SimulationEngine -> MessageBus -> GuidedStrikeWorkflow/Unit handlers
    -> vmf::VmfMessageGateway -> local message cache
```

在线模式：

```text
QML -> SimulationController facade -> NetworkClient / WebSocket VMF envelope
    -> WebSocket vmfMessage
    -> GameServer 权限/阵营/通信链/字典校验
    -> durable event -> 权威 SimulationEngine/MessageBus
    -> vmfEvent + projected snapshot
```

QML 只读取 `SimulationController.vmfWorkflow`，该属性只包含有界工作流投影；
QML 不得访问 `SimulationEngine`、`MessageBus` 或网络内部对象。在线服务器
按战位类型执行第二道权限检查：侦察只能报告/位置，指挥员只能规划和下达引导
撤离，地面分队只能确认引导攻击，攻击机只能上报交战/摧毁/位置，观察员永远
不能发送 VMF。

`StateProjector` 对参与者只投影本方工作流，对观察员投影红蓝两方的阶段摘要；
wire/XML 原文和未投影 payload 不会通过 snapshot、event 或 VMF event 泄漏。

## 6. 操作界面与控制器 facade

`qml/components/GuidedStrikeWorkflowPanel.qml` 是本流程唯一的操作面板，已经
嵌入本地 `CommandPostView` 右侧栏和在线 `OnlineOperationsView` 的战术抽屉。
面板只读取 `SimulationController.vmfWorkflow`、单位查询和已投影的消息列表，不
持有第二份工作流状态。阶段改变必须来自本地 `MessageBus` 或服务器的
`vmfEvent`/snapshot；在线按钮点击后只显示“已发送，等待服务器回执”，不会把
阶段提前改成成功。

QML 使用的 facade 方法如下：

| 方法 | 允许的设计角色 | 作用 |
| --- | --- | --- |
| `reportGuidedStrikeTarget(reconId, targetId, report)` | recon | 目标报告；缺省报告会从当前目标快照补齐逻辑坐标和类型 |
| `dispatchGuidedStrike(attackerId, targetId, waypoints)` | commander | 发送 `StrikePlan` 和 `AttackOrder`，航点进入 `Land Route` |
| `commandGuidedStrikeGroundGuidance(guideId, attackerId, targetId)` | commander | 发送地面引导命令 |
| `confirmGuidedStrikeAttack(guideId, attackerId, targetId, waypoints)` | ground | 发送地面人工攻击确认 |
| `withdrawGuidedStrike(attackerId, home)` | commander | 发送返航位置和撤离确认 |

本地 facade 直接调用 `GuidedStrikeWorkflow`，编码失败或阶段不匹配会返回
`accepted=false`。在线 facade 先用当前场景的 VMF 字典完成 encode/decode 回环，
再调用 `NetworkClient::sendVmfMessage`；服务器仍会重新校验字典、消息类型、战位
归属、通信链路和持久化顺序。观察员不会获得可提交的 facade 权限。

## 7. GIS 坐标对齐与降级

`SimulationEngine::configureCommunication()` 将 `MapProvider::logicalToGeo()` 注入
`VmfMessageGateway`。地图元数据满足 EPSG:3857、`projectAlignment`、逻辑范围和
正向修订版本时，payload 中的 `x/y` 会先转换成真实纬度/经度，再按 VMF 22/23 bit
字段量化；显式 `latitude/longitude` 仍优先使用。编码后的受控 payload 带有
`vmfCoordinateSource=map-gis` 或 `geo`，便于日志和验收区分来源。

没有有效地图元数据、点超出地图范围或 resolver 返回无效坐标时，才使用
`0..1,000,000 m` 的 `logical-grid-fallback`。该值是协议兼容的本地网格，不得当作
真实经纬度展示或用于跨系统 GIS 分析；面板仍显示逻辑米制航点。恢复/切换地图时，
resolver 随当前 `MapProvider` 元数据重新绑定，不能缓存上一张地图的原点。

## 8. 验收清单

* VMF profile 为 `native` 时旧场景保持原生消息路径，面板不显示；显式设置
  `communicationPolicy.format=vmf-design-v1` 后面板出现在本地和在线操作面。
* 依次完成目标报告、派单/航路、地面引导、地面确认、目标摧毁和撤离；重复消息、
  乱序消息、错误目标或错误阵营不会推进阶段。
* 在线端用侦察、指挥、地面和旁观者战位分别验证按钮可见性；服务器拒绝伪造的
  sender/receiver、非本方目标或断链消息，客户端不显示乐观成功。
* 在带 `map/metadata.json` 的运行环境中，对同一逻辑点用 resolver 和显式地理坐标
  编码，VMF bit wire 完全一致；无元数据 fixture 明确得到
  `logical-grid-fallback`。
* 检查 VMF 消息的 canonical base64、bit length、零 padding、ACK 重试和 checkpoint
  恢复；观察员快照只含红蓝阶段摘要，不含 wire、XML 或完整事件历史。
* 乱序或关联 ID 不一致的在线 VMF 消息应收到 `VMF_SEQUENCE_INVALID`，并且服务器
  的 durable event 日志不增加对应记录。

在 VMF 场景中，攻击机实际造成 HP 归零后，仿真引擎只在当前任务登记的侦察机能够
探测目标且通信链路可达指挥所时自动生成一条侦察确认；没有登记任务时最多选择一
个满足条件的侦察机。该确认是只读态势消息，不会再次修改目标生命值，也不会绕过
工作流的关联 ID 校验。该消息由服务端从 `MessageBus::messagePosted` 转换为
`vmfMessage` durable event，事件 payload 明确带有 `generatedBy=simulation`、
`userId=0` 和实际侦察战位 `seatId`；服务器保存完整 canonical VMF wire，客户端
看不到该持久化记录的原始 payload。事件追加失败只记录持久化告警，不会伪造 ACK，
后续检查点仍以服务端当前权威状态为准。

## 9. Checkpoint 与重放

服务器 checkpoint 的 `vmfState` 是可选字段，旧 checkpoint 没有该字段时按空
VMF 状态兼容恢复。恢复顺序是：场景/单元 -> 全局仿真状态 -> VMF 工作流、
pending ACK、去重 ID、trace 摘要；任一结构校验失败都不会提交半恢复状态。
自动侦察确认遵循与客户端 VMF 相同的日志契约：先追加事件，再由消息总线继续投递；
因此定时检查点只能落在已经有对应 durable event 的状态上。`restoreRoomState()`
先恢复 checkpoint，再按事件序列调用 `applyDurableEvent("vmfMessage")`，重新执行
wire 解码、发送战位/阵营/目录/工作流校验并更新 `GuidedStrikeWorkflow`。重放期间
设置服务端 replay guard，服务端生成消息不会再次追加日志，也不会递归生成第二条
侦察确认；消息总线为恢复 pending ACK 所做的内存级 ACK 处理不再转成 durable event
或对外广播。成功后才更新 `eventSequence` 并写回新的 checkpoint。生成事件的 `userId=0` 不参与
客户端 trace 去重，`seatId` 仍必须指向当时登记的侦察单元；空 `traceId` 只允许该
服务端生成路径，客户端消息仍要求非空关联和 trace。观察员只收到红蓝双方的阶段摘要
和脱敏 `vmfEvent`，不收到 wire/XML、完整 payload、事件日志或检查点。

## 10. 正例、反例和验证

聚焦 VMF 编解码、字段映射、ACK、工作流和协议校验：

```bash
cmake --build build/debug --target wargame_tests -j2
./build/debug/wargame_tests --gtest_filter='VmfCodec.*:VmfCatalog.*:VmfGatewayTest.*:VmfMessageBusTest.*:GuidedStrikeWorkflowTest.*:ProtocolTest.*'
```

联网服务启动后，使用隔离账号和临时房间运行完整 VMF 验收：

```bash
ADMIN_PASSWORD='管理员密码' node tools/vmf-guided-strike-smoke.mjs
```

没有启动联网服务时，可先只验证真实 XML fixture 与字典编码：

```bash
VMF_FIXTURES_ONLY=1 node tools/vmf-guided-strike-smoke.mjs
```

脚本覆盖目标报告、派单/航路、地面引导、人工确认、摧毁与撤离、ACK、重复
trace、伪造 sender/receiver、断链和观察员脱敏；结束时会关闭会话、停止临时房间
并删除临时账号。它与通用 `network-smoke.mjs` 使用不同的账号前缀，不依赖生产数据。

完整验证：

```bash
cmake --build build/debug -j2
ctest --test-dir build/debug --output-on-failure
cmake --preset sanitizers
cmake --build build/sanitizers -j2
ctest --test-dir build/sanitizers --output-on-failure
./tools/check-source-format.sh
```

设计目录的独立 CLI 也由根工程统一构建和发布（依赖 `tinyxml2`），并提供一条不写入
源码目录的回归目标：

```bash
cmake --build build/debug --target vmf_design_regression
```

该目标实际执行 `NetworkMonitoring` 的 validate -> encode -> decode -> compare，
以及中文 `Land Route` 描述到 XML、bit 流、反解和文本的完整管线。安装包同时放入
四个 CLI、`dic.xml`、`dic_content.xml`、`message_catalog.json` 和 `msgStruct/*.xml`；
没有 tinyxml2 时根工程继续可构建，但 CMake 会明确报告 `VMF EncoderDecoder CLI:
disabled`，发布脚本不会假装回归已执行。

反例覆盖包括：错误 XML 字段名、缺失或截断 wire、非 canonical base64、非零
padding bit、错误 GRI/FPI、空目标的摧毁确认、重复 trace、错误阵营/战位、
无通信链路、观察员发送和 malformed workflow checkpoint。
