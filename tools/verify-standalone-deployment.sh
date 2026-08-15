#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/wargame-standalone-verify.XXXXXX")"
SOURCE_COPY="$WORK_DIR/source"
DIST_DIR="$WORK_DIR/dist"
EXTRACT_DIR="$WORK_DIR/extracted"
INSTALL_ROOT="$WORK_DIR/home/wargame"
PROJECT="standalone-verify-$$"
DATA_VOLUME="wargame-data-standalone-$$"
HTTP_PORT="${RECOVERY_HTTP_PORT:-18180}"
WS_PORT="${RECOVERY_WS_PORT:-18190}"
ARCHIVE=""
ENV_FILE="$INSTALL_ROOT/.env"
COMPOSE_FILE="$INSTALL_ROOT/current/deploy/compose.yml"

cleanup() {
    set +e
    if [[ -f "$ENV_FILE" && -f "$COMPOSE_FILE" ]]; then
        docker compose --project-name "$PROJECT" --env-file "$ENV_FILE" -f "$COMPOSE_FILE" down --volumes --remove-orphans >/dev/null 2>&1
    fi
    if docker volume inspect "$DATA_VOLUME" >/dev/null 2>&1; then
        docker volume rm "$DATA_VOLUME" >/dev/null 2>&1
    fi
    rm -rf -- "$WORK_DIR"
}
trap cleanup EXIT INT TERM

log() { printf '[standalone-verify] %s\n' "$*"; }
fail() { printf '[standalone-verify] error: %s\n' "$*" >&2; exit 1; }

command -v docker >/dev/null 2>&1 || fail "docker is required"
docker compose version >/dev/null 2>&1 || fail "Docker Compose v2 is required"
mkdir -p "$SOURCE_COPY" "$DIST_DIR" "$EXTRACT_DIR" "$WORK_DIR/home"

log "creating sanitized source snapshot"
tar -C "$ROOT_DIR" \
    --exclude='*/.env*' --exclude='.env*' \
    --exclude='*/__pycache__' --exclude='__pycache__' \
    --exclude='*.db' --exclude='*.db-*' --exclude='*.sqlite*' \
    --exclude='*.jsonl' --exclude='*.log' \
    --exclude='*/backups' --exclude='backups' \
    --exclude='*/build' --exclude='build' --exclude='*/cache' --exclude='cache' \
    --exclude='*/.omo' --exclude='.omo' \
    --exclude='.codegraph' --exclude='dist' \
    -cf - CMakeLists.txt Main.qml main.cpp qml cmake src server map/metadata.json deploy .dockerignore README.md docs \
    | tar -C "$SOURCE_COPY" -xf -

WARGAME_VERSION="standalone-$$" DIST_DIR="$DIST_DIR" \
    "$SOURCE_COPY/deploy/package-one-click.sh" >/dev/null
ARCHIVE="$DIST_DIR/wargame-server-standalone-$$.tar.gz"
[[ -f "$ARCHIVE" && -f "$ARCHIVE.sha256" ]] || fail "package output missing"
(cd "$DIST_DIR" && sha256sum -c "$(basename "$ARCHIVE.sha256")")

if [[ "${VERIFY_STANDALONE_CORRUPT:-0}" == 1 ]]; then
    printf 'corrupt\n' >>"$ARCHIVE"
    if (cd "$DIST_DIR" && sha256sum -c "$(basename "$ARCHIVE.sha256")"); then
        fail "corrupt archive unexpectedly passed checksum"
    fi
    log "corrupt archive rejected before Docker startup"
    exit 0
fi

tar -xzf "$ARCHIVE" -C "$EXTRACT_DIR"
PACKAGE_ROOT="$(find "$EXTRACT_DIR" -mindepth 1 -maxdepth 1 -type d -print -quit)"
[[ -n "$PACKAGE_ROOT" && -x "$PACKAGE_ROOT/deploy/install-server.sh" ]] || fail "extracted installer missing"

log "installing isolated archive with project=$PROJECT volume=$DATA_VOLUME ports=$HTTP_PORT/$WS_PORT"
HOME="$WORK_DIR/home" ADMIN_PASSWORD="fixture-password-$$" \
  "$PACKAGE_ROOT/deploy/install-server.sh" \
    --source "$ARCHIVE" --install-dir "$INSTALL_ROOT" \
    --compose-project "$PROJECT" --data-volume "$DATA_VOLUME" \
    --http-port "$HTTP_PORT" --ws-port "$WS_PORT" \
    --no-reset-admin --no-pull --yes

docker compose --project-name "$PROJECT" --env-file "$ENV_FILE" -f "$COMPOSE_FILE" ps
curl -fsS -i "http://127.0.0.1:${HTTP_PORT}/api/health" | tee "$WORK_DIR/health-response.txt"
grep -q '^HTTP/.* 200' "$WORK_DIR/health-response.txt" || fail "health endpoint did not return HTTP 200"
docker compose --project-name "$PROJECT" --env-file "$ENV_FILE" -f "$COMPOSE_FILE" ps --status running | grep -q 'account-web'
docker compose --project-name "$PROJECT" --env-file "$ENV_FILE" -f "$COMPOSE_FILE" ps --status running | grep -q 'game-server'
log "healthy services and HTTP 200 verified; cleanup trap will remove project, volume, ports, and temporary home"
