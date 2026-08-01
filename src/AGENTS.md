# SOURCE MODULES

## OVERVIEW

`src/` contains shared C++ domain code plus client-only network, view and map adapters. Keep dependencies flowing from adapters toward the domain; the domain must not know about QML or server implementation.

## WHERE TO LOOK

| Area | Location | Boundary |
|------|----------|----------|
| Simulation and infrastructure | `core/` | `wargame_domain`, Qt Core only |
| Concrete units | `units/` | Extends `core/UnitBase` |
| Shared wire contract | `protocol/` | `wargame_protocol`, client/server shared |
| Client transport/state | `network/` | `wargame_client`, WebSocket/Fast DDS |
| QML-facing facade | `view/` | Client-only Qt Quick bridge |

## CONVENTIONS

- Keep `src/core` and `src/units` free of QML, HTTP/WebSocket and server-room policy.
- `src/protocol` may depend on core value types only where the existing contract requires it; do not move UI or account concepts into the wire schema.
- `src/network` and `src/view` may depend on domain and protocol, never the reverse.
- New client-only sources must be added to `wargame_client` in the root `CMakeLists.txt`; domain/protocol sources belong in `cmake/WargameTargets.cmake`.
- Public project types use namespace `gbr`, paired `.h`/`.cpp` files and four-space indentation.
- Changes crossing a target boundary require both the root build and the standalone server build to remain valid.

## ANTI-PATTERNS

- Do not include QML-facing headers from domain code.
- Do not make a server room or account API type part of the reusable domain model.
- Do not duplicate protocol serialization in client, server and QML; update the shared protocol implementation and its tests.
- Do not hide ownership changes to clocks, transports or QObject parents in unrelated adapters.
