#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
INSTALLER="$ROOT_DIR/deploy/install-server.sh"
WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/wargame-runtime-layout.XXXXXX")"
BIN_DIR="$WORK_DIR/bin"
DOCKER_LOG="$WORK_DIR/docker.log"
mkdir -p "$BIN_DIR" "$WORK_DIR/home"
trap 'rm -rf -- "$WORK_DIR"' EXIT

cat >"$BIN_DIR/docker" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"${DOCKER_LOG:?}"
if [[ "$*" == *" ps --status running"* ]]; then
  printf '%s\n' 'game-server healthy'
elif [[ "$*" == *" exec "* ]]; then
  :
fi
EOF
cat >"$BIN_DIR/curl" <<'EOF'
#!/usr/bin/env bash
source_digest="$(sed -n 's/^WARGAME_SOURCE_DIGEST=//p' "${HOME:?}/wargame/.env" | head -n1)"
case "$*" in
  *'/api/health'*) printf '{"sourceDigest":"%s"}\n' "$source_digest" ;;
  *'/api/admin/login'*) printf '{"token":"fixture-token"}\n' ;;
  *'/api/admin/monitor/overview'*) printf '{"accountStatus":"healthy","status":"healthy","sourceDigest":"%s"}\n' "$source_digest" ;;
  *'/api/admin/monitor/terminal/login'*) printf '{"authenticated":true,"terminalTicket":"fixture-ticket"}\n' ;;
  *) printf '{}\n' ;;
esac
EOF
chmod +x "$BIN_DIR/docker" "$BIN_DIR/curl"

run_installer() {
  PATH="$BIN_DIR:$PATH" DOCKER_LOG="$DOCKER_LOG" HOME="$WORK_DIR/home" \
    ADMIN_PASSWORD=fixture-password \
    bash "$INSTALLER" --skip-environment-check --yes "$@"
}

install_root="$WORK_DIR/home/wargame"
run_installer --install-dir "$install_root" --compose-project fixture
test -d "$install_root/current"
test -d "$install_root/backups"
test -f "$install_root/.wargame-install"
test -f "$install_root/.env"
test "$(stat -c '%a' "$install_root/.env")" = 600
grep -Eq '^WARGAME_COMPOSE_PROJECT=fixture$' "$install_root/.env"
grep -Eq "^WARGAME_RUNTIME_ENV_FILE=$install_root/.env$" "$install_root/.env"
grep -Eq -- '--project-name fixture' "$DOCKER_LOG"

assert_rejected_without_docker() {
  local target="$1"
  : >"$DOCKER_LOG"
  if [[ "$target" == "$WORK_DIR"/* ]]; then
    rm -rf -- "$target"
  fi
  if run_installer --install-dir "$target" --compose-project rejected; then
    printf 'expected rejection: %s\n' "$target" >&2
    return 1
  fi
  if [[ "$target" == "$WORK_DIR"/* ]]; then
    test ! -e "$target/.env"
    test ! -e "$target/.wargame-install"
  fi
  test ! -s "$DOCKER_LOG"
}

assert_rejected_without_docker "/"
assert_rejected_without_docker "$WORK_DIR/../escape"
assert_rejected_without_docker "$ROOT_DIR"
: >"$DOCKER_LOG"
if run_installer --install-dir "" --compose-project rejected; then
  printf 'expected empty-path rejection\n' >&2
  exit 1
fi
test ! -s "$DOCKER_LOG"
: >"$DOCKER_LOG"
if run_installer --install-dir ../wargame-runtime-relative --compose-project rejected; then
  printf 'expected relative rejection\n' >&2
  exit 1
fi
test ! -s "$DOCKER_LOG"
mkdir -p "$WORK_DIR/unmarked"
: >"$DOCKER_LOG"
if run_installer --install-dir "$WORK_DIR/unmarked" --compose-project rejected; then
  printf 'expected unmarked-root rejection\n' >&2
  exit 1
fi
test -d "$WORK_DIR/unmarked"
test ! -e "$WORK_DIR/unmarked/.env"
test ! -e "$WORK_DIR/unmarked/.wargame-install"
test ! -s "$DOCKER_LOG"
mkdir -p "$WORK_DIR/symlink-target"
ln -s "$WORK_DIR/symlink-target" "$WORK_DIR/symlink-root"
if run_installer --install-dir "$WORK_DIR/symlink-root" --compose-project rejected; then
  printf 'expected symlink rejection\n' >&2
  exit 1
fi
test ! -s "$DOCKER_LOG"

printf 'runtime_layout=ok\n'
printf 'invalid_inputs=no_docker\n'
