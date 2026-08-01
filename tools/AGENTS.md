# VERIFICATION TOOLS

## OVERVIEW

`tools/` contains source-format checks, CTest baseline comparison, real-service Node contracts/smoke tests and isolated Docker recovery validation. These scripts verify systems; they do not deploy production services.

## WHERE TO LOOK

| Tool | Prerequisites | Scope |
|------|---------------|-------|
| `check-source-format.sh` | Git | Trailing whitespace and diff checks |
| `verify-test-baseline.sh` | Two configured builds | Debug/Sanitizer CTest set parity |
| `network-smoke.mjs` | Node, account/game services, admin secret | Auth, rooms, seats, visibility and commands |
| `room-hosting-contract.mjs` | Node, deployed services, admin secret | Room hosting lifecycle |
| `verify-docker-recovery.sh` | Docker Compose, ports | Isolated smoke, stop, backup, restore and restart |

## CONVENTIONS

- Use unique temporary Compose project/volume names and never target production resources.
- Pass secrets through environment variables or the documented test mechanism; do not echo or hardcode real credentials.
- Network scripts create temporary accounts/rooms and must clean them up on success and failure. Recovery scripts must remove temporary volumes/directories after collecting safe diagnostics.
- Keep deployment logic in `deploy/`; tools may start an isolated test system but must not change host installation or production configuration.
- When build/test registration changes, run both preset builds before declaring the baseline comparison meaningful.

## ANTI-PATTERNS

- Do not run Docker recovery with production Compose names, volumes or `.env`.
- Do not print bearer tokens, passwords, full authorization headers or unredacted service logs into CI artifacts.
- Do not make an integration script depend on an undocumented local port or persistent test account.
- Do not treat Node/Python/Docker checks as CTest coverage; document their separate prerequisites.
