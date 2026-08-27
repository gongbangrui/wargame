# 发布流程

联网协议当前为 v8/schema 8。客户端和权威 `game-server` 优先使用 v8；普通房间仍支持 v7/v6/v4
降级协商。升级前先停止写入、备份账号数据库以及房间 `room-checkpoint.json`、`room-commands.jsonl`
和全部轮转日志，确认备份可在隔离卷恢复后再替换服务。

本流程适用于桌面客户端和 Docker 联网服务的同一版本发布。发布前先将
`WARGAME_VERSION` 更新为目标语义版本，保证 CMake 服务端、Docker 镜像标签和部署配置一致。
`deploy/release-manifest.env` 固定 v8/schema 8、`appindex`、`account-web`、`game-server` 和
WebSocket 权威数据面；`release-identity.txt` 保存一次计算的版本、源码摘要和协议身份。

### 三端统一构建

在具备 Qt 6.10（根项目）和 Qt 6.4（独立 `server/`）的构建机上执行：

```bash
WARGAME_VERSION=2.0.0 ./tools/build-release.sh --clean
(cd dist/release-2.0.0 && sha256sum -c SHA256SUMS)
```

桌面客户端发布产物由 `appindex` 和同级 `map/` 目录共同组成；两者必须一起分发。
`SHA256SUMS` 覆盖可执行文件、发布身份以及全部 GIS 运行时瓦片。

输出目录同时包含桌面 `appindex`、根项目服务端、Qt 6.4 独立服务端和发布身份文件。
服务器发布包仍由 `deploy/package-one-click.sh` 生成；它会携带同一身份并在安装时拒绝摘要或
协议不一致的归档。

## 自动化门禁

### 独立服务发布包

生成的服务发布物必须包含 authority server、account-web 静态资源、Docker 输入和安装器，
同时生成同名 SHA-256 sidecar。发布包不得包含 `.env`、数据库、JSONL、检查点、日志、备份、
构建目录或 Docker 镜像。接收方使用以下流程，不需要 Git 仓库或固定下载主机：

```bash
sha256sum -c wargame-server-<version>.tar.gz.sha256
tar -xzf wargame-server-<version>.tar.gz -C /tmp
cd /tmp/wargame-server-<version>
sudo ./deploy/install-server.sh --install-dir "$HOME/wargame" --compose-project wargame
sudo docker compose --project-name wargame --env-file "$HOME/wargame/.env" \
  -f "$HOME/wargame/current/deploy/compose.yml" ps
```

发布包验收使用 `RECOVERY_HTTP_PORT=18180 RECOVERY_WS_PORT=18190
./tools/verify-standalone-deployment.sh`，该验证器使用临时 Compose 项目、数据卷、端口和
用户目录，并在退出时清理全部资源。

在干净工作区执行：

```bash
cmake --preset debug
cmake --preset sanitizers
cmake --build --preset debug
cmake --build --preset sanitizers
./tools/verify-test-baseline.sh build/debug build/sanitizers
ctest --test-dir build/debug --output-on-failure
ctest --test-dir build/sanitizers --output-on-failure
cmake --build build/debug --target all_qmllint
./tools/check-source-format.sh
./tools/verify-docker-recovery.sh
```

联网房间专项回归还需执行以下独立检查：账号生命周期测试位于
`tests/test_account_room_lifecycle.py`，房间托管契约脚本位于
`tools/room-hosting-contract.mjs`，权威房间测试源文件位于
`tests/test_authoritative_room.cpp`，CMake 目标为 `authoritative_room_tests`，并注册为
`GameServer.AuthoritativeRoom`：

```bash
uv run --with-requirements server/account/requirements.txt python tests/test_account_room_lifecycle.py
ADMIN_PASSWORD='管理员密码' node tools/room-hosting-contract.mjs
cmake --build build/debug --target authoritative_room_tests
build/debug/authoritative_room_tests --gtest_color=no
```

账号生命周期测试使用临时数据目录独立运行；托管契约脚本需要按
`docs/ONLINE_DEPLOYMENT.md` 启动的服务，并会创建和清理临时房间。

推送后，GitHub Actions 的 `Native quality gate` 与 `Docker smoke and recovery` 必须都通过。
Docker 演练会验证联网认证、权限、消息幂等、优雅停止最终检查点、数据卷备份、还原卷和
恢复后的再次联网冒烟。

## 人工验收

以下项目不可由 CI 可靠替代，必须在候选版本上记录结果：

- 900x620、1360x860 和高 DPI/字体缩放下的四席位、情报工作区、双设置面板与账号管理界面。
- 断网 30 秒、服务端重启、丢失 delta 与自动重连后的状态恢复。
- 32 个连接、500 单元的压力测试；记录 tick 耗时、内存、发送队列和重连率。
- 管理员登录、账号单客户端约束、战位权限、敌方视野裁剪和检查点恢复。

完整项目清单见 `docs/NETWORK_TEST_CHECKLIST.md`；任何未通过项均不能标记为正式发布。

## 发布与回滚

1. 记录目标 commit、`WARGAME_VERSION`、CI 链接和人工验收结果。
2. 在 staging 使用与生产相同的 `.env` 字段和持久卷配置部署候选镜像。
3. 对生产数据卷执行归档备份，验证归档可还原到隔离卷。
4. 确认桌面客户端、`vmf-demo-v2` 演示房间和 `vmf-guided-strike-v1` 兼容房间均已切换到 v8，再更新生产 `.env` 的 `WARGAME_VERSION` 并执行
   `docker compose --project-name wargame --env-file /path/to/.env -f /path/to/current/deploy/compose.yml up -d --build`。
5. 通过管理员“服务器监控”确认 game-server 状态为 `healthy`，再执行联网冒烟验证。
6. 出现回归时，停止服务但保留卷，将 `WARGAME_VERSION` 回退到上一已验证版本并重新部署。

回滚不能删除 `wargame-data` 卷；只有完成独立恢复演练后才允许清理历史备份。
