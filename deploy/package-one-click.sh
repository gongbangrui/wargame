#!/usr/bin/env bash
set -Eeuo pipefail

# Build a source bundle that can run deploy/install-server.sh on a Linux host.

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
DIST_DIR="${DIST_DIR:-$ROOT_DIR/dist}"
PROJECT_VERSION="$(sed -nE 's/^project\([^)]* VERSION ([0-9][0-9A-Za-z._-]*).*/\1/p' "$ROOT_DIR/CMakeLists.txt" | head -n1)"
VERSION="${WARGAME_VERSION:-${PROJECT_VERSION:-1.0.0}}"
PACKAGE_ROOT="wargame-server-${VERSION}"
ARCHIVE="$DIST_DIR/${PACKAGE_ROOT}.tar.gz"
CHECKSUM="$ARCHIVE.sha256"
TEMP_ARCHIVE=""
TEMP_CHECKSUM=""
PUBLISHED_ARCHIVE=0

die() { printf '[package-one-click] error: %s\n' "$*" >&2; exit 1; }
cleanup() {
    [[ -z "$TEMP_ARCHIVE" ]] || rm -f -- "$TEMP_ARCHIVE"
    [[ -z "$TEMP_CHECKSUM" ]] || rm -f -- "$TEMP_CHECKSUM"
    (( PUBLISHED_ARCHIVE == 0 )) || rm -f -- "$ARCHIVE" "$CHECKSUM"
}
trap cleanup EXIT

PACKAGE_INPUTS=(
    CMakeLists.txt
    cmake
    src
    server
    map/metadata.json
    deploy
    .dockerignore
    README.md
    docs/ONLINE_DEPLOYMENT.md
    docs/RELEASE.md
)

is_prohibited_path() {
    local path="$1"
    local base="${path##*/}"

    [[ "$path" == "deploy/.env.example" ]] && return 1

    case "$path" in
        .env*|*/.env*|build|build/*|*/build|*/build/*|cache|cache/*|*/cache|*/cache/*|.cache|.cache/*|*/.cache|*/.cache/*|logs|logs/*|*/logs|*/logs/*|backups|backups/*|*/backups|*/backups/*|__pycache__|__pycache__/*|*/__pycache__|*/__pycache__/*)
            return 0
            ;;
    esac

    case "$base" in
        *.db|*.db-*|*.sqlite|*.sqlite-*|*.sqlite3|*.sqlite3-*|*.jsonl|*.log|*.cache|*.pyc|*.pyo|*checkpoint*|*status*|*events*)
            return 0
            ;;
    esac

    return 1
}

validate_package_inputs() {
    local candidate relative
    while IFS= read -r -d '' candidate; do
        relative="${candidate#"$ROOT_DIR"/}"
        if is_prohibited_path "$relative"; then
            die "refusing to package prohibited runtime artifact: $relative"
        fi
    done < <(find "${PACKAGE_INPUTS[@]/#/$ROOT_DIR/}" -mindepth 1 -print0)
}

require_archive_entry() {
    local entry="$1"
    grep -Fqx "$entry" <<<"$LISTING" || die "archive is missing: $entry"
}

[[ "$VERSION" =~ ^[A-Za-z0-9._-]{1,32}$ ]] || die "invalid WARGAME_VERSION: $VERSION"
for path in "${PACKAGE_INPUTS[@]}" deploy/QUICK_START.md; do
    [[ -e "$ROOT_DIR/$path" ]] || die "required package input is missing: $path"
done

validate_package_inputs
mkdir -p "$DIST_DIR"
[[ ! -e "$ARCHIVE" && ! -e "$CHECKSUM" ]] \
    || die "final archive or checksum already exists: $ARCHIVE"
TEMP_ARCHIVE="$(mktemp "$DIST_DIR/.${PACKAGE_ROOT}.XXXXXX.tar.gz")"
TEMP_CHECKSUM="$(mktemp "$DIST_DIR/.${PACKAGE_ROOT}.XXXXXX.sha256")"

# Keep the release bytes reproducible across hosts and source checkout mtimes.
tar -C "$ROOT_DIR" \
    --exclude='deploy/.env.example' \
    --sort=name \
    --mtime='1970-01-01 00:00:00Z' \
    --owner=0 \
    --group=0 \
    --numeric-owner \
    --transform="s,^,${PACKAGE_ROOT}/," \
    -cf - \
    "${PACKAGE_INPUTS[@]}" \
    | gzip -n >"$TEMP_ARCHIVE"

LISTING="$(tar -tzf "$TEMP_ARCHIVE")"
for required in \
    "$PACKAGE_ROOT/deploy/install-server.sh" \
    "$PACKAGE_ROOT/deploy/compose.yml" \
    "$PACKAGE_ROOT/deploy/account.Dockerfile" \
    "$PACKAGE_ROOT/deploy/game-server.Dockerfile" \
    "$PACKAGE_ROOT/server/account/app.py" \
    "$PACKAGE_ROOT/server/account/ai_conversation_monitor.py" \
    "$PACKAGE_ROOT/server/account/ai_conversation_monitor_models.py" \
    "$PACKAGE_ROOT/server/account/static/index.html" \
    "$PACKAGE_ROOT/server/account/static/app.js" \
    "$PACKAGE_ROOT/server/account/static/app.css" \
    "$PACKAGE_ROOT/server/game/main.cpp" \
    "$PACKAGE_ROOT/map/metadata.json" \
    "$PACKAGE_ROOT/deploy/QUICK_START.md" \
    "$PACKAGE_ROOT/src"; do
    if [[ "$required" == "$PACKAGE_ROOT/src" ]]; then
        grep -Fq "${required}/" <<<"$LISTING" || die "archive is missing: $required"
    else
        require_archive_entry "$required"
    fi
done

while IFS= read -r entry; do
    if is_prohibited_path "${entry#"$PACKAGE_ROOT"/}"; then
        die "archive unexpectedly contains prohibited runtime artifact: $entry"
    fi
done <<<"$LISTING"

checksum_value="$(sha256sum "$TEMP_ARCHIVE" | awk '{print $1}')"
printf '%s  %s\n' "$checksum_value" "$(basename "$ARCHIVE")" >"$TEMP_CHECKSUM"

mv "$TEMP_ARCHIVE" "$ARCHIVE"
TEMP_ARCHIVE=""
PUBLISHED_ARCHIVE=1
mv "$TEMP_CHECKSUM" "$CHECKSUM"
TEMP_CHECKSUM=""
PUBLISHED_ARCHIVE=0
printf '%s\n%s\n' "$ARCHIVE" "$CHECKSUM"
