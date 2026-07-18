# 联网开发与服务器部署计划

本文面向第一次开发联网项目的维护者。它描述目标架构、开发顺序、协议、安全、测试、
服务器上线、备份和回滚。P0 联网闭环现已在仓库实现；可直接执行的本地启动、token 创建、
备份和 Compose 步骤见 `docs/P0_NETWORKING_IMPLEMENTATION.md`。本文仍保留分阶段设计和后续
压力测试、staging 演练要求。

## 1. 先明确产品边界

第一版建议只支持以下规模：

- 一个服务器实例承载 1 至 10 个推演房间。
- 每房间 2 名阵营用户、1 名导演、若干只读观察员，最多 32 个连接。
- 每房间最多 500 个单元，服务器固定以 50 ms 仿真 tick 推进。
- Qt 桌面客户端，不是浏览器网页。玩家仍需安装客户端。
- Linux 服务器，推荐 Ubuntu 24.04 LTS、2 vCPU、4 GB 内存、40 GB SSD 起步。
- 第一版使用单节点，不做数据库集群、Kubernetes 或跨机锁步。

在开始写网络代码前，产品负责人必须回答：

1. 编辑器是否只能在房间暂停时改场景。建议第一版只允许暂停时修改。
2. 红蓝双方是否能看到“曾发现但已丢失”的目标，以及轨迹保留多久。
3. 观察员看到导演全图还是某一阵营视野。
4. 房间在所有人离线后保留多久，服务器重启后是否恢复。
5. 最大房间数、最大单元数、允许的最大地图和场景文件大小。

## 2. P0 实现状态

表中原有阻塞项已由 `wargame_domain`、`wargame_protocol`、`wargame_clientlib`、
`wargame_serverlib` 和 `wargame_server` 处理。远程客户端只使用 `ClientStateStore`，服务端在
序列化前完成权限和视野投影，SQLite 保存检查点、审计及幂等结果。阶段 6 的本机自动化基线已
覆盖 32 个连接、500 单元快照、协议违规、幂等限流交错、断线命令重发和损坏检查点回退。
8 小时长稳、真实弱网、磁盘故障、staging 证书和备份恢复演练仍未完成，不能据此开放公网。

### 原始差距分析

当前已有的良好基础：

- `SimulationEngine` 是统一权威状态入口。
- QML 只通过 `SimulationController` 交互。
- `controller.command(action, args)` 已形成统一命令入口。
- 场景可序列化，已有运行时快照雏形和 50 ms 时钟抽象。
- `LocalTransport`、`MessageBus`、角色视图和大量单元测试已经存在。

当前不能直接部署联网的原因：

| 现状 | 联网风险 | 正确处理 |
|---|---|---|
| `SimulationController` 直接拥有 `SimulationEngine` | 每个客户端会各算一份世界，状态必然分叉 | 远程客户端只持有 `ClientStateStore` |
| `corelib` 同时链接 Qt Quick 和视图代码 | 无头服务器仍依赖 GUI 库 | 拆分 domain/client/server 库 |
| `ITransport` 包装 `MessageBus` | 容易把互联网连接和战场通信距离混为一谈 | WebSocket 网关放在引擎外部 |
| `SnapshotCodec` 没有协议版本、房间 revision 和事件序号 | 无法可靠升级、补包或拒绝旧客户端 | 增加协议 envelope 和 schema 校验 |
| 命令只返回 `void`，错误依赖字符串信号 | 客户端无法关联请求和结果 | 引入结构化 `CommandResult` |
| 所有状态都存在本地引擎中 | 若直接发完整快照，红蓝客户端可读取敌方全图 | 服务器生成按角色裁剪的视图 |
| 没有登录、权限、限流、审计 | 任意连接可控制任意单元 | 身份和角色只由服务器绑定 |
| 没有 headless server、健康检查和指标 | 无法可靠托管 | 新建独立服务器目标和运维接口 |

## 3. 目标架构

