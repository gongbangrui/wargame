#!/usr/bin/env bash
set -Eeuo pipefail

# Reset the authoritative runtime state for the Compose-hosted game room.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CURRENT_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
if [[ -f "$CURRENT_ROOT/../.wargame-install" ]]; then
    ROOT_DIR="$(cd -- "$CURRENT_ROOT/.." && pwd)"
else
    ROOT_DIR="$CURRENT_ROOT"
fi
COMPOSE_FILE="$ROOT_DIR/current/deploy/compose.yml"
ENV_FILE="$ROOT_DIR/.env"
[[ -f "$ROOT_DIR/.wargame-install" ]] || {
    [[ "$ROOT_DIR" == "$CURRENT_ROOT" ]] || { printf '[reset-room] error: managed installation marker not found\n' >&2; exit 1; }
    COMPOSE_FILE="$ROOT_DIR/deploy/compose.yml"
}
ROOM_ID="main"
ASSUME_YES=0
STARTUP_TIMEOUT_SECONDS=120
BACKUP_ROOT="$ROOT_DIR/backups"
SERVICES_STOPPED=0

log() { printf '[reset-room] %s\n' "$*"; }
die() { printf '[reset-room] error: %s\n' "$*" >&2; exit 1; }

usage() {
    cat <<'EOF'
Reset the hosted room to the preparing state while preserving accounts and scenario data.

Usage:
  ./deploy/reset-room.sh --yes [--room-id main] [--backup-root DIRECTORY]

The command stops the deployed services, copies the complete /data volume to a
timestamped backup directory, clears the selected room's transient control
state, removes its runtime checkpoint/log files, and starts the services again.
EOF
}

docker_cmd() {
    if [[ $EUID -eq 0 ]] || docker info >/dev/null 2>&1; then
        docker "$@"
    else
        sudo docker "$@"
    fi
}

compose_cmd() {
    local project
    project="$(value_from_env WARGAME_COMPOSE_PROJECT wargame)"
    docker_cmd compose --project-name "$project" --env-file "$ENV_FILE" -f "$COMPOSE_FILE" "$@"
}

value_from_env() {
    local key="$1" fallback="$2" value
    value="$(sed -n "s/^${key}=//p" "$ENV_FILE" | head -n1 || true)"
    printf '%s' "${value:-$fallback}"
}

restart_after_error() {
    local code=$?
    if (( SERVICES_STOPPED == 1 )); then
        log "Reset did not complete; restarting services without rebuilding images."
        compose_cmd up -d >/dev/null 2>&1 || true
    fi
    exit "$code"
}

trap restart_after_error ERR

while (($#)); do
    case "$1" in
        --yes) ASSUME_YES=1; shift ;;
        --room-id) ROOM_ID="${2:?missing value for --room-id}"; shift 2 ;;
        --backup-root) BACKUP_ROOT="${2:?missing value for --backup-root}"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown option: $1" ;;
    esac
done

[[ $ASSUME_YES -eq 1 ]] || die "refusing to reset persistent state without --yes"
[[ -f "$COMPOSE_FILE" ]] || die "Compose file not found: $COMPOSE_FILE"
[[ -f "$ENV_FILE" ]] || die "runtime environment file not found: $ENV_FILE"
[[ "$ROOM_ID" =~ ^[A-Za-z0-9_.-]{1,64}$ ]] || die "invalid room id: $ROOM_ID"
ACTIVE_ROOM_ID="$(value_from_env GAME_ROOM_ID main)"
[[ "$ROOM_ID" == "$ACTIVE_ROOM_ID" ]] \
    || die "room id must match the hosted GAME_ROOM_ID: $ACTIVE_ROOM_ID"
command -v docker >/dev/null 2>&1 || die "docker is required"
command -v curl >/dev/null 2>&1 || die "curl is required"

compose_cmd config --quiet

ACCOUNT_CONTAINER="$(compose_cmd ps -q account-web)"
GAME_CONTAINER="$(compose_cmd ps -q game-server)"
[[ -n "$ACCOUNT_CONTAINER" && -n "$GAME_CONTAINER" ]] || die "both deployed services must exist before reset"

ACCOUNT_MOUNT="$(docker_cmd inspect "$ACCOUNT_CONTAINER" --format '{{range .Mounts}}{{if eq .Destination "/data"}}{{.Type}}|{{.Name}}|{{.Source}}{{end}}{{end}}')"
GAME_MOUNT="$(docker_cmd inspect "$GAME_CONTAINER" --format '{{range .Mounts}}{{if eq .Destination "/data"}}{{.Type}}|{{.Name}}|{{.Source}}{{end}}{{end}}')"
[[ "$ACCOUNT_MOUNT" == volume\|* && "$ACCOUNT_MOUNT" == "$GAME_MOUNT" ]] \
    || die "account-web and game-server must share one named /data volume"

