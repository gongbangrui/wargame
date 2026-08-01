# AUTHORITATIVE GAME SERVER

## OVERVIEW

`server/game` is the online authority: WebSocket sessions, room/seat lifecycle, command authorization, projected broadcasts, checkpoints, event replay and optional Fast DDS capability reporting.

## WHERE TO LOOK

| Responsibility | Type |
|----------------|------|
| Connection/auth/command orchestration | `GameServer` |
| Seat, deployment, readiness and lifecycle invariants | `AuthoritativeRoom` |
| Role/side/communication visibility filtering | `StateProjector` |
| Checkpoint/event log and recovery | `RoomPersistence` |
| Optional participant lifecycle | `FastDdsNode` |
| Process entry and `GAME_PORT` | `main.cpp` |

## CONVENTIONS

- `GameServer` is the orchestrator. `AuthoritativeRoom` is the authority for seats, transfer, deployment, readiness, revision, idempotency and lifecycle; connection session maps are mirrors only.
- Every client input passes authentication, seat/role/side control, communication reachability and parameter checks before it reaches the engine.
- Every snapshot, event, chat, intel share and map mark is filtered through `StateProjector` or an equivalent explicit projection before broadcast.
- Durable command events are appended before the recoverable state change. New durable event types require write, replay, validation and version handling together.
- Checkpoints use the existing atomic/size-limited persistence path. Restore must validate the checkpoint, reconstruct room/engine/idempotency state, replay later events and write a fresh checkpoint.
- Connection tokens, WebSocket sessions and online client collections are transient; do not serialize them into room state.
- Current persistence defaults are under `/data`: room checkpoint, command/event logs and status files. Respect rotation, fsync and incompatible-file archiving behavior.
- `FastDdsNode` currently creates capability metadata only. WebSocket remains the business data plane unless DDS ownership and consistency are explicitly designed.

## ANTI-PATTERNS

- Do not send a complete `SimulationEngine` snapshot and rely on QML to hide enemy fields.
- Do not let `ClientSession`/`m_seats` override `AuthoritativeRoom` state, or let client seat/side fields override server roster.
- Do not make a notification such as `TargetDestroyed` directly apply combat damage.
- Do not silently change checkpoint/event schema, sequence semantics or recovery ordering.
- Do not log bearer tokens, full authorization headers, passwords or untrusted secret input.

## TESTS

Changes affecting this directory should consider `test_authoritative_room.cpp`, `test_room_persistence.cpp`, `test_state_projector.cpp`, protocol/client state tests and the isolated network/Docker smoke surfaces.
