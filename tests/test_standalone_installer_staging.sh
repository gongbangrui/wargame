#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/wargame-installer-staging.XXXXXX")"
BIN_DIR="$WORK_DIR/bin"
DOCKER_LOG="$WORK_DIR/docker.log"
mkdir -p "$BIN_DIR" "$WORK_DIR/home"
trap 'rm -rf -- "$WORK_DIR"' EXIT

cat >"$BIN_DIR/docker" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"${DOCKER_LOG:?}"
if [[ "${NO_BUILD_IMAGE_MISSING:-0}" == 1 && "$*" == "image inspect "* ]]; then
    exit 1
fi
if [[ "${FAIL_UP:-0}" == 1 && "$*" == *" up "* ]]; then
    exit 1
fi
if [[ "$*" == *" ps --status running"* ]]; then
    printf '%s\n' 'game-server healthy'
fi
EOF
cat >"$BIN_DIR/curl" <<'EOF'
#!/usr/bin/env bash
fixture_root="${FIXTURE_INSTALL_ROOT:-${HOME:?}/wargame}"
source_digest="$(sed -n 's/^WARGAME_SOURCE_DIGEST=//p' "$fixture_root/.env" | head -n1)"
case "$*" in
    *'/api/health'*) printf '{"status":"ok","sourceDigest":"%s"}\n' "$source_digest" ;;
    *'/api/admin/login'*) printf '{"token":"fixture-token"}\n' ;;
    *'/api/admin/monitor/overview'*) printf '{"accountStatus":"healthy","status":"healthy","sourceDigest":"%s"}\n' "$source_digest" ;;
    *'/api/admin/monitor/terminal/login'*) printf '{"authenticated":true,"terminalTicket":"fixture-ticket"}\n' ;;
    *) printf '{}\n' ;;
esac
EOF
chmod +x "$BIN_DIR/docker" "$BIN_DIR/curl"

archive="$WORK_DIR/wargame-server-fixture.tar.gz"
grep -Fqx 'AI_PROVIDER=auto' "$ROOT_DIR/deploy/.env.example"
grep -Fqx 'OLLAMA_BASE_URL=http://host.docker.internal:11434' "$ROOT_DIR/deploy/.env.example"
grep -Fqx 'OLLAMA_MODEL=auto' "$ROOT_DIR/deploy/.env.example"
grep -Fqx '      OLLAMA_MODEL: "${OLLAMA_MODEL:-auto}"' "$ROOT_DIR/deploy/compose.yml"
grep -Fqx 'OLLAMA_MODEL="auto"' <(sed -n '1,80p' "$ROOT_DIR/deploy/install-server.sh")
tar -C "$ROOT_DIR" \
    --exclude='server/account/*.db*' \
    --exclude='server/account/*.log' \
    --exclude='server/account/backups' \
    --exclude='build' --exclude='dist' --exclude='.codegraph' \
    --transform='s,^,wargame-server-fixture/,' \
    -czf "$archive" CMakeLists.txt cmake src server map/metadata.json deploy .dockerignore README.md docs

run_installer() {
    PATH="$BIN_DIR:$PATH" DOCKER_LOG="$DOCKER_LOG" HOME="$WORK_DIR/home" \
        FIXTURE_INSTALL_ROOT="$install_root" \
        NO_BUILD_IMAGE_MISSING="${NO_BUILD_IMAGE_MISSING:-0}" \
        ADMIN_PASSWORD=fixture-password \
        bash "$ROOT_DIR/deploy/install-server.sh" \
        --source "$archive" --install-dir "$WORK_DIR/home/wargame" \
        --compose-project fixture \
        --skip-environment-check --yes "$@"
}

run_source_installer() {
    local source_path="$1" target_root="$2"
    shift 2
    PATH="$BIN_DIR:$PATH" DOCKER_LOG="$DOCKER_LOG" HOME="$WORK_DIR/home" \
        FIXTURE_INSTALL_ROOT="$target_root" \
        NO_BUILD_IMAGE_MISSING="${NO_BUILD_IMAGE_MISSING:-0}" \
        ADMIN_PASSWORD=fixture-password \
        bash "$ROOT_DIR/deploy/install-server.sh" \
        --source "$source_path" --install-dir "$target_root" \
        --compose-project fixture --skip-environment-check --yes "$@"
}

