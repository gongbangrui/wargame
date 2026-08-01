# SHARED PROTOCOL

## OVERVIEW

`src/protocol` is the client/server wire contract: envelopes, protocol validation, state snapshots, deltas and stable command results.

## WHERE TO LOOK

| Concern | Location | Notes |
|---------|----------|-------|
| Message/envelope types | `Protocol.h/.cpp` | Shared JSON shape and validation |
| State projection data | `StateDelta.h/.cpp` | Snapshot/delta encoding and application |
| Consumers | `src/network/`, `server/game/` | Client store and server projector must agree |
| Tests | `tests/test_protocol.cpp`, `test_snapshot.cpp`, `test_client_state_store.cpp` | Contract and recovery coverage |

## CONVENTIONS

- Treat field names, enum strings, error codes, sequence/revision semantics and schema versions as compatibility contracts.
- Validate untrusted JSON at the protocol boundary before converting it into domain commands.
- A delta is valid only against its expected base revision. The client must establish a complete snapshot before applying deltas.
- Keep server visibility filtering in `server/game/StateProjector`; protocol types should represent the permitted result, not decide permissions.
- Add new fields with explicit defaults or version handling. Update both C++ consumers and all relevant tests in the same change.
- Protocol changes must remain usable by the standalone server target, which does not link Qt Quick or QtKeychain.

## ANTI-PATTERNS

- Do not silently rename, reuse or reinterpret an existing field, error code or message type.
- Do not put account database records, QML-only presentation state or server-private secrets into shared payloads.
- Do not send full engine snapshots from the server and rely on QML to hide data.
- Do not accept a sequence gap, unknown base revision or malformed command envelope as a best-effort update.
