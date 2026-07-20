# 联网版本地部署

联网版由两个 Docker 服务组成：

- `account-web`：账号管理网页、管理员认证、兵棋账号认证和 SQLite 数据库。
- `game-server`：权威 `SimulationEngine`、WebSocket 房间、角色权限、视野裁剪、就绪与聊天。

两个端口只映射到本机回环地址：

- 账号管理平台：`http://localhost:8080`
- 推演 WebSocket：`ws://localhost:8090`

## 1. 一键安装（推荐）

将仓库复制到新的 Debian/Ubuntu、Rocky Linux 或 Fedora 服务器后，以 root 或 sudo 用户执行：

```bash
sudo ./deploy/install-server.sh --admin-password '设置一个强管理员密码'
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

服务器需让其他计算机连接时，指定对外地址：

```bash
sudo ./deploy/install-server.sh \
  --bind-address 0.0.0.0 \
  --public-host 192.168.1.20 \
  --admin-password '设置一个强管理员密码'
```

终端与会话设置同样由该单一脚本写入 `.env`，无需手动修改 Compose 文件：

```bash
sudo ./deploy/install-server.sh \
  --admin-password '设置一个强管理员密码' \
  --session-hours 24 \
  --shell-session-seconds 1800 \
  --shell-ticket-seconds 180 \
  --shell-max-sessions 3 \
  --startup-timeout-seconds 180
```

若部署环境不需要网页 Shell，追加 `--no-shell`。默认启用 Shell。

`--public-host` 应填客户端能访问的服务器 IP 或域名。脚本默认使用 `8080` 和 `8090`，可通过 `--http-port` 和 `--ws-port` 更换。远程部署时还需按服务器防火墙放行这两个端口。

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

```bash
docker compose -f deploy/compose.yml up -d --build
```

容器会创建持久卷 `wargame-data`，保存：

- `/data/wargame.db`：管理员、兵棋账号和登录会话。
- `/data/scenario.json`：联网房间当前初始场景。
- `/data/room-checkpoint.json`：包含推演时间、单位运行态、FSM、当前阶段和命令幂等记录的原子检查点。
- `/data/room-commands.jsonl`：关键命令的写前事件日志，用于重放尚未进入检查点的操作。

访问 `http://localhost:8080`，使用管理员账号登录。管理页面支持创建、修改、停用、
删除导演席、编辑席、红方和蓝方账号，也支持修改管理员密码。

“服务器监控”页面仅向已登录管理员开放，提供：

- 账号服务与兵棋服务状态、当前阶段、推演时间、场景版本和连接数。
- 最近的 WebSocket 连接审计与消息流摘要，日志文件限制为约 1 MiB。
- 需再次确认管理员密码的真实 Shell，会以 `account-web` 容器内的非特权 `wargame` 用户运行。它不能访问宿主机、Docker 套接字或 `game-server` 容器；授权凭证只能使用一次且两分钟内失效，Shell 会话最长 15 分钟，单实例最多两个并发会话。

默认 Compose 部署会启用该入口。若自行运行 `account-web`，需显式设置
`WEB_SHELL_ENABLED=true`；生产环境应仅通过 HTTPS 暴露管理网页。

查看容器状态：

```bash
docker compose -f deploy/compose.yml ps
docker compose -f deploy/compose.yml logs -f account-web game-server
```

停止服务但保留账号和场景：

```bash
docker compose -f deploy/compose.yml down
```

仅在确认需要清空全部账号与场景时删除数据卷：

```bash
docker compose -f deploy/compose.yml down -v
```

## 4. 本机重置管理员密码

服务器运行时执行交互式重置：

```bash
./deploy/reset-admin.sh
```

也可以直接提供新密码：

```bash
./deploy/reset-admin.sh 'NewAdminPassword!'
```

重置会让现有管理员网页会话全部失效，但不会修改兵棋账号。

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
`http://localhost:8080`，使用管理平台创建的兵棋账号登录。登录后页面由服务端角色固定：

- 编辑席：准备阶段编辑红蓝双方初始单位，开局后只读。
- 红方/蓝方：准备阶段编辑己方阵容并提交就绪，开局后指挥己方单位。
- 导演席：查看全局态势，双方就绪后开局，可暂停、继续、调速和结束重置。

所有联网席位都可以使用顶部“通信”按钮进入实时文字频道。设置面板中的“运行模式”
可以重新打开模式选择界面，在本地与联网模式之间切换。

## 7. 联网冒烟验证

服务处于准备阶段且场景中红蓝双方各有一个指挥所时，可运行仓库内的联网冒烟脚本：

```bash
ADMIN_PASSWORD='管理员密码' node tools/network-smoke.mjs
```

脚本会临时创建导演、编辑、红方和蓝方账号，验证认证、权限拒绝、聊天、就绪、开局、
结束重置，以及重置后的准备阶段编辑；成功或失败后均会删除临时账号。脚本不会在运行中
推演场景、任一方已经就绪、缺失任一方指挥所或非准备阶段时修改场景。

## 6. 权限与数据边界

- 账号角色只取自账号服务签发的服务器会话，不接受客户端上传的角色或阵营。
- 红蓝客户端只收到己方单位和服务端判定为已探测的敌方单位。
- 单位控制、目标可见性、地图边界、场景编辑阶段和阵营归属均由推演服务器再次校验。
- 推演服务器是唯一推进 50ms 仿真 tick 的节点，客户端不在联网模式自行推进时间。
- 联网协议当前为 v2；客户端使用完整快照建立基线，之后应用带状态版本的增量，发现序号缺口时自动请求完整同步。
- 每次初始场景修改都会清除双方就绪状态。
- 导演结束推演后，事件时间归零，单位恢复到本轮开局时的位置和参数。
- 服务器每 10 秒及关键操作后写入检查点；正常停止时再写一次。检查点损坏或事件日志无法严格重放时，服务端拒绝监听，避免静默回退到错误战局。
