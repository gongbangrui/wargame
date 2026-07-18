# P0 联网实现与首次部署

本文说明仓库中已经可用的联网代码。完整设计理由和生产运维背景见
`docs/NETWORKING_PLAN.md`。

## 1. 已实现的边界

- `wargame_domain`：权威仿真，仅依赖 Qt Core。
- `wargame_protocol`：版本化 JSON envelope、严格字段和大小校验。
- `wargame_clientlib`：`ClientStateStore`、本地/远程会话适配器、WebSocket 重连和重同步。
- `wargame_serverlib`：token 认证、权限、视野投影、房间、网关、限流、SQLite 和指标。
- `wargame_server`：不链接 Qt Quick/QML/GUI 的无头程序。
- `appindex`：启动时可选择单机或联网模式；传入 `--server` 会预填连接表单，用户确认后才显示服务器投影状态。

服务器是唯一权威节点。`MessageBus/LocalTransport` 仍只负责仿真域内通信规则，WebSocket
位于引擎外层。红蓝双方收不到未探测敌方单元；已探测敌方也不包含 HP、计划和共享知识。

## 2. 本机构建

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_PREFIX_PATH=/path/to/Qt/6.10/gcc_64 \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

确认服务器没有 GUI 依赖：

```bash
ldd build/wargame_server | grep -E 'Qt6(Quick|Qml|Gui)'
```

正常结果为空。

## 3. 创建本机配置、token 和管理员

不要把 token、pepper 或真实配置提交 Git。

```bash
cp config/server.dev.example.json config/server.dev.json
export WARGAME_TOKEN_PEPPER="$(openssl rand -hex 32)"
read -rsp '输入至少 32 字符的客户端 token: ' WARGAME_TOKEN; echo
TOKEN_HASH="$(printf '%s\n' "$WARGAME_TOKEN" | \
  ./build/wargame_server --hash-token-stdin)"
echo "$TOKEN_HASH"
```

把输出的 64 位 `TOKEN_HASH` 填到 `config/server.dev.json` 的 `tokenHash`。`role` 可为：

- `red`：只能看到并指挥红方。
- `blue`：只能看到并指挥蓝方。
- `director`：全图和推演控制。
- `editor`：全图；只能在暂停时替换场景。
- `observer`：默认不接收单元状态，只有地图和房间状态。

每个 token 还固定绑定 `userId`、`roomId` 和过期时间。服务端不保存明文 token。

管理员平台默认只监听 `127.0.0.1:9091`，通过 `/admin` 访问。先生成管理员密码哈希：

```bash
read -rsp '输入管理员密码（至少 8 位）: ' ADMIN_PASSWORD; echo
ADMIN_HASH="$(printf '%s\n' "$ADMIN_PASSWORD" | \
  ./build/wargame_server --hash-password-stdin)"
unset ADMIN_PASSWORD
```

将 `ADMIN_HASH` 中的 `salt` 和 `passwordHash` 填入 `config/server.dev.json` 的 `admin` 对象。生产环境
应通过 Caddy 的 HTTPS `/admin` 反向代理访问，不要直接把 9091 暴露到公网。登录后可以创建、禁用、
启用、重置密码和删除账号。账号的角色、阵营、房间和过期时间由服务器保存，客户端不能自行修改。

管理员密码轮换方式：重新执行 `--hash-password-stdin`，替换 `admin.salt` 与 `admin.passwordHash`，
然后滚动重启服务器。该操作会使已有管理平台会话失效，不影响游戏账号或 Token。

创建账号时，`red`/`blue` 角色必须绑定同名阵营，其他角色不能绑定阵营；账号密码不写入客户端设置。

校验并启动：

```bash
./build/wargame_server --config config/server.dev.json --check-config
./build/wargame_server --config config/server.dev.json
```

另开终端启动客户端。token 只放当前进程环境，不写设置文件：

```bash
export WARGAME_TOKEN='刚才输入的明文 token'
./build/appindex --server ws://127.0.0.1:8080/ws
```

客户端会打开联网连接表单并预填服务器地址；确认“连接”后才会发送 token。token 只保留在当前
进程内用于认证和自动重连，设置中只会保存用户选择的服务器地址。

健康和指标：

```bash
curl --fail http://127.0.0.1:9090/healthz
curl --fail http://127.0.0.1:9090/metrics
```

## 4. 协议和安全行为

- 客户端先发 `hello`，服务器返回 `welcome` 和角色裁剪后的 `snapshot`；随后每 100 ms
  发送连续 `delta`，只有首次连接、版本变化或重同步才重发完整快照。
- 所有 envelope 带 `protocolVersion`、`messageId`、`sequence`、`scenarioRevision` 和 `serverTick`。
- 每条命令带唯一 `commandId`；10 分钟内重复命令直接返回原结果，不重复执行。客户端断线后
  会用原 ID 重发尚未收到回执的命令，且服务端在限流前查询幂等结果。
- revision 或 sequence 不连续时，客户端请求完整重同步。
- 断线按 1、2、4、8、15、30 秒加随机抖动重连；15 秒心跳，45 秒超时。
- 单包默认 256 KiB，快照 8 MiB，发送队列 1 MiB；每连接消息 100/s、突发 200，
  命令 20/s、突发 40，完整重同步最多每秒一次。
- 三次协议错误、认证超时、心跳超时或慢客户端会被断开。
- WebSocket 只接受 `/ws`；hello、command、resync 和心跳 payload 拒绝未知字段。
- 认证/协议配置错误不会无限重连；可恢复断线仍使用带抖动的指数退避。
- 权限和阵营从服务器 token 会话读取，忽略客户端伪造的角色或阵营。