```text
Qt 桌面客户端
  QML
   |
  SimulationController
   |-- LocalSessionAdapter  -> SimulationEngine（单机/测试模式）
   `-- RemoteSessionAdapter -> ClientStateStore + WebSocketClient
                                      |
                                  WSS / JSON
                                      |
反向代理 Caddy（TLS、连接入口、基础限流）
                                      |
                                  SessionGateway
                           /          |          \
                    AuthPolicy   RoomManager   ProtocolCodec
                                      |
                              SimulationRoom
                                      |
                        SimulationEngine + LocalTransport
                                      |
                            SQLite / 审计 / 检查点
```

最重要的规则：

- 服务器是唯一权威仿真节点。
- 客户端发送“意图”，例如移动、攻击、暂停，不发送最终坐标或 HP。
- 客户端不推进仿真，不参与服务器锁步，也不能因断线让服务器停钟。
- `MessageBus` 继续表达仿真域内通信规则，例如距离、ECM、CP 旁路。
- WebSocket 只表达客户端与服务器之间的可靠互联网传输。
- 红蓝视野裁剪发生在服务器序列化之前，不能把全图发给客户端再由 QML 隐藏。
- 导演和编辑器权限也必须由服务器校验，不能信任客户端传来的 `role`。

`LockedStepClock` 可用于测试、回放和服务器确定性验证，但不应等待所有客户端确认后才推进。
权威服务器如果等待客户端，任何弱网或恶意连接都能拖停整个房间。

## 4. 建议的代码模块

先拆库，再写网络。建议逐步调整为：

```text
src/domain/              纯仿真域：engine、unit、scenario、message bus
src/protocol/            envelope、命令 DTO、快照 DTO、版本兼容
src/client/              WebSocketClient、ClientStateStore、RemoteSessionAdapter
src/server/              main、SessionGateway、RoomManager、AuthPolicy
src/view/                SimulationController、QML 辅助对象
tests/unit/              快速单元测试
tests/integration/       真实 server + 多 client 测试
deploy/                  Containerfile、compose.yml、Caddyfile、systemd 备选
```

CMake 目标建议为：

```text
wargame_domain       -> Qt6::Core
wargame_protocol     -> Qt6::Core
wargame_clientlib    -> Qt6::Core Qt6::Network Qt6::WebSockets
wargame_serverlib    -> Qt6::Core Qt6::Network Qt6::WebSockets Qt6::Sql
appindex             -> domain + clientlib + Qt Quick
wargame_server       -> domain + protocol + serverlib，不链接 Qt Quick
wargame_tests
wargame_integration_tests
```

不要让服务器目标链接 `MapTileRenderer`、QML 或 `Qt6::Quick`。地图 PNG 由客户端包或静态 HTTPS
服务提供，服务器只保存地图 ID、版本、坐标边界和哈希。

## 5. 协议设计

### 5.1 首版传输

第一版使用 Qt WebSockets 的 WSS：

- 开发环境：`ws://127.0.0.1:8080/ws`。
- 生产环境：`wss://game.example.com/ws`。
- 首版使用 JSON，便于日志和抓包。
- 协议稳定并测出 JSON 是瓶颈后，再切换 CBOR；不要一开始同时维护两套编码。
- 单包上限建议 256 KiB，完整场景快照上限建议 8 MiB。

### 5.2 统一 envelope

每个包都包含：

```json
{
  "protocolVersion": 1,
  "type": "command",
  "sessionId": "01J...",
  "clientId": "01J...",
  "messageId": "01J...",
  "sequence": 123,
  "scenarioRevision": 7,
  "serverTick": 4200,
  "sentAt": "2026-07-17T12:00:00.000Z",
  "payload": {}
}
```

客户端上传的 `role`、`side` 只能作为显示信息，服务器必须以登录会话中保存的角色为准。

### 5.3 首版消息类型

