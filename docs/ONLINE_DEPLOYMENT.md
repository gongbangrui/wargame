# 联网版本地部署

联网版由两个 Docker 服务组成：

- `account-web`：账号管理网页、管理员认证、兵棋账号认证和 SQLite 数据库。
- `game-server`：权威 `SimulationEngine`、房间/战位管理、视野裁剪、就绪与聊天，并提供 WebSocket 权威数据面。

两个端口只映射到本机回环地址：

- 账号管理平台：`http://localhost:8080`
- 推演网关：`ws://localhost:8090`；生产实时数据面使用按房间隔离的 WebSocket 会话。

## 0. 独立发布包（全新 Linux 主机）

发布包由外部发布渠道提供，仓库不内置下载地址。下载归档和同名校验文件后，先校验再解压：

```bash
archive="wargame-server-<version>.tar.gz"
sha256sum -c "$archive.sha256"
mkdir -p /tmp/wargame-release
tar -xzf "$archive" -C /tmp/wargame-release
cd /tmp/wargame-release/wargame-server-<version>
./deploy/install-server.sh --help
```

安装器会在调用用户的 `$HOME/wargame` 创建 `current/`、`.env`、`backups/` 和
`.wargame-install`。归档包含 `account-web` 和权威 `game-server` 的本地构建输入，
不需要源码检出或镜像仓库。首次安装和更新都应复用同一个 Compose 项目名：

```bash
sudo ./deploy/install-server.sh \
  --install-dir "$HOME/wargame" \
  --compose-project wargame
```

安装器会在终端安全提示输入管理员密码。密码至少 8 个字符且不设最大长度；非交互部署可使用
`--admin-password-stdin` 从权限受限的密钥文件或密钥管理器读取，密码不得作为命令行参数传入。

更新时对新归档重复校验、解压和安装；已有 `.env`、备份目录和
`WARGAME_DATA_VOLUME` 会保留。健康检查、重置和卸载从任意工作目录使用安装根：

```bash
install_root="$HOME/wargame"
sudo docker compose --project-name wargame --env-file "$install_root/.env" \
  -f "$install_root/current/deploy/compose.yml" ps
curl -fsS http://127.0.0.1:8080/api/health
sudo "$install_root/current/deploy/reset-room.sh" --yes
sudo "$install_root/current/deploy/reset-admin.sh"
sudo "$install_root/current/deploy/uninstall-server.sh" --yes
```

普通卸载保留配置和数据卷；只有显式提供 `--purge-data` 或 `--remove-config` 并确认后，
才会删除持久数据或 `.env`。

## 1. 一键安装（推荐）

将仓库复制到新的 Debian/Ubuntu、Rocky Linux 或 Fedora 服务器后，以 root 或 sudo 用户执行：

```bash
sudo ./deploy/install-server.sh
```

安装器会自动完成：

- 检查 Linux、CPU/内存/磁盘、Docker、Compose 和端口。
- 按操作系统安装 Docker、Compose、curl、OpenSSL 和端口检查工具。
- 生成内部 API 密钥、写入权限为 `600` 的 `.env`。
- 构建、启动并等待账号服务和 WebSocket 服务健康。
- 检查管理员登录、服务器监控概览和容器运维终端认证，输出管理网页和客户端连接地址。

脚本可重复执行，会备份旧 `.env`，不会删除 `wargame-data` 数据卷。每次执行默认会重置管理员密码；如需保留已有密码，使用：

```bash
sudo ./deploy/install-server.sh --reuse-admin-password
```

服务器需让其他计算机连接时，保持服务只绑定回环地址，并在前置反向代理终止 TLS：

```bash
sudo ./deploy/install-server.sh \
  --bind-address 127.0.0.1 \
  --public-host game.example.com \
  --public-game-ws-url wss://game.example.com
```

安装器拒绝非回环绑定。反向代理应将 HTTPS 管理网页转发到 `127.0.0.1:8080`、WSS
转发到 `127.0.0.1:8090`，并阻止外部直接访问这两个端口；否则管理员密码、玩家密码和
Bearer 会话会通过明文 HTTP 暴露。

终端与会话设置同样由该单一脚本写入 `.env`，无需手动修改 Compose 文件：

```bash
sudo ./deploy/install-server.sh \
  --session-hours 24 \
  --shell-session-seconds 1800 \
  --shell-ticket-seconds 180 \
  --shell-max-sessions 3 \
  --startup-timeout-seconds 180
```