install_root="$WORK_DIR/home/wargame"
run_installer
test -f "$install_root/.wargame-install"
test -f "$install_root/.env"
test -x "$install_root/current/deploy/install-server.sh"
test -f "$install_root/current/server/account/static/index.html"
test ! -e "$install_root/current/server/account/__pycache__"
first_digest="$(sed -n 's/^WARGAME_SOURCE_DIGEST=//p' "$install_root/.env" | head -n1)"
test -n "$first_digest"
grep -Fqx 'WARGAME_ENABLE_FASTDDS=OFF' "$install_root/.env"
grep -Fqx 'WARGAME_FASTDDS_MODE=disabled' "$install_root/.env"
grep -Fqx 'FASTDDS_DOMAIN_ID=0' "$install_root/.env"
grep -Fqx 'FASTDDS_DISCOVERY_TIMEOUT_MS=5000' "$install_root/.env"
grep -Fqx 'FASTDDS_STATIC_PEERS=' "$install_root/.env"
grep -Fqx "OLLAMA_MODEL='auto'" "$install_root/.env"
grep -Fq -- "--project-name fixture" "$DOCKER_LOG"
grep -Fq -- "-f $install_root/current/deploy/compose.yml" "$DOCKER_LOG"

if run_installer --bind-address 0.0.0.0 --public-game-ws-url wss://game.example; then
    printf 'non-loopback plaintext deployment was accepted\n' >&2
    exit 1
fi

sed -i 's/^WARGAME_DATA_VOLUME=.*/WARGAME_DATA_VOLUME=fixture-volume/' "$install_root/.env"
sed -i \
    -e 's/^WARGAME_ENABLE_FASTDDS=.*/WARGAME_ENABLE_FASTDDS=OFF/' \
    -e 's/^WARGAME_FASTDDS_MODE=.*/WARGAME_FASTDDS_MODE=disabled/' \
    -e 's/^FASTDDS_DOMAIN_ID=.*/FASTDDS_DOMAIN_ID=7/' \
    -e 's/^FASTDDS_DISCOVERY_TIMEOUT_MS=.*/FASTDDS_DISCOVERY_TIMEOUT_MS=1200/' \
    -e 's/^FASTDDS_STATIC_PEERS=.*/FASTDDS_STATIC_PEERS=10.0.0.2:7400/' \
    -e 's/^AI_PROVIDER=.*/AI_PROVIDER=ollama/' \
    -e 's#^OLLAMA_BASE_URL=.*#OLLAMA_BASE_URL=\x27http://127.0.0.1:11434\x27#' \
    -e 's/^OLLAMA_MODEL=.*/OLLAMA_MODEL=\x27explicit-model\x27/' \
    -e 's/^OLLAMA_CONNECT_TIMEOUT_MS=.*/OLLAMA_CONNECT_TIMEOUT_MS=1200/' \
    -e 's/^OLLAMA_TIMEOUT_MS=.*/OLLAMA_TIMEOUT_MS=45000/' \
    -e 's/^OLLAMA_MAX_RESPONSE_BYTES=.*/OLLAMA_MAX_RESPONSE_BYTES=32768/' \
    "$install_root/.env"
run_installer --reuse-admin-password --no-reset-admin
second_digest="$(sed -n 's/^WARGAME_SOURCE_DIGEST=//p' "$install_root/.env" | head -n1)"
test "$first_digest" = "$second_digest"
grep -Fqx 'WARGAME_DATA_VOLUME=fixture-volume' "$install_root/.env"
grep -Fqx 'WARGAME_ENABLE_FASTDDS=OFF' "$install_root/.env"
grep -Fqx 'WARGAME_FASTDDS_MODE=disabled' "$install_root/.env"
grep -Fqx 'FASTDDS_DOMAIN_ID=7' "$install_root/.env"
grep -Fqx 'FASTDDS_DISCOVERY_TIMEOUT_MS=1200' "$install_root/.env"
grep -Fqx 'FASTDDS_STATIC_PEERS=10.0.0.2:7400' "$install_root/.env"
grep -Fqx 'AI_PROVIDER=ollama' "$install_root/.env"
grep -Fqx "OLLAMA_BASE_URL='http://127.0.0.1:11434'" "$install_root/.env"
grep -Fqx "OLLAMA_MODEL='explicit-model'" "$install_root/.env"
grep -Fqx 'OLLAMA_CONNECT_TIMEOUT_MS=1200' "$install_root/.env"
grep -Fqx 'OLLAMA_TIMEOUT_MS=45000' "$install_root/.env"

