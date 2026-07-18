# Staging WSS、恢复与升级演练

该目录的 Compose 配置运行单个权威服务器实例。升级会短暂重启服务器；它是可回滚的受控
升级，不是零停机滚动发布。要实现零停机，需要引入共享持久化、会话迁移和至少两个协调的
服务器副本，当前架构不具备这些前提。

## 上线前准备

1. 在 staging 主机上创建 `/opt/wargame`，复制 `deploy/` 中的文件。
2. 将 `.env.example` 复制为 `.env`，设置固定镜像版本、公开域名和至少 32 字节的 pepper。
3. 将 `server.prod.example.json` 复制为 `server.prod.json`，填入测试 token 的 hash，并按
   `docs/P0_NETWORKING_IMPLEMENTATION.md` 生成管理员 salt/hash 后填写 `admin`。
4. 创建 `data`、`logs` 和 `backups` 目录，确保容器内 UID 10001 可写 `data` 与 `logs`：
   `chown -R 10001:10001 data logs && chmod 750 data logs && chmod 750 backups`。
5. 将 DNS 指向 staging 主机；防火墙只开放 80、443 和受控 SSH。

## WSS 与健康验收

```bash
cd /opt/wargame
docker compose --env-file .env -f compose.yml config
docker compose --env-file .env -f compose.yml up -d
./wait-healthy.sh /opt/wargame 90
./smoke-test.sh "https://${WARGAME_PUBLIC_HOST}"
# 可选：设置 WARGAME_VALID_TOKEN 后执行 WebSocket 有效/无效 token 探针
WARGAME_CURL_INSECURE=1 ./staging-check.sh /opt/wargame "https://${WARGAME_PUBLIC_HOST}"
```

使用一个 red token 和一个 blue token 分别通过 `wss://${WARGAME_PUBLIC_HOST}/ws` 登录，确认：

- TLS 证书有效，`/healthz` 返回 `status=ok`；
- 红蓝无法获取未探测敌方的原始坐标、HP、计划和共享知识；
- 服务端重启后 tick、HP、位置和场景版本从检查点恢复。
- 通过 `https://${WARGAME_PUBLIC_HOST}/admin` 登录管理平台，创建测试账号并验证账号密码登录、禁用和重置密码。

## 备份与恢复演练

```bash
./backup.sh /opt/wargame
./restore.sh /opt/wargame/backups/wargame-YYYYMMDDTHHMMSSZ.sqlite3 /opt/wargame
./wait-healthy.sh /opt/wargame 90
```

恢复前脚本保存当前数据库，恢复后必须执行两种 token 登录和健康检查。备份文件的 SHA-256
校验失败时不得继续恢复。

## 受控升级与回滚演练

```bash
./upgrade.sh 0.2.0 /opt/wargame
```

脚本会先创建一致性备份，再拉取新镜像、重建 server，并在 90 秒内检查健康状态。任一步失败
时会把 `.env` 中的版本恢复为旧值、重新启动旧服务器并再次检查健康状态。演练完成后记录：

- 新旧镜像版本和升级开始/完成时间；
- 健康恢复耗时；
- 登录、权限隔离、检查点恢复和备份恢复结果；
- 回滚是否被触发及其结果。

## 短时验收矩阵

不进行长时浸泡测试时，至少执行以下短时检查：

```bash
./staging-check.sh /opt/wargame "https://${WARGAME_PUBLIC_HOST}"
curl --fail "https://${WARGAME_PUBLIC_HOST}/healthz"
curl --fail "https://${WARGAME_PUBLIC_HOST}/metrics" | tee /tmp/wargame-metrics.txt
docker compose --env-file .env -f compose.yml restart server
./wait-healthy.sh /opt/wargame 90
```

验收内容包括健康接口、有效/无效 token、红蓝权限隔离、异常断连后重连、服务器重启恢复和
升级失败回滚。每次验收记录镜像摘要、数据库 SHA-256、恢复耗时和关键指标。

## 监控与日志

`/metrics` 提供 Prometheus 文本格式指标。至少采集连接数、认证失败、命令接受/拒绝、限流、
重同步、慢客户端断开、检查点失败、持久化失败和 tick 耗时。Compose 的 server 与 Caddy 日志
均使用 Docker `local` 驱动，单个文件 20 MiB、最多 5 个文件；宿主机仍应配置磁盘使用告警。

Prometheus 与 Compose 放在同一 Docker 网络时，可使用 `prometheus.yml` 抓取 `server:9090`，
并加载 `prometheus-alerts.yml`。公网只暴露 Caddy 的 80/443，不直接暴露 9090。