| type | 方向 | 用途 |
|---|---|---|
| `hello` | 客户端 -> 服务端 | 协议版本、客户端版本、token、恢复游标 |
| `welcome` | 服务端 -> 客户端 | 身份、角色、房间、当前 tick、版本范围 |
| `snapshot` | 服务端 -> 客户端 | 角色裁剪后的完整状态 |
| `delta` | 服务端 -> 客户端 | 连续增量状态 |
| `command` | 客户端 -> 服务端 | 用户意图和唯一 `commandId` |
| `commandResult` | 服务端 -> 客户端 | accepted/rejected、错误码、对应 commandId |
| `event` | 服务端 -> 客户端 | 战报、提示、房间状态变化 |
| `resyncRequest` | 客户端 -> 服务端 | 发现序号缺口，请求完整快照 |
| `ping`/`pong` | 双向 | 保活和延迟测量 |
| `error` | 服务端 -> 客户端 | 协议级错误，严重时随后断开 |

命令示例：

```json
{
  "type": "command",
  "messageId": "msg-...",
  "payload": {
    "commandId": "cmd-...",
    "action": "moveTo",
    "args": {
      "unitId": "red_r1",
      "pos": {"x": 5000.0, "y": 6000.0}
    }
  }
}
```

结构化结果示例：

```json
{
  "type": "commandResult",
  "payload": {
    "commandId": "cmd-...",
    "accepted": false,
    "code": "UNIT_NOT_OWNED",
    "message": "不能控制其他阵营单元",
    "appliedAtTick": 4201
  }
}
```

错误码必须稳定，中文 `message` 可以变化。客户端逻辑判断错误码，不能解析中文字符串。

### 5.4 顺序、幂等和重连

- 每条命令带全局唯一 `commandId`，推荐 UUIDv7 或 ULID。
- 服务器按用户和房间保存最近 10 分钟的命令结果，重复命令直接返回旧结果。
- 服务器下行 `sequence` 严格递增。
- 客户端只应用 `sequence == lastSequence + 1` 的 delta。
- 发现缺口、revision 不一致或 delta 无法应用时，立即发 `resyncRequest`。
- 完整快照必须带 `scenarioRevision`、`serverTick`、`lastSequence` 和地图哈希。
- 心跳建议 15 秒，45 秒无 pong 断开。
- 重连退避建议 1、2、4、8、15、30 秒，并加入 0 至 20% 随机抖动。
- 客户端显示可以对位置保留 100 至 200 ms 插值缓冲，但命令状态必须以服务器回执为准。

## 6. 服务器权限矩阵

| 操作 | red | blue | director | editor | observer |
|---|---:|---:|---:|---:|---:|
| 查看己方状态 | 是 | 是 | 是 | 是 | 按配置 |
| 查看敌方未探测状态 | 否 | 否 | 是 | 是 | 否 |
| 指挥己方存活单元 | 是 | 是 | 是 | 否 | 否 |
| 控制对方单元 | 否 | 否 | 是 | 否 | 否 |
| 暂停/速率/单步 | 否 | 否 | 是 | 否 | 否 |
| 修改场景 | 否 | 否 | 可选 | 仅暂停时 | 否 |
| 创建/结束房间 | 否 | 否 | 是 | 可选 | 否 |

命令执行前按顺序检查：

1. 包结构、大小、字段类型和协议版本。
2. token 是否有效、是否过期、是否绑定当前房间。
3. `commandId` 是否已经处理。
4. 角色是否允许该 action。
5. 阵营是否拥有目标单元。
6. 单元是否存在、存活、可移动，参数是否有限且在地图边界内。
7. 场景 revision 是否匹配。
8. 仿真域规则是否允许，例如通信可达性。
9. 执行后写审计记录，再返回结构化结果。

## 7. 分阶段开发计划

### 阶段 0：冻结单机基线，约 2 至 4 天

任务：

- 保持全部现有单元测试通过，并在 CI 中执行 `ctest --output-on-failure`。
- 为 ASan/UBSan、`qmllint`、格式检查增加 CI job。
- 给场景和运行时快照增加 `schemaVersion`。
- 将 `SimulationEngine::command` 改为返回结构化 `CommandResult`。
- 把 action 和错误码定义为稳定常量，拒绝未知 action，不能静默忽略。
- 确认所有遍历顺序对结果无影响；有影响时排序或使用两阶段结算。
- 为固定种子、固定 tick 的回放建立 golden test。