TIMESTAMP="$(date -u +%Y%m%d-%H%M%S)"
BACKUP_DIR="$BACKUP_ROOT/${ROOM_ID}-reset-$TIMESTAMP"
umask 077
mkdir -p "$BACKUP_DIR"

log "Stopping services that share ${ACCOUNT_MOUNT#volume|}"
compose_cmd stop account-web game-server
SERVICES_STOPPED=1

log "Backing up /data to $BACKUP_DIR/data"
docker_cmd cp "$ACCOUNT_CONTAINER:/data" "$BACKUP_DIR/data"
[[ -f "$BACKUP_DIR/data/wargame.db" ]] || die "backup did not contain wargame.db"

log "Clearing transient state for room $ROOM_ID"
compose_cmd run --rm --no-deps -e "RESET_ROOM_ID=$ROOM_ID" \
    --entrypoint python account-web -c '
import os
import sqlite3
from datetime import UTC, datetime
from pathlib import Path

room_id = os.environ["RESET_ROOM_ID"]
db = sqlite3.connect("/data/wargame.db", timeout=10)
try:
    db.execute("BEGIN IMMEDIATE")
    if db.execute("SELECT 1 FROM rooms WHERE room_id=?", (room_id,)).fetchone() is None:
        raise RuntimeError(f"room does not exist: {room_id}")
    now = datetime.now(UTC).replace(microsecond=0).isoformat().replace("+00:00", "Z")
    db.execute("UPDATE rooms SET status=?, updated_at=? WHERE room_id=?", ("preparing", now, room_id))
    for table in ("room_operations", "room_presence", "room_kick_requests"):
        db.execute(f"DELETE FROM {table} WHERE room_id=?", (room_id,))
    db.commit()
finally:
    db.close()

for filename in ("room-checkpoint.json", "room-commands.jsonl", "game-events.jsonl", "game-status.json"):
    Path("/data", filename).unlink(missing_ok=True)
print(f"room {room_id} reset")
'

log "Starting services"
compose_cmd up -d
SERVICES_STOPPED=0

for ((attempt = 1; attempt <= STARTUP_TIMEOUT_SECONDS; attempt++)); do
    ACCOUNT_CONTAINER="$(compose_cmd ps -q account-web)"
    GAME_CONTAINER="$(compose_cmd ps -q game-server)"
    if [[ -n "$ACCOUNT_CONTAINER" && -n "$GAME_CONTAINER" ]] \
        && [[ "$(docker_cmd inspect "$ACCOUNT_CONTAINER" --format '{{.State.Health.Status}}')" == "healthy" ]] \
        && [[ "$(docker_cmd inspect "$GAME_CONTAINER" --format '{{.State.Health.Status}}')" == "healthy" ]]; then
        break
    fi
    sleep 1
done

[[ "$(docker_cmd inspect "$ACCOUNT_CONTAINER" --format '{{.State.Health.Status}}')" == "healthy" ]] \
    || die "account-web did not become healthy"
[[ "$(docker_cmd inspect "$GAME_CONTAINER" --format '{{.State.Health.Status}}')" == "healthy" ]] \
    || die "game-server did not become healthy"

HTTP_PORT="$(value_from_env HTTP_PORT 8080)"
HOST_BIND_ADDRESS="$(value_from_env HOST_BIND_ADDRESS 127.0.0.1)"
[[ "$HOST_BIND_ADDRESS" == "0.0.0.0" ]] && HOST_BIND_ADDRESS="127.0.0.1"
ROOMS_JSON="$(curl -fsS --max-time 5 "http://${HOST_BIND_ADDRESS}:${HTTP_PORT}/api/rooms")"
printf '%s' "$ROOMS_JSON" | compose_cmd exec -T -e "RESET_ROOM_ID=$ROOM_ID" account-web python -c '
import json
import os
import sys

room_id = os.environ["RESET_ROOM_ID"]
rooms = json.load(sys.stdin)["rooms"]
room = next((item for item in rooms if item["roomId"] == room_id), None)
if room is None or room.get("status") != "preparing" or room.get("pendingOperation") is not None:
    raise SystemExit("room was not reset to preparing without a pending operation")
'

log "Completed. Backup: $BACKUP_DIR"
