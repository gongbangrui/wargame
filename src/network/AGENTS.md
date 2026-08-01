# CLIENT NETWORK

## OVERVIEW

`src/network` owns account login, WebSocket sessions, reconnect behavior, command retry, latency diagnostics, client snapshot/delta state and the optional Fast DDS transport adapter.

## WHERE TO LOOK

| Concern | Location | Notes |
|---------|----------|-------|
| Session and commands | `NetworkClient.*` | HTTPS login, WebSocket auth, reconnect and command ids |
| Remote state | `ClientStateStore.*` | Snapshot baseline, contiguous deltas and resync |
| Data-plane adapter | `FastDdsTransport.*` | Optional `ITransport` implementation |
| Tests | `tests/test_client_state_store.cpp`, `test_fastdds_transport.cpp`, `test_transport.cpp` | Recovery and transport behavior |

## CONVENTIONS

- Login obtains the token and WebSocket URL from the account service; the game server remains the authority for room and seat state.
- `ClientStateStore` accepts a complete snapshot first, then only deltas with the expected contiguous revision. Gaps or failed application trigger resync.
- Commands need stable ids, explicit pending state and bounded retransmission. Do not turn a send attempt into a confirmed result.
- Reconnect must restore authentication/session state through the server protocol and must not replay commands outside the pending-command contract.
- Account and game latency diagnostics are observability only; they must not change simulation authority.
- `FastDdsTransport` is optional and must preserve `ITransport` ownership and sink lifetime. WebSocket is still the normal server data plane.

## ANTI-PATTERNS

- Do not advance remote simulation time in the client.
- Do not accept client-provided `side`, `seat`, role or visibility as authoritative.
- Do not expose tokens or authorization headers in logs, payloads, QML properties or persisted scenario files.
- Do not optimistically mark a seat, deployment or command as successful before the server response.