若部署环境需要网页 Shell，显式追加 `--shell`；默认禁用 Shell。

`--public-host` 应填客户端能访问的服务器 IP 或域名。脚本默认使用 `8080` 和 `8090`，可通过 `--http-port` 和 `--ws-port` 更换。远程部署时只放行反向代理的 HTTPS/WSS 入口，不能放行回环端口。

脚本帮助：

```bash
./deploy/install-server.sh --help
```

## 2. 手动初始化配置

在项目根目录执行：

```bash
cp deploy/.env.example .env
```

编辑 `.env`，至少修改 `ADMIN_PASSWORD` 与 `INTERNAL_API_KEY`。`INTERNAL_API_KEY`
应使用足够长的随机字符串；它只用于两个容器之间的内部身份查询。

首次创建数据卷时，账号平台会使用 `.env` 中的 `ADMIN_USERNAME` 和
`ADMIN_PASSWORD` 建立管理员。已有数据卷不会被环境变量覆盖。

## 3. 启动服务器

下面的相对路径命令仅适用于仓库根目录的源码手动部署；已安装发布包请使用上文的
`$HOME/wargame/current` 绝对路径和固定 Compose 项目名。

```bash
docker compose --project-name wargame --env-file .env -f deploy/compose.yml up -d --build
```

`game-server` 镜像默认关闭 Fast DDS（`WARGAME_ENABLE_FASTDDS=OFF`、
`WARGAME_FASTDDS_MODE=disabled`）。即使构建阶段存在 SDK，运行时也会在认证、访问控制和
加密传输实现前拒绝兼容模式；WebSocket 仍是唯一权威数据面。

登录、房间目录、聊天、地图标记、snapshot、delta、command 和 resync 均通过已认证的
HTTPS/WebSocket 管线传输；DDS 未启用。

容器会创建持久卷 `wargame-data`，保存：

- `/data/wargame.db`：管理员、兵棋账号和登录会话。
- `/data/scenario.json`：联网房间当前初始场景。
- `/data/room-checkpoint.json`：包含推演时间、单位运行态、FSM、当前阶段和命令幂等记录的原子检查点。
- `/data/room-commands.jsonl`：关键命令的写前事件日志，用于重放尚未进入检查点的操作。

访问 `http://localhost:8080`，使用管理员账号登录。管理页面支持创建、修改、停用、
创建、修改、停用和删除普通联网账号，也支持修改管理员密码。账号不绑定阵营、导演或编辑身份，进入房间后再占用战位。
房间页支持设置战位容量、通信/侦察参数，以及开启、停止、重置和重新部署推演。
单个 `game-server` 实例只托管 `GAME_ROOM_ID`（默认 `main`）；其他网页创建的房间会出现在目录中，
但在对应服务器实例接入前不可加入或执行生命周期操作。

“服务器监控”页面仅向已登录管理员开放，提供：

- 账号服务与兵棋服务状态、当前阶段、推演时间、场景版本和连接数。
- 最近的 WebSocket 连接审计与消息流摘要，日志文件限制为约 1 MiB。
- 需再次确认管理员密码的真实 Shell，会以 `account-web` 容器内的非特权 `wargame` 用户运行。它不能访问宿主机、Docker 套接字或 `game-server` 容器；授权凭证只能使用一次且两分钟内失效，Shell 会话最长 15 分钟，单实例最多两个并发会话。

默认 Compose 部署和直接运行 `account-web` 都会禁用该入口。启用后这是高权限运维功能，
生产环境应仅通过 HTTPS 暴露管理网页，并将 `WEB_SHELL_ALLOWED_ORIGINS`
显式设为实际的 `https://主机[:端口]`。本地默认仅允许
`http://127.0.0.1:8080` 和 `http://localhost:8080`；缺失、格式错误或不匹配的
WebSocket Origin 会在一次性凭证被消费前拒绝。

账号服务将失败登录持久化到 SQLite：同一账号/IP 在 60 秒内最多 5 次，
同一 IP 跨账号最多 20 次。只有显式列入 `LOGIN_TRUSTED_PROXIES` 的直连反向代理
才能通过 `X-Forwarded-For` 提供客户端地址。阈值可向下调整，不能超过上述上限。

查看容器状态：

```bash
docker compose --project-name wargame --env-file .env -f deploy/compose.yml ps
docker compose --project-name wargame --env-file .env -f deploy/compose.yml logs -f account-web game-server
```

停止服务但保留账号和场景：

```bash
docker compose --project-name wargame --env-file .env -f deploy/compose.yml down
```