验收：同一场景和同一命令日志运行 100 次，最终快照哈希一致；Debug、Release、ASan 均通过。

### 阶段 1：拆分 domain/client/server 库，约 3 至 5 天

任务：

- 从 `corelib` 移出 `src/view/`、`MapTileRenderer` 和 Qt Quick 依赖。
- 建立 `wargame_server` 空壳，支持 `--help`、`--version`、配置文件和优雅退出。
- 服务器仍使用 `SimulationEngine + LocalTransport`。
- 建立 `ISessionAdapter`，让 `SimulationController` 不再直接假设一定有本地 engine。
- 保留单机模式作为开发和离线演示模式。

验收：无显示服务器上运行 `wargame_server --check-config`，`ldd` 不出现 Qt Quick/QML/GUI。

### 阶段 2：协议编解码与内存回环，约 4 至 6 天

任务：

- 实现 envelope、消息类型、字段上限和 schema 校验。
- 实现 `ClientStateStore`，QML 从 store 读取不可变视图。
- 用内存 loopback 连接 `RemoteSessionAdapter` 与 `SessionGateway`，暂不打开 socket。
- 完成 hello/welcome、snapshot、command/commandResult、resync。
- 给协议做畸形 JSON、未知字段、旧版本、新版本和超大包测试。

验收：同一进程中客户端不持有权威 engine，仍可完成加载、移动、攻击、结束推演。

### 阶段 3：本机 WebSocket，约 4 至 7 天

任务：

- 加入 Qt WebSockets，监听 `127.0.0.1`，默认不暴露公网。
- Qt socket 是异步的，第一版可与单房间 engine 共用事件线程，避免不必要的线程锁。
- 固定 50 ms 服务器 tick；每 100 ms 下发一次状态 delta。
- 对每个连接设置接收包、发送队列、命令速率和空闲时间上限。
- 慢客户端发送队列超过 1 MiB 时先发错误再断开，不能拖慢房间。

验收：两个独立客户端进程连接同一服务器，红方命令只改变服务器状态，断开任一客户端不影响时钟。

### 阶段 4：身份、角色和视野裁剪，约 5 至 8 天

第一版私有部署可使用管理员预生成的 256 bit 随机 token：

- 数据库只保存 token 哈希和固定 pepper，不保存明文。
- token 绑定用户、角色、阵营、房间和过期时间。
- token 只能经 TLS 发送，客户端保存在系统密钥环，不写 `settings.json`。
- 后续有统一账号体系时再接 OIDC/JWT，不要自行设计密码哈希算法。

任务：

- 实现权限矩阵和服务器视野投影 `VisibleStateProjector`。
- 红蓝快照只包含己方完整状态和合法探测信息。
- 事件、消息日志、目标详情也必须经过同样的裁剪。
- 对越权命令和伪造 side/role 写安全审计。

验收：测试直接解析红方收到的原始 JSON，确认其中不存在未探测蓝方坐标、HP、计划或内部 ID。

### 阶段 5：房间、持久化和恢复，约 5 至 8 天

任务：

- SQLite 开启 WAL，保存用户、token 哈希、房间元数据、场景版本和审计事件。
- 每 10 秒或每 200 tick 写一次原子检查点；关键命令追加事件日志。
- 重启时读取最新检查点，再重放其后的命令。
- 房间级 `scenarioRevision` 每次编辑后递增。
- 推演运行时禁止编辑，或由导演显式暂停后编辑。
- 数据库和快照放持久卷，容器升级不能丢数据。

验收：强制终止服务器后重启，房间恢复到最后已确认 tick，命令不会重复执行。

### 阶段 6：弱网、压力和安全，约 5 至 10 天

测试矩阵：

- 延迟 50/200/500 ms，抖动 100 ms，断网 30 秒后恢复。
- 消息重复、重排、截断、超大 JSON、错误 UTF-8、未知 action。
- 32 个连接、500 单元、连续运行 8 小时。
- token 过期、撤销、跨房间使用、角色伪造、命令刷屏。
- 服务器重启、磁盘满、数据库只读、日志目录不可写。

