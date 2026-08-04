# PROJECT KNOWLEDGE BASE

**Generated:** 2026-07-29
**Commit:** da7b0b3
**Branch:** main

## OVERVIEW

Qt 6.10+/C++20 兵棋推演工程，包含 QML 桌面客户端、共享仿真/协议库、可选 Fast DDS 适配、无头权威游戏服务器和独立 Python 账号服务。客户端本地模式由 `SimulationEngine` 推进；联网模式由服务器推进，客户端只发送命令并消费经过权限裁剪的状态。

## STRUCTURE

```text
./
├── src/                 # 共享领域、协议、客户端网络和 QML 桥接
├── qml/                 # Qt Quick 根壳、视图和组件
├── server/              # C++ 权威游戏服务器与 Python 账号服务
├── tests/               # GoogleTest 及账号生命周期测试
├── deploy/              # Docker Compose、安装、恢复和发布脚本
├── tools/               # 格式、基线、联网冒烟和 Docker 验证
├── map/                 # 运行时瓦片、GIS 元数据和制图工具
├── docs/                # 网络、部署、发布和验收说明
└── cmake/               # 共享 CMake target helpers
```

## WHERE TO LOOK

| Task | Location | Notes |
|------|----------|-------|
| Domain simulation | `src/core/`, `src/units/` | `wargame_domain`; default tick is 50 ms |
| Shared wire contract | `src/protocol/` | Snapshot, delta, envelope and command results |
| Client network state | `src/network/` | HTTPS login, WebSocket, reconnect, resync and retries |
| QML bridge | `src/view/SimulationController.*` | Sole business bridge exposed as `controller` |
| QML UI | `Main.qml`, `qml/` | Use controller APIs; do not reach into engine objects |
| Server authority | `server/game/` | Room, permissions, projection, persistence and WebSocket |
| Account API | `server/account/` | FastAPI/Uvicorn and SQLite-backed administration |
| Build graph | `CMakeLists.txt`, `server/CMakeLists.txt`, `cmake/` | Root and standalone server paths differ |
| Quality gates | `.github/workflows/quality.yml`, `tools/` | Native and Docker recovery jobs |
| Deployment | `deploy/`, `docs/ONLINE_DEPLOYMENT.md` | Production-facing scripts and data volumes |

## CODE MAP

| Symbol | Type | Location | Role |
|--------|------|----------|------|
| `main` | Client entry | `main.cpp` | Creates Qt app, controller, editor and QML engine |
| `SimulationController` | QML facade | `src/view/SimulationController.*` | Selects local/online mode and forwards commands |
| `SimulationEngine` | Domain engine | `src/core/SimulationEngine.*` | Owns local authoritative simulation and ticks |
| `NetworkClient` | Client adapter | `src/network/NetworkClient.*` | Authenticates and exchanges WebSocket messages |
| `ClientStateStore` | Client state machine | `src/network/ClientStateStore.*` | Applies snapshot/delta and requests resync on gaps |
| `GameServer` | Server orchestrator | `server/game/GameServer.*` | Auth, sessions, commands, broadcast and recovery |
| `AuthoritativeRoom` | Room authority | `server/game/AuthoritativeRoom.*` | Seats, deployment, readiness and lifecycle |
| `StateProjector` | Security boundary | `server/game/StateProjector.*` | Filters state/events by role, side and communication |
| `RoomPersistence` | Durable state | `server/game/RoomPersistence.*` | Checkpoints, event log, rotation and restore |

## CONVENTIONS

- Use CMake Presets and Ninja. Root builds require Qt 6.10+, Qt Quick, Network, WebSockets and Qt6Keychain; C++ is C++20.
- `debug` builds client, server and tests in `build/debug`; `sanitizers` uses ASan/UBSan in `build/sanitizers`.
- The standalone `server/` project has a lower Qt 6.4 minimum and intentionally avoids Qt Quick/QtKeychain; keep that distinction explicit.
- C++ project types belong in namespace `gbr`; use paired headers/sources and four-space indentation.
- `wargame_domain` must remain independent of QML and server implementation. `wargame_protocol` is shared by client and server.
- QML receives `controller` from `main.cpp`/`Main.qml`; generic actions use `controller.command(action, args)`, while server-side permissions remain authoritative.
- In online mode, the server advances simulation time and sends projected state. Client-side QML permission flags are UX gates, never security checks.
- Exactly one live command post per side is required before a match can run. Stable command-post ids are `red_cp` and `blue_cp`.
- Map source files under `map/` are authoritative; CMake stages the runtime subset into the build tree.
- GoogleTest is fetched by CMake. Python, Node, QML lint, Docker recovery and source-format checks are separate validation surfaces.

## ANTI-PATTERNS (THIS PROJECT)

- Do not restore the old single-process-only architecture description; online sessions are server-authoritative.
- Do not expose complete enemy state, unprojected events, credentials, tokens, databases, checkpoints or logs to clients or Git.
- Do not let QML call `SimulationEngine`, `MessageBus` or network internals directly, or treat QML permission state as authorization.
- Do not execute remote simulation locally, optimistically confirm seats/commands, or apply a delta without a valid snapshot and contiguous revision.
- Do not make a notification/event mutate authoritative combat state, and do not send a server command before durable-event handling where persistence requires it.
- Do not edit a running scenario, change production data without an isolated backup, or run recovery scripts against a production Compose project.
- Do not commit build outputs, `.env`, SQLite databases, JSONL/checkpoints, logs, Qt Creator state, generated releases or map build intermediates.

## COMMANDS

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug

cmake --preset sanitizers
cmake --build --preset sanitizers
ctest --preset sanitizers

cmake --build build/debug --target all_qmllint
./tools/check-source-format.sh
./tools/verify-test-baseline.sh build/debug build/sanitizers
uv run --with-requirements server/account/requirements.txt python tests/test_account_room_lifecycle.py
```

For deployment-level validation, use isolated credentials and ports with `tools/network-smoke.mjs`, `tools/room-hosting-contract.mjs`, and `tools/verify-docker-recovery.sh`. Release-specific sequencing and rollback rules live in `docs/RELEASE.md`.

## NOTES

- `corelib` is only a compatibility alias for `wargame_client`; do not treat it as the pure domain library.
- Fast DDS is optional and discovery-based. WebSocket remains the current authoritative server data plane unless the implementation explicitly adds DDS ownership and consistency rules.
- Fast DDS 首期只作为可选兼容适配：`WargameEnvelope.idl`/CMake 生成代码和 participant/topic/reader/writer 生命周期必须在 SDK 与 `fastddsgen` 同时存在时才启用；缺失依赖要明确显示 disabled。WebSocket 仍是登录、权威 snapshot/delta、命令和 resync 的唯一数据面。
- `backups/` and `dist/` are runtime/release artifacts, not source modules. `build/` and Qt Creator generated directories must stay out of code navigation.
- The working tree may contain broad user changes. Preserve unrelated changes when updating this knowledge base.
