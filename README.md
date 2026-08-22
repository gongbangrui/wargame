# 兵棋推演（Wargame）

兵棋推演是一个基于 Qt 6/QML 和 C++ 的兵棋仿真项目，提供本地桌面推演、场景编辑、单位指挥、导演席观察，以及可选的权威服务器联网模式。

项目的仿真核心由 C++ 驱动，默认以 50 ms 为一个仿真 tick。QML 只通过 `SimulationController` 与核心交互，服务器模式下由无界面的权威服务器推进房间状态，客户端只提交命令并接收裁剪后的状态。

## 功能概览

- 本地 Qt Quick 桌面客户端。
- 场景编辑、单位编组与红蓝双方阵容配置。
- 本地模式保留编辑、红方、蓝方和导演视图；联网模式使用独立战位视角。
- 五种单位：指挥所、侦察无人机、攻击无人机、地面侦察单位、干扰无人机。
- 移动单位 FSM：待机、移动、巡逻、撤退。
- 50 ms 固定仿真 tick、单位探测、定向通信距离/中继和 ECM 干扰。
- 本地模式与账号认证后的联网推演模式。
- 权威服务器、账号管理、房间生命周期、战位分配、独立视野、定向情报共享和断线重连。
- WebSocket 权威数据面：按房间和战位隔离会话，协议和权限模型由游戏服务统一执行。
- Fast DDS 兼容适配保留为后续安全传输实现的接口，当前默认关闭；所有登录、房间目录、状态和命令均走 WebSocket/HTTPS 权威数据面。
- 检查点、事件日志、状态快照与增量同步。
- 内置 GIS 瓦片资源和地图元数据。

## 技术栈与要求

- C++20
- Qt 6.10 或更高版本
- Qt Quick、Qt Quick Controls 2、Qt Network、Qt WebSockets
- CMake 3.25 或更高版本
- Ninja
- Python 3（账号服务及地图工具）
- tinyxml2 开发包（构建 `design/EncoderDecoder` 的 VMF CLI；缺少时 CLI 会明确显示 disabled）
- Docker Engine 与 Docker Compose（联网部署时需要）

项目统一使用 CMake Presets 与 Ninja 构建。

Fast DDS 集成需要同时安装 SDK 和 `fastddsgen`。默认构建和运行时都关闭 DDS；在认证、访问控制和加密配置完成前，设置兼容模式也会被服务端拒绝。客户端和服务器继续使用 WebSocket 权威数据面。

## 快速开始：桌面客户端

先确认 Qt 的 CMake 配置能够被找到，然后执行：

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_PREFIX_PATH=/path/to/Qt/6.10.x/gcc_64 \
  -DCMAKE_BUILD_TYPE=Debug

ninja -C build appindex
./build/appindex
```

也可以使用项目提供的 CMake Presets：

```bash
cmake --preset debug
cmake --build --preset debug
```

默认构建还会生成 `vmf_encode`、`vmf_decode`、`vmf_validate` 和 `xml_compare`。发布前可运行
设计目录的真实 XML/bit 回归管线：

```bash
cmake --build build/debug --target vmf_design_regression
```

只构建客户端或服务器且未安装 tinyxml2 时，可设置
`-DWARGAME_BUILD_VMF_TOOLS=OFF`；字典仍作为运行时 profile 安装，CLI 回归则标记为 disabled。

客户端可在启动后选择本地模式或联网模式。默认情况下，联网模式连接本机的账号服务 `http://localhost:8080` 和推演 WebSocket 服务 `ws://localhost:8090`。

## 质量门禁与恢复验证

CI 对每次提交执行 Debug/ASan-UBSan 构建、QML lint、源码格式检查，以及隔离 Docker 卷中的联网冒烟和恢复演练。本地可执行相同检查：

```bash
cmake --preset debug
cmake --preset sanitizers
cmake --build --preset debug
cmake --build --preset sanitizers
ctest --test-dir build/debug --output-on-failure
ctest --test-dir build/sanitizers --output-on-failure
cmake --build build/debug --target all_qmllint
./tools/check-source-format.sh
./tools/verify-docker-recovery.sh
```

