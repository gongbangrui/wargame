#!/usr/bin/env bash
set -euo pipefail

# 在独立 Docker 项目和命名卷中验证：优雅停止最终落盘、备份、还原、重启恢复。
root_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
compose_file="$root_dir/deploy/compose.yml"
run_id="recovery-$(date +%s)-$RANDOM"
source_project="wargame-$run_id-source"
restore_project="wargame-$run_id-restore"
source_volume="wargame-$run_id-source-data"
restore_volume="wargame-$run_id-restore-data"
temp_dir="$(mktemp -d)"
archive="$temp_dir/wargame-data.tar"
source_env="$temp_dir/source.env"
restore_env="$temp_dir/restore.env"
http_port="${RECOVERY_HTTP_PORT:-18080}"
ws_port="${RECOVERY_WS_PORT:-18090}"
restore_http_port="$((http_port + 1))"
restore_ws_port="$((ws_port + 1))"
admin_password="Recovery-${run_id}-Pass"

cleanup() {
  docker compose -p "$source_project" --env-file "$source_env" -f "$compose_file" down -v --remove-orphans >/dev/null 2>&1 || true
  docker compose -p "$restore_project" --env-file "$restore_env" -f "$compose_file" down -v --remove-orphans >/dev/null 2>&1 || true
  docker volume rm "$source_volume" "$restore_volume" >/dev/null 2>&1 || true
  rm -rf "$temp_dir"
}
trap cleanup EXIT

write_env() {
  local path="$1" volume="$2" account_port="$3" game_port="$4"
  cat >"$path" <<EOF
ADMIN_USERNAME=admin
ADMIN_PASSWORD=$admin_password
INTERNAL_API_KEY=recovery-$run_id-internal-key
WARGAME_RUNTIME_ENV_FILE=$path
HOST_BIND_ADDRESS=127.0.0.1
HTTP_PORT=$account_port
WS_PORT=$game_port
PUBLIC_GAME_WS_URL=ws://127.0.0.1:$game_port
WARGAME_DATA_VOLUME=$volume
WEB_SHELL_ENABLED=false
EOF
}

wait_healthy() {
  local project="$1" env_file="$2"
  local deadline=$((SECONDS + 180))
  while (( SECONDS < deadline )); do
    local ps_output healthy
    ps_output="$(docker compose -p "$project" --env-file "$env_file" -f "$compose_file" ps --format json)"
    healthy="$(printf '%s\n' "$ps_output" \
      | grep -Eci '"health"[[:space:]]*:[[:space:]]*"healthy"' || true)"
    [[ "$healthy" -eq 2 ]] && return 0
    sleep 2
  done
  docker compose -p "$project" --env-file "$env_file" -f "$compose_file" ps >&2 || true
  docker compose -p "$project" --env-file "$env_file" -f "$compose_file" logs >&2 || true
  echo "等待 Docker 服务健康检查超时。" >&2
  return 1
}

write_env "$source_env" "$source_volume" "$http_port" "$ws_port"
write_env "$restore_env" "$restore_volume" "$restore_http_port" "$restore_ws_port"

docker compose -p "$source_project" --env-file "$source_env" -f "$compose_file" up -d --build
wait_healthy "$source_project" "$source_env"
ADMIN_PASSWORD="$admin_password" ACCOUNT_URL="http://127.0.0.1:$http_port" node "$root_dir/tools/network-smoke.mjs"

docker compose -p "$source_project" --env-file "$source_env" -f "$compose_file" stop --timeout 30 game-server
docker compose -p "$source_project" --env-file "$source_env" -f "$compose_file" up -d game-server
wait_healthy "$source_project" "$source_env"

docker run --rm -v "$source_volume:/source:ro" -v "$temp_dir:/backup" ubuntu:24.04 \
  tar -C /source -cf /backup/wargame-data.tar .
docker volume create "$restore_volume" >/dev/null
docker run --rm -v "$restore_volume:/target" -v "$temp_dir:/backup:ro" ubuntu:24.04 \
  tar -C /target -xf /backup/wargame-data.tar

docker compose -p "$restore_project" --env-file "$restore_env" -f "$compose_file" up -d --build
wait_healthy "$restore_project" "$restore_env"
ADMIN_PASSWORD="$admin_password" ACCOUNT_URL="http://127.0.0.1:$restore_http_port" node "$root_dir/tools/network-smoke.mjs"

echo "Docker 持久化验证通过：SIGTERM 落盘、卷备份、还原和重启恢复均成功。"
