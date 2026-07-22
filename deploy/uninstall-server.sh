#!/usr/bin/env bash
set -Eeuo pipefail

# 兵器推演联网服务一键卸载器
# 默认保留账号、场景和对局数据；使用 --purge-data 才会删除数据卷。
# Copyright (c) 2026 Gbr

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
COMPOSE_FILE="$ROOT_DIR/deploy/compose.yml"
ENV_FILE="$ROOT_DIR/.env"
WARGAME_DATA_VOLUME="wargame-data"
PURGE_DATA=0
REMOVE_CONFIG=0
ASSUME_YES=0

log() { printf '  [Gbr] %s\n' "$*"; }
ok() { printf '  [ OK ] %s\n' "$*"; }
die() { printf '  [FAIL] %s\n' "$*" >&2; exit 1; }

usage() {
  cat <<'EOF'
兵器推演联网服务一键卸载器

用法:
  sudo ./deploy/uninstall-server.sh [选项]

选项:
  --purge-data       同时删除账号、场景和对局数据卷（不可恢复）
  --remove-config    同时删除根目录 .env 配置文件（不可恢复）
  --yes              跳过确认提示
  -h, --help         显示帮助

默认仅停止并删除本服务的容器、网络和本地构建镜像，保留数据卷及 .env。
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
  if [[ -f "$ENV_FILE" ]]; then
    docker_cmd compose --env-file "$ENV_FILE" -f "$COMPOSE_FILE" "$@"
  else
    docker_cmd compose -f "$COMPOSE_FILE" "$@"
  fi
}

load_data_volume_name() {
  [[ -f "$ENV_FILE" ]] || return 0
  local value
  value="$(sed -n 's/^WARGAME_DATA_VOLUME=//p' "$ENV_FILE" | head -n1 || true)"
  [[ -n "$value" ]] && WARGAME_DATA_VOLUME="$value"
}

validate_configuration() {
  [[ -f "$COMPOSE_FILE" ]] || die "未找到 Compose 配置：$COMPOSE_FILE"
  [[ "$WARGAME_DATA_VOLUME" =~ ^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$ ]] || die "数据卷名称格式无效"
}

parse_args() {
  while (($#)); do
    case "$1" in
      --purge-data) PURGE_DATA=1; shift ;;
      --remove-config) REMOVE_CONFIG=1; shift ;;
      --yes) ASSUME_YES=1; shift ;;
      -h|--help) usage; exit 0 ;;
      *) die "未知选项：$1（使用 --help 查看帮助）" ;;
    esac
  done
}

confirm_uninstall() {
  (( ASSUME_YES == 1 )) && return 0
  [[ -t 0 ]] || die "非交互卸载请使用 --yes"
  printf '  将停止并删除兵器推演服务容器、网络和本地镜像'
  (( PURGE_DATA == 1 )) && printf '，并永久删除数据卷 %s' "$WARGAME_DATA_VOLUME"
  (( REMOVE_CONFIG == 1 )) && printf '，并永久删除 .env'
  printf '。继续？[y/N] '
  local answer
  read -r answer
  [[ "$answer" =~ ^[Yy]$ ]] || die "用户取消卸载"
}

main() {
  parse_args "$@"
  load_data_volume_name
  validate_configuration
  command -v docker >/dev/null 2>&1 || die "未检测到 Docker"
  docker_cmd compose version >/dev/null 2>&1 || die "Docker Compose v2 插件不可用"
  confirm_uninstall

  log "停止并删除服务容器、网络和本地镜像"
  compose_cmd down --remove-orphans --rmi local
  ok "服务容器、网络和本地镜像已删除"

  if (( PURGE_DATA == 1 )); then
    if docker_cmd volume inspect "$WARGAME_DATA_VOLUME" >/dev/null 2>&1; then
      docker_cmd volume rm "$WARGAME_DATA_VOLUME" >/dev/null
      ok "数据卷 $WARGAME_DATA_VOLUME 已删除"
    else
      log "数据卷 $WARGAME_DATA_VOLUME 不存在，无需删除"
    fi
  else
    log "已保留数据卷 $WARGAME_DATA_VOLUME"
  fi

  if (( REMOVE_CONFIG == 1 )); then
    if [[ -f "$ENV_FILE" ]]; then
      if [[ $EUID -eq 0 ]]; then rm -f "$ENV_FILE"; else sudo rm -f "$ENV_FILE"; fi
      ok ".env 配置文件已删除"
    fi
  else
    log "已保留配置文件 $ENV_FILE"
  fi
}

main "$@"