仅在确认需要清空全部账号与场景时删除数据卷：

```bash
docker compose --project-name wargame --env-file .env -f deploy/compose.yml down -v
```

## 4. 本机重置管理员密码

服务器运行时执行交互式重置：

```bash
./deploy/reset-admin.sh
```

非交互场景通过标准输入提供新密码（不要把密码作为命令行参数）：

```bash
read -r -s NEW_ADMIN_PASSWORD
printf '%s\n' "$NEW_ADMIN_PASSWORD" | ./deploy/reset-admin.sh
unset NEW_ADMIN_PASSWORD
```

重置会让现有管理员网页会话全部失效，但不会修改兵棋账号。
账号服务要求密码至少 8 个字符，不设最大长度、复杂度或黑名单规则；
所有供应的字符串仍使用 Argon2 哈希存储。

## 5. 构建桌面客户端

客户端在原有 Qt Quick 依赖之外需要 Qt 6 Network 和 Qt 6 WebSockets。仍使用项目根目录
的 CMake 构建，Qt 版本要求保持为 6.10 及以上：

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_PREFIX_PATH=/path/to/Qt/6.x/gcc_64 \
  -DCMAKE_BUILD_TYPE=Debug
ninja -C build appindex
./build/appindex
```

首次打开客户端会要求选择本地模式或联网模式。联网模式默认账号服务器为
`http://localhost:8080`，使用管理平台创建的普通联网账号登录。登录后先选择房间，再选择临时战位。
双方指挥官优先分配；普通用户可以选择攻击机、侦察机、地面单位或干扰机。指挥官在准备阶段部署友方单位并提交就绪，网页管理员确认后开启推演。

所有联网席位都可以使用顶部“通信”按钮进入实时文字频道。设置面板中的“运行模式”
可以重新打开模式选择界面，在本地与联网模式之间切换。

## 7. 联网冒烟验证

服务处于准备阶段且场景中红蓝双方各有一个指挥所时，可运行仓库内的联网冒烟脚本：

```bash
ADMIN_PASSWORD='管理员密码' node tools/network-smoke.mjs
```

脚本会临时创建普通账号，验证认证、房间、战位、权限拒绝、聊天、就绪、开局、
结束重置，以及重置后的准备阶段操作；成功或失败后均会删除临时账号。脚本不会在运行中
推演场景、任一方已经就绪、缺失任一方指挥所或非准备阶段时修改场景。

发布前的三个专项回归入口如下：独立账号生命周期测试为
`tests/test_account_room_lifecycle.py`，在仓库根目录执行
`uv run --with-requirements server/account/requirements.txt python tests/test_account_room_lifecycle.py`；
房间托管契约脚本为 `tools/room-hosting-contract.mjs`，在服务运行且管理员密码已设置时执行
`ADMIN_PASSWORD='管理员密码' node tools/room-hosting-contract.mjs`；权威房间测试源文件为
`tests/test_authoritative_room.cpp`，CMake 目标为 `authoritative_room_tests`，执行
`cmake --build build/debug --target authoritative_room_tests && build/debug/authoritative_room_tests --gtest_color=no`。

## 6. 权限与数据边界

- 账号会话只证明用户身份；账号不带阵营或管理角色，战位由房间服务器分配，不接受客户端上传的阵营或战位权限。
- 红蓝客户端只收到己方单位和服务端判定为已探测的敌方单位。
- 单位控制、目标可见性、地图边界、场景编辑阶段和阵营归属均由推演服务器再次校验。
- 推演服务器是唯一推进 50ms 仿真 tick 的节点，客户端不在联网模式自行推进时间。
- 房间实时数据使用按房间和战位隔离的 WebSocket 会话（`SeatSnapshot`、`SeatDelta`、`SeatCommand`、`ChatMessage` 等），并共用同一套服务器权限、序列号和视野裁剪。
- 联网协议当前为 v3/schema 2；客户端使用完整快照建立基线，之后应用带状态版本的增量，发现序号缺口时自动请求完整同步。
- 每次初始场景修改都会清除双方就绪状态。
- 网页管理员停止或重置推演后，事件时间归零，单位恢复到本轮开局时的位置和参数；“重新部署”会清除全部战位的部署结果并重新弹出指挥官部署流程。
- 服务器每 10 秒及关键操作后写入检查点；正常停止时再写一次。检查点损坏或事件日志无法严格重放时，服务端拒绝监听，避免静默回退到错误战局。
