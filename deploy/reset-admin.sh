#!/usr/bin/env bash
set -eu

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CURRENT_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
if [[ -f "$CURRENT_ROOT/../.wargame-install" ]]; then
    INSTALL_ROOT="$(cd -- "$CURRENT_ROOT/.." && pwd)"
    CURRENT_ROOT="$INSTALL_ROOT/current"
else
    INSTALL_ROOT="$CURRENT_ROOT"
fi
COMPOSE_FILE="$CURRENT_ROOT/deploy/compose.yml"
ENV_FILE="$INSTALL_ROOT/.env"
[[ -f "$COMPOSE_FILE" ]] || { printf 'Compose file not found: %s\n' "$COMPOSE_FILE" >&2; exit 1; }
[[ -f "$ENV_FILE" ]] || { printf 'runtime environment file not found: %s\n' "$ENV_FILE" >&2; exit 1; }
PROJECT="$(sed -n 's/^WARGAME_COMPOSE_PROJECT=//p' "$ENV_FILE" | head -n1)"
PROJECT="${PROJECT:-wargame}"
docker_cmd() {
    if [[ $EUID -eq 0 ]] || docker info >/dev/null 2>&1; then docker "$@"; else sudo docker "$@"; fi
}
compose_cmd() { docker_cmd compose --project-name "$PROJECT" --env-file "$ENV_FILE" -f "$COMPOSE_FILE" "$@"; }

if [ "$#" -gt 0 ]; then
    printf '密码不能通过命令行参数传入；请使用交互输入或 stdin\n' >&2
    exit 2
fi
if [[ -t 0 ]]; then
    read -r -s -p "新的管理员密码: " password
    printf '\n'
else
    IFS= read -r password || { printf '无法从 stdin 读取管理员密码\n' >&2; exit 2; }
fi
printf '%s\n' "$password" | compose_cmd exec -T account-web python /app/reset_admin.py
