# CORE DOMAIN

## OVERVIEW

`src/core` owns simulation rules, clocks, transports, scenario data, map coordinates, combat resolution and snapshot/checkpoint codecs.

## WHERE TO LOOK

| Concern | Files | Contract |
|---------|-------|----------|
| Engine orchestration | `SimulationEngine.*` | Fixed tick order, commands, replay and rollback |
| Unit model/FSM | `UnitBase.*`, `UnitFsm.*` | Health, capabilities, movement and state transitions |
| Communication | `MessageBus.*`, `ITransport.*`, `LocalTransport.*` | Distance, side and command-post rules |
| Persistence | `Scenario.*`, `SnapshotCodec.*` | Schema/version and complete restore validation |
| Determinism | `CombatResolver.*`, `IClock.*` | Seeded results and injectable time |

## CONVENTIONS

- `SimulationEngine` depends on `IClock` and `ITransport` abstractions. Keep network adapters outside this directory.
- The normal local path uses a 50 ms tick, `RealTimeClock` and `LocalTransport`; `LockedStepClock` is a separate synchronization path.
- Mobile units configure `UnitFsm` in their constructor and delegate their tick behavior to it.
- Unit ids are unique and non-empty; runtime maps, communication registration and checkpoint unit sets must agree.
- Health, subsystem and effective capability values must retain their existing bounds. Dead units do not move, repair or receive combat damage.
- `CombatResolver::resolve` remains deterministic for the same request, sequence and battle seed. `CommandResult` error codes are externally visible.
- Scenario/checkpoint schema changes require compatibility handling and tests. Restore must reject duplicates, unknown units and partial state without corrupting the current state.
- `TileCacheLocator` and `TileImageProvider` are client resource helpers; do not make combat or unit rules depend on them.

## ANTI-PATTERNS

- Do not add QML, WebSocket, HTTP or `server/game` policy to the domain.
- Do not reorder `SimulationEngine` tick phases casually; it changes replay and lockstep behavior.
- Do not treat a notification such as target destruction as a new authoritative damage command.
- Do not leave an external transport sink pointing at a destroyed engine or let the engine own a transport it does not own.

## TESTS

Use `tests/test_engine.cpp`, `test_unitfsm.cpp`, `test_messagebus.cpp`, `test_transport.cpp`, `test_snapshot.cpp`, `test_checkpoint.cpp`, `test_combat_resolver.cpp`, `test_locksync.cpp` and `test_geo.cpp` for the corresponding contract.