生产必须由 Caddy 提供 WSS。不要直接把 8080 或 9090 暴露到公网。

## 5. 持久化、备份和恢复

SQLite 使用 WAL 和 `synchronous=FULL`，保存：

- token 哈希和绑定身份；
- 每 10 秒生成的场景/运行时检查点；
- 命令审计；
- 10 分钟幂等命令结果。

服务器启动时自动恢复最新可解析检查点；最新记录损坏时回退到上一条有效记录，全部损坏则
拒绝启动，不会静默加载默认场景。恢复前会完整校验单元、tick、HP、位置和计划，正常退出前
再写一次检查点。

一致性备份：

```bash
./build/wargame_server --config config/server.dev.json \
  --backup backups/manual.sqlite3
sha256sum backups/manual.sqlite3
```

容器部署使用 `deploy/backup.sh` 和 `deploy/restore.sh`。恢复时脚本先停服务器、保留恢复前
数据库，再替换并重启。每次升级前必须先备份并在 staging 做真实恢复演练。

## 6. Docker Compose 上线顺序

### 公网一键安装器

对于全新的 Ubuntu/Debian 公网服务器，可仅上传 GBR 编写的安装器。它会自动检测环境、安装宿主机缺少的
`curl`、`git`、`openssl`、`iproute2`、Docker Engine 和 Compose 插件，并从 GBR 官方 Git 仓库下载源码：

```bash
curl -fL -o install-public.sh \
  https://raw.githubusercontent.com/gongbangrui/wargame/main/deploy/install-public.sh
sudo bash install-public.sh --domain game.example.com
```

开始前须将 `game.example.com` 的 A/AAAA 记录指向服务器，并在云安全组、防火墙中开放 TCP 80、443。
脚本会检查端口占用、构建本地无头镜像。Qt、CMake、Ninja、编译器与服务器运行库全部在容器中自动安装；
随后生成 5 年有效期的 red/blue/director Token 与账号密码、生成管理员 hash、配置 Caddy 自动 HTTPS 并启动健康检查。

完成后客户端使用 `wss://game.example.com/ws`，管理员使用 `https://game.example.com/admin`。所有初始
凭据仅保存在 `/opt/wargame/INITIAL_CREDENTIALS.txt`，权限为 `600`。脚本不适用于已有 SQLite 数据库；
后续版本更新请使用 `deploy/upgrade.sh`，账号管理请通过 `/admin` 完成。

CI 或自动化场景：

```bash
sudo bash install-public.sh --domain game.example.com --yes
```

避免通过 `--admin-password` 直接传入生产密码，因为命令行参数可能被 shell 历史或进程列表记录；交互式
输入或安装器随机生成密码更安全。

1. 将 `deploy/` 复制到 `/opt/wargame`。
2. 将 `.env.example` 复制为 `.env`，生成 32 字节以上 pepper，权限设为 600。
3. 将 `server.prod.example.json` 复制为 `server.prod.json`，填入真实 tokenHash、账号配置和管理员 hash。
4. 把 `compose.yml` 中镜像组织名改为自己的仓库，构建并推送固定版本镜像。
5. 把 `Caddyfile` 的域名通过 `WARGAME_PUBLIC_HOST` 配置；DNS 先指向服务器。管理平台路径为 `/admin`。
6. 安全组和 UFW 只开放 22、80、443。
7. 运行 `docker compose config`，再 `up -d`。
8. 检查 `/healthz`、WSS 登录、红蓝隔离、断线重连和服务端重启恢复。
9. 执行一次备份和恢复演练后，才允许真实用户进入。

```bash
cd /opt/wargame
chmod 600 .env
docker compose --env-file .env -f compose.yml config
docker compose --env-file .env -f compose.yml up -d
docker compose --env-file .env -f compose.yml ps
./smoke-test.sh "https://${WARGAME_PUBLIC_HOST}"
```

`Containerfile` 用 `aqtinstall` 固定安装 Qt 6.10.0，运行镜像只复制服务器二进制、Qt
运行库和 SQLite 驱动，不包含编译器、QML 或地图资源。

## 7. 已自动化的阶段 6 基线

`wargame_network_tests` 当前覆盖：

- 32 个同时认证连接；
- 500 单元导演快照小于 2 MiB；
- malformed/超大/未知字段、三次协议违规断线和错误 WebSocket 路径；
- 权限与红蓝原始状态隔离；
- 令牌桶耗尽时，同一 `commandId` 仍返回原结果；
- 回执丢失后的同 ID 重发，以及认证失败停止重连；
- 最新检查点损坏回退、运行时原子校验和 tick/HP/位置/revision 重启恢复。

这不等于阶段 6 全部完成。真实 50/200/500 ms 延迟与抖动、30 秒断网、8 小时长稳、磁盘满/
只读数据库、p95/p99 指标和 staging WSS 仍需在受控环境执行。

## 8. 上线前人工验收

- 为 red、blue、director 分别创建独立 token，不共用。
- 用抓包或集成测试确认红方原始 JSON 不含未探测蓝方 ID、坐标、HP 或计划。
- 撤销一个 token，确认旧客户端无法重连。
- 连续发送重复 `commandId`，确认只执行一次。
- 断网 30 秒再恢复，确认客户端重连并收到完整快照。
- 强制终止服务器再启动，确认 tick、HP、位置和 revision 恢复。
- 将 staging 数据库备份到另一目录并真实恢复。
- 观察 `/metrics` 的拒绝、限流、重同步、发送队列和检查点失败计数。
