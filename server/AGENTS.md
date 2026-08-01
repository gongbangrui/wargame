# SERVER SERVICES

## OVERVIEW

`server/` contains the headless C++ authoritative game server and the independent Python account web service. The two services coordinate over HTTP/internal APIs and share deployment data only through the documented Compose volume contract.

## WHERE TO LOOK

| Area | Location | Build/runtime |
|------|----------|---------------|
| Server target | `CMakeLists.txt`, `game/` | `wargame_serverlib`, `wargame_server` |
| Game authority | `game/` | Qt Core/Network/WebSockets, no Qt Quick |
| Account API | `account/` | FastAPI/Uvicorn, SQLite, static admin UI |
| Deployment contract | `../deploy/`, `../docs/ONLINE_DEPLOYMENT.md` | Compose services and `/data` |

## CONVENTIONS

- The standalone server CMake path is intentionally separate from the root client path and has a Qt 6.4 minimum. Keep it free of Qt Quick and QtKeychain dependencies.
- `wargame_server` owns online simulation time and room authority. Clients are untrusted command submitters.
- Authentication/account operations belong to `server/account`; game permissions, projection and room lifecycle belong to `server/game`.
- Treat `INTERNAL_API_KEY`, `/data`, checkpoints, event logs and SQLite as production-sensitive boundaries.
- Optional Fast DDS support is capability-gated; do not claim it is the authoritative data plane unless topics, ownership and consistency are implemented.

## ANTI-PATTERNS

- Do not let account data decide simulation state without the game-server contract, or let game sessions write account-owned tables directly.
- Do not expose unprojected engine state or accept client-provided role/side/seat as authority.
- Do not add a server dependency on the QML client target.
- Do not test destructive deployment scripts against the production Compose project or volume.

## VALIDATION

Build the standalone server target as well as the root debug target when changing shared server/domain code; run the account lifecycle test separately for Python service changes.