恢复脚本会创建独立 Docker 项目、卷和临时账号，验证联网冒烟、`SIGTERM` 最终检查点、
数据卷归档、还原卷和服务恢复；结束后自动清理测试资源。它默认使用本机 `18080/18090`
及 `18081/18091` 端口，可通过 `RECOVERY_HTTP_PORT` 和 `RECOVERY_WS_PORT` 覆盖。
正式发布、人工验收和回滚步骤见 [`docs/RELEASE.md`](docs/RELEASE.md)。

## 联网服务器

联网模式由两个服务组成：

- `account-web`：账号管理、管理员认证、房间配置和 SQLite 数据。
- `game-server`：权威 `SimulationEngine`、房间/战位管理、视野裁剪和 WebSocket 数据面节点。

发布时使用 `WARGAME_VERSION=2.0.0 ./tools/build-release.sh --clean` 生成统一的桌面客户端、
根项目服务端和 Qt 6.4 独立服务端身份；服务器一键包随后由
`WARGAME_VERSION=2.0.0 DIST_DIR=dist ./deploy/package-one-click.sh` 生成。包内身份文件会在
全新主机安装前校验 v8/schema 8 和源码摘要，避免客户端与两个服务端错配；普通房间仍可按
协商结果兼容 v7/v6/v4，严格 VMF 房间只接受 v8/schema 8。

### 下载发布包部署

发布包和 `.sha256` 校验文件由外部发布渠道提供。全新 Linux 主机先执行校验和解压，
再从解压目录运行安装器；安装器会在调用用户的 `$HOME/wargame` 构建两个服务：

```bash
sha256sum -c wargame-server-<version>.tar.gz.sha256
tar -xzf wargame-server-<version>.tar.gz -C /tmp
cd /tmp/wargame-server-<version>
sudo ./deploy/install-server.sh --install-dir "$HOME/wargame" --compose-project wargame
```

更新时保持同一个安装根和 Compose 项目名。使用
`$HOME/wargame/current/deploy/compose.yml`、`.env` 和已安装的 reset/uninstall helpers；
普通更新与卸载保留 `.env`、备份和命名 Docker 数据卷。

推荐使用安装脚本部署：

```bash
sudo ./deploy/install-server.sh \
  --install-dir "$HOME/wargame"
```

服务端可用 `./build/debug/server/wargame_server --version` 确认内嵌版本；Docker 镜像也使用
`.env` 中的 `WARGAME_VERSION` 标记。

需要被局域网或其他客户端访问时，请把回环端口放在 HTTPS/WSS 反向代理后：

```bash
sudo ./deploy/install-server.sh \
  --bind-address 127.0.0.1 \
  --public-host game.example.com \
  --public-game-ws-url wss://game.example.com
```

安装器拒绝非回环绑定，避免直接以明文 HTTP 暴露管理员登录、玩家登录和 Bearer 会话；反向代理必须终止 TLS，并且防火墙不得放行 8080/8090。

也可以手动启动：

以下命令仅适用于仓库根目录的源码手动部署；已安装发布包请使用安装器和绝对路径命令。

```bash
cp deploy/.env.example .env
# 编辑 .env，至少修改 ADMIN_PASSWORD 和 INTERNAL_API_KEY
docker compose --project-name wargame --env-file .env -f deploy/compose.yml up -d --build
docker compose --project-name wargame --env-file .env -f deploy/compose.yml ps
```

Docker 构建默认关闭 Fast DDS 适配（`WARGAME_ENABLE_FASTDDS=OFF`，`WARGAME_FASTDDS_MODE=disabled`）。即使构建了可选 SDK，运行时也会在安全认证与加密传输实现前拒绝兼容模式；WebSocket 仍是 snapshot/delta/command 的唯一权威通道。

账号管理页面默认地址为 `http://localhost:8080`，推演 WebSocket 默认地址为 `ws://localhost:8090`。生产环境应在反向代理后使用 HTTPS/WSS；8080/8090 必须保持仅回环可达，防火墙只需放行反向代理的 TLS 入口。

查看日志：

```bash
docker compose --project-name wargame --env-file .env -f deploy/compose.yml logs -f account-web game-server
```

停止服务但保留数据：

```bash
docker compose --project-name wargame --env-file .env -f deploy/compose.yml down
```

删除数据卷前请确认不再需要账号、场景和检查点：

```bash
docker compose --project-name wargame --env-file .env -f deploy/compose.yml down -v
```

更多部署、账号重置、联网战位和数据恢复说明见 [`docs/ONLINE_DEPLOYMENT.md`](docs/ONLINE_DEPLOYMENT.md)。

