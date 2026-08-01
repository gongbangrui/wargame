# TESTS

## OVERVIEW

`tests/` combines the C++ GoogleTest target, authoritative-room aggregation tests and an independent Python account lifecycle test. Network, Node and Docker tests live under `tools/` and are not automatically CTest cases.

## WHERE TO LOOK

| Layer | Files/target | Run with |
|-------|--------------|----------|
| Domain/FSM/combat | `test_engine.cpp`, `test_unitfsm.cpp`, `test_combat_resolver.cpp`, related files | `wargame_tests` |
| Protocol/state | `test_protocol.cpp`, `test_snapshot.cpp`, `test_client_state_store.cpp` | `wargame_tests` |
| Server room/persistence | `test_authoritative_room.cpp`, `test_room_persistence.cpp`, `test_state_projector.cpp` | `authoritative_room_tests`/`wargame_tests` |
| Account service | `test_account_room_lifecycle.py` | Python/uv command |

## CONVENTIONS

- C++ tests use GoogleTest suites and descriptive `TEST`/`TEST_F` names; CTest discovers `wargame_tests`, while `GameServer.AuthoritativeRoom` wraps the dedicated room binary.
- Use `cmake --preset debug` and `cmake --preset sanitizers` before comparing test sets. Sanitizer runs use the preset ASAN/UBSAN environment.
- Add focused regression coverage for changed domain, protocol, persistence, projection or client-state behavior. Keep fixtures isolated and deterministic.
- Account tests use temporary data; never point them at a production `DATA_DIR`.
- Network smoke, room contract and Docker recovery require separate credentials/services/ports and must not be silently folded into unit tests.
- If CTest registration changes, update both preset builds and run `tools/verify-test-baseline.sh`.

## ANTI-PATTERNS

- Do not delete or weaken a failing test to make a build green.
- Do not assume a passing unit test covers QML interaction, real WebSocket lifecycle, Fast DDS or Docker recovery.
- Do not leave test accounts, rooms, credentials, checkpoints or volumes behind after integration tests.
- Do not use shared mutable global state when a temporary directory, fake clock or in-process server fixture is available.
