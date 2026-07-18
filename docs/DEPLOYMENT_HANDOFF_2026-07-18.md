# 联网部署交接记录

## 当前验证结果

- 本机 Compose staging：server healthy，Caddy HTTPS/WSS 正常。
- 健康地址：`https://localhost:18443/healthz`。
- 指标地址：`https://localhost:18443/metrics`。
- 直连开发地址：`ws://127.0.0.1:18080/ws`。
- 完整回归：`145/145` 通过。
- 真实 SQLite 备份、SHA-256 校验、恢复和健康回归通过。
- 故意失败的升级回滚通过，版本配置恢复为旧版本且服务 healthy。
- 未执行长时浸泡测试。

## 生产上线前

1. 复制 `.env.example` 为 `.env`，生成至少 32 字节 pepper，执行 `chmod 600 .env`。
2. 复制 `server.prod.example.json` 为 `server.prod.json`，为 red、blue、director 分别填入 token hash。
3. 执行 `chown -R 10001:10001 data logs && chmod 750 data logs backups`。
4. 执行 `./validate-prod.sh /opt/wargame`。
5. 执行 `docker compose ... config`、`up -d`、`wait-healthy.sh` 和 `smoke-test.sh`。

## 备份与恢复

```bash
./backup.sh /opt/wargame
sha256sum --check backups/wargame-*.sqlite3.sha256
./restore.sh backups/wargame-YYYYMMDDTHHMMSSZ.sqlite3 /opt/wargame
```

恢复脚本默认将活动数据库设置为 `660`。只有无法调整宿主机属主的 rootless 临时目录才允许
显式设置 `WARGAME_RESTORE_MODE=666`。

## 监控

将 `deploy/prometheus.yml` 和 `deploy/prometheus-alerts.yml` 装载到 Prometheus，采集
`server:9090/metrics`。告警重点为健康失败、检查点失败、tick 超过 50ms 和认证失败激增。

## 回滚

```bash
./upgrade.sh <已验证版本> /opt/wargame
```

升级脚本会先备份，拉取失败或健康检查失败时恢复旧版本配置并重新启动旧 server。
