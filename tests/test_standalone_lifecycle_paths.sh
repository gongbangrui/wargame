#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/wargame-lifecycle.XXXXXX")"
BIN_DIR="$WORK_DIR/bin"
LOG="$WORK_DIR/docker.log"
INSTALL_ROOT="$WORK_DIR/home/wargame"
mkdir -p "$BIN_DIR" "$INSTALL_ROOT/current/deploy" "$INSTALL_ROOT/backups"
trap 'rm -rf -- "$WORK_DIR"' EXIT

cat >"$BIN_DIR/docker" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"${DOCKER_LOG:?}"
exit 0
EOF
chmod +x "$BIN_DIR/docker"
cp "$ROOT_DIR/deploy/compose.yml" "$INSTALL_ROOT/current/deploy/compose.yml"
cp "$ROOT_DIR/CMakeLists.txt" "$INSTALL_ROOT/current/CMakeLists.txt"
cp "$ROOT_DIR/deploy/reset-admin.sh" "$INSTALL_ROOT/current/deploy/reset-admin.sh"
cp "$ROOT_DIR/deploy/reset-room.sh" "$INSTALL_ROOT/current/deploy/reset-room.sh"
cp "$ROOT_DIR/deploy/uninstall-server.sh" "$INSTALL_ROOT/current/deploy/uninstall-server.sh"
cp "$ROOT_DIR/deploy/install-server.sh" "$INSTALL_ROOT/current/deploy/install-server.sh"
cp "$ROOT_DIR/deploy/install-server-no-check.sh" "$INSTALL_ROOT/current/deploy/install-server-no-check.sh"
cp "$ROOT_DIR/deploy/release-lib.sh" "$INSTALL_ROOT/current/deploy/release-lib.sh"
printf 'WARGAME_COMPOSE_PROJECT=fixture\nWARGAME_DATA_VOLUME=fixture-volume\n' >"$INSTALL_ROOT/.env"
printf 'version=1\n' >"$INSTALL_ROOT/.wargame-install"

printf '%s\n' fixture-password | PATH="$BIN_DIR:$PATH" DOCKER_LOG="$LOG" \
    bash "$INSTALL_ROOT/current/deploy/reset-admin.sh"
grep -Fq -- "compose --project-name fixture --env-file $INSTALL_ROOT/.env -f $INSTALL_ROOT/current/deploy/compose.yml exec -T account-web" "$LOG"

: >"$LOG"
PATH="$BIN_DIR:$PATH" DOCKER_LOG="$LOG" \
    bash "$INSTALL_ROOT/current/deploy/uninstall-server.sh" --yes
grep -Fq -- "compose --project-name fixture --env-file $INSTALL_ROOT/.env -f $INSTALL_ROOT/current/deploy/compose.yml down" "$LOG"
! grep -Fq -- 'volume rm fixture-volume' "$LOG"
test -f "$INSTALL_ROOT/.env"

: >"$LOG"
PATH="$BIN_DIR:$PATH" DOCKER_LOG="$LOG" \
    bash "$INSTALL_ROOT/current/deploy/install-server-no-check.sh" --help >/dev/null

printf 'managed_root=resolved\n'
printf 'compose_project=fixture\n'
printf 'ordinary_uninstall=preserves_config_and_volume\n'
