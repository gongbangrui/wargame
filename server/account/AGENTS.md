# ACCOUNT SERVICE

## OVERVIEW

`server/account` is the FastAPI/Uvicorn account and administration service with a SQLite database and static browser console. It is separate from the C++ game authority.

## WHERE TO LOOK

| Concern | Location |
|---------|----------|
| HTTP API, schema and auth | `app.py` |
| Admin console | `static/index.html`, `static/app.js`, `static/app.css` |
| Runtime dependencies | `requirements.txt` |
| Admin password reset | `reset_admin.py`, `../../deploy/reset-admin.sh` |
| Lifecycle tests | `../../tests/test_account_room_lifecycle.py` |

## CONVENTIONS

- Development entry is `uv run --with-requirements server/account/requirements.txt python -m uvicorn app:app`; production runs in `account.Dockerfile`/Compose.
- Use `DATA_DIR/wargame.db` for account-owned SQLite. Do not write databases, sessions or runtime files into the source tree.
- Argon2 password hashes, bearer admin sessions, session expiry and one-active-player-session behavior are security contracts.
- Internal game-server calls require `INTERNAL_API_KEY`; keep account-owned tables separate from game-server-owned checkpoint/status/event files in the shared `/data` volume.
- Schema changes update `initialize_database()`, reset behavior and lifecycle tests together.
- Browser admin tokens stay in `sessionStorage`; do not move them to `localStorage`, HTML or logs. Terminal access remains ticketed, time-limited and non-root.

## ANTI-PATTERNS

- Do not expose the development server as the production deployment path.
- Do not log passwords, bearer tokens, internal keys or complete authorization headers.
- Do not let the static console bypass account API authorization or let account code mutate game checkpoints directly.
- Do not run reset scripts without the documented scope and backup/confirmation behavior.