验收建议：

- p95 命令回执小于 250 ms（同地域正常网络）。
- 单个 50 ms tick 的 p99 执行时间小于 25 ms。
- 500 单元完整角色快照压缩前小于 2 MiB。
- 8 小时无持续内存增长，ASan/UBSan 无报告。
- 未授权和视野泄漏测试全部通过。

当前进度：前述可在单进程/本机稳定复现的项目已进入 `wargame_network_tests`。仍需使用
`tc netem` 或等价网络故障注入执行延迟、抖动和 30 秒断网矩阵，并在独立 staging 主机执行
8 小时长稳、磁盘满/只读、真实 WSS、备份恢复及 p95/p99 采样；这些项目不能由短时 CI 替代。

### 阶段 7：预发布与正式上线，约 3 至 5 天

- 建立 `staging.game.example.com`，使用与生产相同的容器和 Caddy 配置。
- 完成一次从零部署、证书签发、备份、恢复、升级和回滚演练。
- 先做 2 至 4 人封闭测试，再逐步增加房间数。
- 上线后一周每天检查错误率、重连率、tick 延迟、磁盘和备份。

## 8. 新手本地开发流程

阶段 3 完成后，建议提供统一命令：

```bash
# 终端 1：构建
cmake -S . -B build -G Ninja \
  -DCMAKE_PREFIX_PATH=/path/to/Qt/6.10/gcc_64 \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel

# 终端 2：只监听本机，使用开发配置
./build/wargame_server \
  --config ./config/server.dev.json \
  --listen 127.0.0.1 \
  --port 8080

# 终端 3、4：分别启动红蓝客户端
WARGAME_TOKEN="$RED_TOKEN" ./build/appindex --server ws://127.0.0.1:8080/ws
WARGAME_TOKEN="$BLUE_TOKEN" ./build/appindex --server ws://127.0.0.1:8080/ws

# 测试
ctest --test-dir build --output-on-failure
./build/wargame_integration_tests --server ./build/wargame_server
```

`--server` 会预填客户端的联网连接表单；用户在表单中确认后才连接。token 通过环境变量传入，
不会写入客户端设置文件。

开发 token 只能用于本机配置，不能提交 Git。仓库应提供 `server.dev.example.json` 和 `.env.example`，
真实 `server.dev.json`、`.env`、数据库、证书、日志和快照必须加入 `.gitignore`。

## 9. 生产服务器部署方案

推荐“容器 + Docker Compose + Caddy”。Caddy 负责公网 443 和证书，游戏服务器只在容器网络中
监听 8080。不要把 8080 直接开放到公网。

### 9.1 准备云服务器和域名

1. 购买 Ubuntu 24.04 LTS，至少 2 vCPU、4 GB RAM、40 GB SSD。
2. 给服务器绑定固定公网 IP。
3. 在 DNS 添加 `game.example.com` 的 A/AAAA 记录。
4. 云厂商安全组只开放 22、80、443。22 最好只允许管理员固定 IP。
5. 等待 `nslookup game.example.com` 返回新服务器 IP 后再启动 Caddy。

### 9.2 基础加固

以下命令中的用户名和域名必须替换：

```bash
sudo apt update
sudo apt full-upgrade -y
sudo apt install -y docker.io docker-compose-v2 ufw unattended-upgrades

sudo adduser deploy
sudo usermod -aG docker deploy

sudo ufw default deny incoming
sudo ufw default allow outgoing
sudo ufw allow 22/tcp
sudo ufw allow 80/tcp
sudo ufw allow 443/tcp
sudo ufw enable
sudo ufw status verbose
```

先确认新用户能用 SSH key 登录，再考虑关闭 root 和密码登录。不要在尚未验证新会话时直接关闭
当前 SSH，否则很容易把自己锁在服务器外。

### 9.3 服务器目录