## 联网账号与战位

账号只表示可以登录联网服务，不绑定阵营、导演或编辑身份。同一账号只能登录一个客户端；进入房间后由服务器分配临时战位。
双方指挥官优先占用；若一方指挥官尚未占用，新用户必须先选择该指挥官。双方指挥官就绪后由网页管理员开启推演。其他用户可以选择攻击机、侦察机、地面单位或干扰机，数量、通信范围和侦察范围由房间配置决定。

每个战位拥有独立知识库。服务器按照发送方通信范围生成有向通信链，情报必须由用户手动指定接收战位，未共享的信息不会自动出现在同阵营客户端。

红蓝客户端不会收到尚未探测到的敌方单位完整状态。服务器在序列化状态前执行权限和视野裁剪，而不是把全图发送给客户端后再由 QML 隐藏。

## 核心架构

```text
QML
  |
SimulationController
  |-- 本地模式：SimulationEngine
  `-- 联网模式：NetworkClient + ClientStateStore

本地模式：
SimulationEngine -> MessageBus -> UnitBase / UnitFsm

联网模式：
客户端 HTTPS 登录/房间目录 <-> account-web
客户端 WebSocket 权威数据面 <-> game-server <-> SimulationEngine
                              |
                         account-web
```

主要模块：

| 路径 | 内容 |
| --- | --- |
| `src/core/` | 仿真引擎、消息总线、场景、地图、快照和时钟 |
| `src/units/` | 指挥所、无人机、地面单位和移动单位基类 |
| `src/protocol/` | 网络协议、状态快照和增量 |
| `src/network/` | 网络客户端和客户端状态存储 |
| `src/view/` | C++/QML 桥接、场景编辑和地图瓦片渲染 |
| `qml/` | QML 根视图、指挥席、导演席和可复用组件 |
| `server/` | 无头权威游戏服务器与账号服务 |
| `deploy/` | Docker Compose、镜像和服务器安装脚本 |
| `tests/` | 单元、协议、服务器、快照和联网相关测试 |
| `map/` | GIS 瓦片、地图元数据和地图生成工具 |
| `docs/` | 联网架构、部署和验证文档 |

## 重要规则

- C++ 类统一位于 `gbr` 命名空间。
- QML 与 C++ 的唯一桥接对象是 `controller`，QML 不直接访问引擎或消息总线。
- 所有 QML 命令通过 `controller.command(action, args)` 进入仿真引擎。
- 每个阵营必须恰好有一个存活的指挥所，否则场景不能开始推演并会自动暂停。
- 指挥所 ID 应使用 `red_cp` 和 `blue_cp`，以保证命令发送者解析稳定。
- 移动单位通过 `UnitFsm` 管理状态，新增移动单位应在构造函数调用 `setupFsm()`，并在 `onTick()` 中委托给 FSM。
- 服务器是联网模式唯一的权威仿真节点；客户端不推进联网仿真时间。
- `.env`、数据库、事件日志、检查点和构建目录不会被提交，敏感配置不得放入源码或 Git。

## 网络验证

服务器进入准备阶段且红蓝双方各有一个指挥所后，可运行联网冒烟测试：

```bash
ADMIN_PASSWORD='管理员密码' node tools/network-smoke.mjs
```

该脚本会临时创建测试账号，验证认证、战位权限、定向通信、聊天、双方就绪、管理员开局、结束重置和准备阶段编辑，并在结束后删除测试账号。

## 地图资源

`map/` 保存运行时使用的瓦片和元数据。CMake 会在构建时将 `map/12/*.png`、`metadata.json` 和 `tilejson.json` staged 到构建目录。地图制作相关脚本和说明见：

- [`map/README.md`](map/README.md)
- [`map/tools/build_tiles.py`](map/tools/build_tiles.py)
- [`map/MANIFEST.sha256`](map/MANIFEST.sha256)

## 安全提示

- 不要提交 `.env`、管理员密码、内部 API 密钥、数据库、检查点或日志。
- 生产环境请使用强密码、随机内部密钥和 HTTPS/WSS。
- 账号管理页面和 WebSocket 端口应只开放给必要的网络范围。
- GitHub Personal Access Token 不应出现在截图、聊天记录、提交内容或命令历史中；如果泄露，应立即撤销并重新生成。
