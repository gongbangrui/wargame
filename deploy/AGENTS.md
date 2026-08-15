# DEPLOYMENT

## OVERVIEW

`deploy/` is the production-facing Compose, Docker image, install, reset, uninstall and source-package surface. Scripts can change host services, credentials, containers and persistent data.

## WHERE TO LOOK

| Operation | Entry |
|-----------|-------|
| Install/update | `install-server.sh` |
| Compose contract | `compose.yml`, `.env.example` |
| Admin reset | `reset-admin.sh` |
| Room reset | `reset-room.sh` |
| Uninstall | `uninstall-server.sh` |
| Source package | `package-one-click.sh` |

## CONVENTIONS

- `account-web` and `game-server` share the documented named `/data` volume; account API and game-server persistence ownership must remain distinct.
- Runtime configuration comes from the managed installation root `.env` with mode `0600`; replaceable source lives under `current/`, backups under `backups/`, and the `.wargame-install` marker identifies a managed root. Never commit runtime configuration. Use absolute Compose/env/project paths from the installed root.
- Install/update scripts may install packages, write `.env`, rebuild images, recreate containers and enable services. Verify target host, Compose project and backup before running.
- `reset-room.sh` requires explicit confirmation and must back up `/data` before removing room checkpoint/log/status files. `uninstall-server.sh --purge-data` and `--remove-config` are destructive.
- `package-one-click.sh` must emit a deterministic archive plus `.sha256` sidecar, include `release-manifest.env` for the v6/schema 6 three-end contract, and exclude every `.env` variant, databases, JSONL, checkpoints, logs, backups, build outputs and other runtime secrets. `QUICK_START.md` is the package-local fresh-host guide.
- `install-server.sh` accepts a source directory or validated archive through `--source`, stages it beside `current/`, and preserves the installed `.env`, backups and named volume across updates. Failed activation keeps the previous `current/` usable.
- Keep production behind the documented network/reverse-proxy boundary; evaluate the high-risk web-shell setting explicitly.

## ANTI-PATTERNS

- Do not run reset, uninstall or recovery commands against an unintended Compose project or volume.
- Do not put passwords or API keys in command examples, committed files, image layers or CI logs.
- Do not delete volumes/configuration without explicit scope, backup and confirmation.
- Do not use `latest` as the only production image identity or change ports/bind addresses without reviewing exposure.