```bash
sudo mkdir -p /opt/wargame/{config,data,logs,backups}
sudo chown -R deploy:deploy /opt/wargame
sudo chmod 750 /opt/wargame
cd /opt/wargame
```

建议最终仓库提供：

```text
deploy/
  compose.yml
  Caddyfile
  server.prod.example.json
  backup.sh
  restore.sh
  smoke-test.sh
```

### 9.4 密钥和配置

生成 token pepper：

```bash
umask 077
openssl rand -hex 32
```

将结果写入 `/opt/wargame/.env`，权限必须是 600：

```dotenv
WARGAME_VERSION=1.0.0
WARGAME_PUBLIC_HOST=game.example.com
WARGAME_TOKEN_PEPPER=替换为64位十六进制随机值
```

```bash
chmod 600 /opt/wargame/.env
```

不要把 `.env` 放入镜像、日志、截图或 Git。生产配置应从环境变量读取 secret，普通限制项放
只读 JSON 配置。

### 9.5 Compose 模板

阶段 7 前应在仓库落地并测试类似配置：

```yaml
services:
  server:
    image: ghcr.io/your-org/wargame-server:${WARGAME_VERSION}
    restart: unless-stopped
    read_only: true
    environment:
      WARGAME_TOKEN_PEPPER: ${WARGAME_TOKEN_PEPPER}
      WARGAME_CONFIG: /app/config/server.json
    volumes:
      - ./config/server.prod.json:/app/config/server.json:ro
      - ./data:/app/data
      - ./logs:/app/logs
    expose:
      - "8080"
      - "9090"
    healthcheck:
      test: ["CMD", "/app/bin/wargame_server", "--health-probe", "http://127.0.0.1:9090/healthz"]
      interval: 15s
      timeout: 3s
      retries: 4
      start_period: 20s
    security_opt:
      - no-new-privileges:true
    cap_drop:
      - ALL
    logging:
      driver: local
      options:
        max-size: "20m"
        max-file: "5"

  caddy:
    image: caddy:2.10
    restart: unless-stopped
    depends_on:
      server:
        condition: service_healthy
    ports:
      - "80:80"
      - "443:443"
      - "443:443/udp"
    volumes:
      - ./config/Caddyfile:/etc/caddy/Caddyfile:ro
      - caddy_data:/data
      - caddy_config:/config
    security_opt:
      - no-new-privileges:true

volumes:
  caddy_data:
  caddy_config:
```

Caddyfile 示例：

```caddyfile
game.example.com {
    encode zstd gzip

    handle /ws* {
        reverse_proxy server:8080
    }

    handle /healthz {
        reverse_proxy server:9090
    }

    respond 404
}
```

生产健康接口只返回 `ok`、版本和是否可接流量，不返回房间、用户、token 或内部异常堆栈。

### 9.6 首次启动

```bash
cd /opt/wargame
docker compose --env-file .env -f config/compose.yml config
docker compose --env-file .env -f config/compose.yml pull
docker compose --env-file .env -f config/compose.yml up -d
docker compose --env-file .env -f config/compose.yml ps
docker compose --env-file .env -f config/compose.yml logs --tail=200 server

curl --fail https://game.example.com/healthz
```

之后用 staging 客户端连接 `wss://game.example.com/ws`，验证：登录、红蓝隔离、命令回执、
断线重连、推演结束、服务端重启恢复。

## 10. CI/CD 和镜像发布

推荐流水线：

1. 编译 Debug，运行单元测试和 `qmllint`。
2. 编译 ASan/UBSan，运行测试。
3. 编译 Release，运行集成测试和协议兼容测试。
4. 构建带固定版本号和 Git commit 标签的服务器镜像。
5. 生成 SBOM，扫描镜像高危漏洞。
6. 推送到容器仓库，但不自动覆盖生产。
7. staging 自动部署并运行 smoke test。
8. 人工批准后生产只更新 `.env` 中的不可变版本号。

不能使用 `latest` 作为生产唯一标签。至少保留当前版本和前两个已验证版本，便于快速回滚。

