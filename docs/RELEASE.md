# 发布流程

本流程适用于桌面客户端和 Docker 联网服务的同一版本发布。发布前先将
`WARGAME_VERSION` 更新为目标语义版本，保证 CMake 服务端、Docker 镜像标签和部署配置一致。

## 自动化门禁

在干净工作区执行：

```bash
cmake --preset debug
cmake --preset sanitizers
cmake --build --preset debug
cmake --build --preset sanitizers
./tools/verify-test-baseline.sh build/debug build/sanitizers
ctest --preset debug
ctest --preset sanitizers
cmake --build build/debug --target all_qmllint
./tools/check-source-format.sh
./tools/verify-docker-recovery.sh
```

推送后，GitHub Actions 的 `Native quality gate` 与 `Docker smoke and recovery` 必须都通过。
Docker 演练会验证联网认证、权限、消息幂等、优雅停止最终检查点、数据卷备份、还原卷和
恢复后的再次联网冒烟。

## 人工验收

以下项目不可由 CI 可靠替代，必须在候选版本上记录结果：

- 1100x720、1360x860 和高 DPI/字体缩放下的四席位与账号管理界面。
- 断网 30 秒、服务端重启、丢失 delta 与自动重连后的状态恢复。
- 32 个连接、500 单元的压力测试；记录 tick 耗时、内存、发送队列和重连率。
- 管理员登录、角色权限、敌方视野裁剪和检查点恢复。

完整项目清单见 `docs/NETWORK_TEST_CHECKLIST.md`；任何未通过项均不能标记为正式发布。

## 发布与回滚

1. 记录目标 commit、`WARGAME_VERSION`、CI 链接和人工验收结果。
2. 在 staging 使用与生产相同的 `.env` 字段和持久卷配置部署候选镜像。
3. 对生产数据卷执行归档备份，验证归档可还原到隔离卷。
4. 更新生产 `.env` 的 `WARGAME_VERSION` 并执行 `docker compose -f deploy/compose.yml up -d --build`。
5. 通过管理员“服务器监控”确认 game-server 状态为 `healthy`，再执行联网冒烟验证。
6. 出现回归时，停止服务但保留卷，将 `WARGAME_VERSION` 回退到上一已验证版本并重新部署。

回滚不能删除 `wargame-data` 卷；只有完成独立恢复演练后才允许清理历史备份。