sed -i 's/^OLLAMA_TIMEOUT_MS=.*/OLLAMA_TIMEOUT_MS=15000/' "$install_root/.env"
run_installer --reuse-admin-password --no-reset-admin
grep -Fqx 'OLLAMA_TIMEOUT_MS=60000' "$install_root/.env"
grep -Fqx 'OLLAMA_MAX_RESPONSE_BYTES=32768' "$install_root/.env"

if NO_BUILD_IMAGE_MISSING=1 run_installer --reuse-admin-password --no-reset-admin --no-build; then
    printf '%s\n' '--no-build accepted a missing release-tagged image' >&2
    exit 1
fi
test "$second_digest" = "$(sed -n 's/^WARGAME_SOURCE_DIGEST=//p' "$install_root/.env" | head -n1)"

bad_archive="$WORK_DIR/bad.tar.gz"
printf 'not an archive\n' >"$bad_archive"
bad_root="$WORK_DIR/bad-install"
if PATH="$BIN_DIR:$PATH" DOCKER_LOG="$DOCKER_LOG" HOME="$WORK_DIR/home" \
    bash "$ROOT_DIR/deploy/install-server.sh" --source "$bad_archive" \
    --install-dir "$bad_root" --compose-project bad --skip-environment-check --yes; then
    printf 'malformed archive was accepted\n' >&2
    exit 1
fi
test ! -e "$bad_root/.env"
test ! -e "$bad_root/.wargame-install"

raw_source="$WORK_DIR/raw-source"
raw_install="$WORK_DIR/home/raw-wargame"
mkdir -p "$raw_source"
tar -xzf "$archive" --strip-components=1 -C "$raw_source"
mkdir -p "$raw_source/server/account/backups" "$raw_source/server/account/.pytest_cache"
printf 'runtime secret\n' >"$raw_source/deploy/.env"
printf 'sqlite fixture\n' >"$raw_source/server/account/runtime.db"
printf '{"event":"fixture"}\n' >"$raw_source/server/account/events.jsonl"
printf 'fixture log\n' >"$raw_source/server/account/debug.log"
printf 'checkpoint\n' >"$raw_source/server/account/room-checkpoint.json"
printf 'backup\n' >"$raw_source/server/account/backups/fixture.txt"
printf 'cache\n' >"$raw_source/server/account/.pytest_cache/fixture"
run_source_installer "$raw_source" "$raw_install"
raw_first_digest="$(sed -n 's/^WARGAME_SOURCE_DIGEST=//p' "$raw_install/.env" | head -n1)"
test -n "$raw_first_digest"
test -f "$raw_install/current/deploy/.env.example"
test ! -e "$raw_install/current/deploy/.env"
test ! -e "$raw_install/current/server/account/runtime.db"
test ! -e "$raw_install/current/server/account/events.jsonl"
test ! -e "$raw_install/current/server/account/debug.log"
test ! -e "$raw_install/current/server/account/room-checkpoint.json"
test ! -e "$raw_install/current/server/account/backups"
test ! -e "$raw_install/current/server/account/.pytest_cache"
printf 'changed runtime secret\n' >"$raw_source/deploy/.env"
printf 'changed sqlite fixture\n' >"$raw_source/server/account/runtime.db"
run_source_installer "$raw_source" "$raw_install" --reuse-admin-password --no-reset-admin
raw_second_digest="$(sed -n 's/^WARGAME_SOURCE_DIGEST=//p' "$raw_install/.env" | head -n1)"
test "$raw_first_digest" = "$raw_second_digest"

marker_before="$(sha256sum "$install_root/.wargame-install" | awk '{print $1}')"
if FAIL_UP=1 PATH="$BIN_DIR:$PATH" DOCKER_LOG="$DOCKER_LOG" HOME="$WORK_DIR/home" \
    bash "$ROOT_DIR/deploy/install-server.sh" --source "$archive" \
    --install-dir "$install_root" --compose-project fixture \
    --reuse-admin-password --no-reset-admin --skip-environment-check --yes; then
    printf 'forced activation failure was accepted\n' >&2
    exit 1
fi
test -f "$install_root/current/deploy/compose.yml"
test "$marker_before" = "$(sha256sum "$install_root/.wargame-install" | awk '{print $1}')"
find "$install_root/backups" -maxdepth 1 -type d -name 'failed-staging-*' -print -quit | grep -q .

printf 'archive_install=ok\n'
printf 'update_volume=preserved\n'
printf 'malformed_archive=refused\n'
printf 'activation_failure=rolled_back\n'
printf 'raw_source_artifacts=excluded\n'
printf 'raw_source_digest=stable\n'