Qt 6.10 运行库应在构建镜像中固定补丁版本。服务器运行镜像只包含服务器二进制、所需 Qt Core/
Network/WebSockets/Sql 动态库、CA 证书和时区数据，不包含编译器、QML、地图母图和测试工具。

## 11. 监控、日志和告警

至少暴露以下指标：

- 当前连接数、房间数、每角色连接数。
- 命令 accepted/rejected 总数和按错误码计数。
- 命令排队、处理和回执延迟的 p50/p95/p99。
- tick 执行耗时、超时 tick、服务器事件循环延迟。
- snapshot/delta 字节数、发送队列大小、重同步次数。
- 登录失败、token 过期、限流和协议错误次数。
- 进程 CPU、内存、磁盘、数据库大小和备份时间。

日志使用结构化 JSON，并包含 `requestId`、`sessionId`、`clientId`、`commandId`、`serverTick`。
永远不要记录明文 token、完整 Authorization 头或用户输入中的秘密。

建议告警阈值：

- 健康检查连续 2 分钟失败。
- p99 tick 超过 50 ms 持续 5 分钟。
- 磁盘使用超过 75%，85% 升级为紧急。
- 5 分钟登录失败超过正常基线 5 倍。
- 15 分钟重同步率超过连接数的 20%。

## 12. 备份、恢复、升级和回滚

### 12.1 备份

- 每天备份 SQLite、房间检查点和场景文件。
- SQLite 使用 `.backup` API 或停写快照，不能直接复制正在写入的数据库文件。
- 备份先写临时文件，完成校验后原子改名。
- 本机保留 7 天，异地对象存储保留 30 天。
- 每个备份生成 SHA-256，并至少每月做一次真实恢复演练。

### 12.2 升级

```bash
cd /opt/wargame
# 先修改 .env 中 WARGAME_VERSION 为已在 staging 验证的固定版本
docker compose --env-file .env -f config/compose.yml pull server
docker compose --env-file .env -f config/compose.yml up -d server
docker compose --env-file .env -f config/compose.yml ps
curl --fail https://game.example.com/healthz
```

升级前先创建备份。涉及数据库 migration 时，migration 必须有版本号、可重复检测，并明确是否可回滚。

### 12.3 回滚

1. 将 `.env` 的 `WARGAME_VERSION` 改回上一个已验证版本。
2. `docker compose up -d server`。
3. 若新版本已执行不可逆数据库 migration，按发布说明恢复升级前备份。
4. 运行 smoke test，检查房间恢复、客户端协议和视野隔离。

## 13. 上线前最终清单

- [ ] 客户端不持有权威远程 `SimulationEngine`。
- [ ] WebSocket 网关没有替换仿真域 `MessageBus`。
- [ ] 红蓝原始网络包通过视野泄漏测试。
- [ ] 所有命令有稳定结果码、幂等 ID、权限和参数校验。
- [ ] WSS 证书有效，8080/9090 未暴露公网。
- [ ] token 仅保存哈希，客户端使用系统密钥环。
- [ ] 包大小、速率、发送队列、连接数和超时均有限制。
- [ ] 单元、协议、集成、弱网、压力、ASan/UBSan 测试通过。
- [ ] 固定版本镜像在 staging 完成重启恢复和回滚演练。
- [ ] 健康检查、指标、结构化日志和告警可用。
- [ ] 自动备份成功，并从备份恢复过一次。
- [ ] 运维人员持有部署、轮换 token、备份、恢复和事故处理文档。

## 14. 推荐实施顺序总结

不要从“先打开一个公网 WebSocket”开始。正确顺序是：

```text
确定规则
 -> 固化确定性和结构化命令结果
 -> 拆分无头 domain/server
 -> 内存回环跑通客户端状态仓库
 -> 本机 WebSocket
 -> 权限和服务器视野裁剪
 -> 持久化与重连
 -> 弱网/压力/安全测试
 -> staging
 -> 备份和回滚演练
 -> 小规模生产
```

按这个顺序，任何阶段失败都仍保留可运行的单机模式，也能在暴露公网前发现最昂贵的架构和
数据泄漏问题。
