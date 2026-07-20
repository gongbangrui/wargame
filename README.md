# 兵器推演（Wargame）

兵器推演是一个基于 Qt 6/QML 和 C++ 的兵棋仿真项目，提供本地桌面推演、场景编辑、单位指挥、导演席观察，以及可选的权威服务器联网模式。

项目的仿真核心由 C++ 驱动，默认以 50 ms 为一个仿真 tick。QML 只通过 `SimulationController` 与核心交互，服务器模式下由无界面的权威服务器推进房间状态，客户端只提交命令并接收裁剪后的状态。

## 功能概览

- 本地 Qt Quick 桌面客户端。
- 场景编辑、单位编组与红蓝双方阵容配置。
- 导演席、红方指挥席、蓝方指挥席和编辑席四种视图模式。
- 五种单位：指挥所、侦察无人机、攻击无人机、地面侦察单位、干扰无人机。
- 移动单位 FSM：待机、移动、巡逻、撤退。
- 50 ms 固定仿真 tick、单位探测、通信距离、指挥所通信旁路和 ECM 干扰。
- 本地模式与 WebSocket 联网模式。
- 权威服务器、账号管理、角色权限、房间状态、聊天、就绪控制和断线重连。
- 检查点、事件日志、状态快照与增量同步。
- GoogleTest 单元测试、协议测试、快照测试、锁步测试和服务器状态测试。
- 内置 GIS 瓦片资源和地图元数据。

## 技术栈与要求

- C++20
- Qt 6.10 或更高版本
- Qt Quick、Qt Quick Controls 2、Qt Network、Qt WebSockets
- CMake 3.22 或更高版本
- Ninja
- Python 3（账号服务及地图工具）
- Docker Engine 与 Docker Compose（联网部署时需要）

GoogleTest 在 CMake 配置阶段通过 `FetchContent` 获取，不需要手动安装。项目使用 CMake 构建；仓库中的 qmake 相关文件仅用于兼容旧环境，不作为主构建入口。

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
ctest --preset debug
```

客户端可在启动后选择本地模式或联网模式。默认情况下，联网模式连接本机的账号服务 `http://localhost:8080` 和推演 WebSocket 服务 `ws://localhost:8090`。

## 测试

Debug 构建默认启用测试：

```bash
cmake --preset debug
cmake --build --preset debug --target wargame_tests
ctest --preset debug
```

直接运行测试程序：

```bash
./build/debug/wargame_tests
```

启用 AddressSanitizer 和 UndefinedBehaviorSanitizer：

```bash
cmake --preset sanitizers
cmake --build --preset sanitizers
ctest --preset sanitizers
```

## 联网服务器

联网模式由两个服务组成：

- `account-web`：账号管理、管理员认证、角色账号和 SQLite 数据。
- `game-server`：权威 `SimulationEngine`、WebSocket 房间、权限检查、视野裁剪、聊天和检查点。

推荐使用安装脚本部署：

```bash
sudo ./deploy/install-server.sh \
  --admin-password '设置一个强管理员密码'
```

需要被局域网或其他客户端访问时：

```bash
sudo ./deploy/install-server.sh \
  --bind-address 0.0.0.0 \
  --public-host 192.168.1.20 \
  --admin-password '设置一个强管理员密码'
```

也可以手动启动：

```bash
cp deploy/.env.example .env
# 编辑 .env，至少修改 ADMIN_PASSWORD 和 INTERNAL_API_KEY
docker compose -f deploy/compose.yml up -d --build
docker compose -f deploy/compose.yml ps
```

账号管理页面默认地址为 `http://localhost:8080`，推演 WebSocket 默认地址为 `ws://localhost:8090`。生产环境应在反向代理后使用 HTTPS/WSS，并根据实际网络配置防火墙和 `--public-host`。

查看日志：

```bash
docker compose -f deploy/compose.yml logs -f account-web game-server
```

停止服务但保留数据：

```bash
docker compose -f deploy/compose.yml down
```

删除数据卷前请确认不再需要账号、场景和检查点：

```bash
docker compose -f deploy/compose.yml down -v
```

更多部署、账号重置、联网角色和数据恢复说明见 [`docs/ONLINE_DEPLOYMENT.md`](docs/ONLINE_DEPLOYMENT.md)。

## 联网角色

服务器端绑定账号角色，不信任客户端自行提交的角色或阵营：

| 角色 | 主要权限 |
| --- | --- |
| 编辑席 | 准备阶段编辑红蓝双方初始场景 |
| 红方 | 查看己方态势并指挥红方单位 |
| 蓝方 | 查看己方态势并指挥蓝方单位 |
| 导演席 | 查看全局态势、控制开始/暂停/速度和结束推演 |

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
客户端 WebSocket <-> game-server <-> SimulationEngine
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

该脚本会临时创建测试账号，验证认证、角色权限、聊天、就绪、开局、结束重置和准备阶段编辑，并在结束后删除测试账号。

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

## 许可证

当前仓库未声明开源许可证。除非项目维护者另行说明，使用、复制或重新发布前请先取得作者许可。
