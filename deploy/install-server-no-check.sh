#!/usr/bin/env bash
set -Eeuo pipefail

# 兵器推演联网服务一键安装器（无 sudo 版）
# 要求：当前用户已具备 Docker/Compose 权限，且依赖命令已安装。
# Copyright (c) 2026 Gbr

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
COMPOSE_FILE="$ROOT_DIR/deploy/compose.yml"
ENV_FILE="$ROOT_DIR/.env"
BACKUP_FILE=""
BIND_ADDRESS="127.0.0.1"
PUBLIC_HOST=""
HTTP_PORT="8080"
WS_PORT="8090"
ADMIN_USERNAME="admin"
ADMIN_PASSWORD=""
SESSION_HOURS="12"
WEB_SHELL_ENABLED="true"
WEB_SHELL_TICKET_SECONDS="120"
WEB_SHELL_SESSION_SECONDS="900"
WEB_SHELL_MAX_SESSIONS="2"
STARTUP_TIMEOUT_SECONDS="90"
WARGAME_VERSION="0.1.0"
WARGAME_DATA_VOLUME="wargame-data"
REUSE_PASSWORD=0
RESET_ADMIN=1
NO_BUILD=0
ASSUME_YES=0
ADMIN_TOKEN=""
BIND_ADDRESS_SET=0
PUBLIC_HOST_SET=0

if [[ -t 1 ]]; then
  C_RESET=$'\033[0m'; C_DIM=$'\033[2m'; C_CYAN=$'\033[36m'; C_GREEN=$'\033[32m'
  C_YELLOW=$'\033[33m'; C_RED=$'\033[31m'; C_WHITE=$'\033[97m'
else
  C_RESET=""; C_DIM=""; C_CYAN=""; C_GREEN=""; C_YELLOW=""; C_RED=""; C_WHITE=""
fi

log() { printf '%b\n' "${C_CYAN}  [Gbr]${C_RESET} $*"; }
ok() { printf '%b\n' "${C_GREEN}  [  OK ]${C_RESET} $*"; }
warn() { printf '%b\n' "${C_YELLOW}  [WARN ]${C_RESET} $*" >&2; }
die() { printf '%b\n' "${C_RED}  [FAIL ]${C_RESET} $*" >&2; exit 1; }
step() { printf '%b\n' "\n${C_WHITE}  >>> $*${C_RESET}"; }
require_cmd() { command -v "$1" >/dev/null 2>&1 || die "缺少命令：$1（无 sudo 版不会自动安装依赖）"; }

usage() {
  cat <<'EOF'
兵器推演联网服务器安装器（无 sudo 版）

用法:
  ./deploy/install-server-no-check.sh [选项]

说明:
  本脚本不执行环境检测、依赖安装、sudo 或 systemctl 操作。
  请先确保当前用户可以直接运行 docker compose，并已安装 curl、openssl。

选项:
  --bind-address IP          Docker 监听 IP；可指定 VPN/FRP 所用网卡，默认自动绑定本机公网 IPv4
  --public-host HOST         客户端连接用 IP 或域名；FRP 场景请填写 FRP 公网入口
  --http-port PORT           管理网页端口，默认 8080
  --ws-port PORT             推演 WebSocket 端口，默认 8090
  --admin-username NAME      管理员用户名，默认 admin
  --admin-password PASS      管理员密码；省略时交互输入
  --reuse-admin-password     复用现有 .env 中的管理员密码
  --session-hours HOURS      管理员网页会话时长，1-168，默认 12
  --shell                    启用网页容器 Shell（默认启用）
  --no-shell                 禁用网页容器 Shell
  --shell-ticket-seconds N   Shell 凭证时效，30-600，默认 120
  --shell-session-seconds N  Shell 会话时长，60-3600，默认 900
  --shell-max-sessions N     Shell 最大并发会话数，1-8，默认 2
  --startup-timeout-seconds N 等待服务就绪时长，30-600，默认 90
  --reset-admin              启动后重置现有管理员密码（默认开启）
  --no-reset-admin           启动后保留现有管理员密码
  --no-build                 不重新构建镜像，仅启动已有镜像
  --yes                      跳过确认提示
  -h, --help                 显示帮助
EOF
}

on_error() {
  local code=$?
  printf '%b\n' "${C_RED}  [FAIL ] 安装在第 ${BASH_LINENO[0]} 行中止（退出码 ${code}）${C_RESET}" >&2
  printf '%b\n' "${C_DIM}          可查看日志：docker compose --env-file .env -f deploy/compose.yml logs --tail=120${C_RESET}" >&2
  exit "$code"
}
trap on_error ERR

compose_cmd() { docker compose --env-file "$ENV_FILE" -f "$COMPOSE_FILE" "$@"; }

ensure_docker_access() {
  docker compose version >/dev/null 2>&1 || die "当前用户无法使用 Docker Compose；请确认 Docker 正在运行，并将当前用户加入 docker 组"
  docker info >/dev/null 2>&1 && return 0

  # 用户刚加入 docker 组但尚未重新登录时，使用无 sudo 的 sg 刷新本次进程组。
  if [[ "${WARGAME_DOCKER_GROUP_REEXEC:-0}" != 1 ]] \
    && command -v sg >/dev/null 2>&1 \
    && getent group docker 2>/dev/null | awk -F: -v user="$(id -un)" '{n=split($4, members, ","); for (i=1; i<=n; i++) if (members[i] == user) found=1} END {exit !found}'; then
    local command_line
    printf -v command_line '%q ' "$0" "$@"
    exec sg docker -c "WARGAME_DOCKER_GROUP_REEXEC=1 $command_line"
  fi

  die "当前用户无法使用 Docker API；请执行 newgrp docker 或重新登录后重试（不需要 sudo）"
}

load_env_value() {
  [[ -f "$ENV_FILE" ]] || return 0
  sed -n "s/^$1=//p" "$ENV_FILE" | head -n1 || true
}

read_existing_env() {
  local value
  [[ -f "$ENV_FILE" ]] || return 0
  value="$(load_env_value ADMIN_USERNAME)"; [[ -n "$value" ]] && ADMIN_USERNAME="$value"
  value="$(load_env_value HOST_BIND_ADDRESS)"
  if [[ -n "$value" ]]; then BIND_ADDRESS="$value"; BIND_ADDRESS_SET=1; fi
  value="$(load_env_value HTTP_PORT)"; [[ -n "$value" ]] && HTTP_PORT="$value"
  value="$(load_env_value WS_PORT)"; [[ -n "$value" ]] && WS_PORT="$value"
  value="$(load_env_value SESSION_HOURS)"; [[ -n "$value" ]] && SESSION_HOURS="$value"
  value="$(load_env_value WEB_SHELL_ENABLED)"; [[ -n "$value" ]] && WEB_SHELL_ENABLED="$value"
  value="$(load_env_value WEB_SHELL_TICKET_SECONDS)"; [[ -n "$value" ]] && WEB_SHELL_TICKET_SECONDS="$value"
  value="$(load_env_value WEB_SHELL_SESSION_SECONDS)"; [[ -n "$value" ]] && WEB_SHELL_SESSION_SECONDS="$value"
  value="$(load_env_value WEB_SHELL_MAX_SESSIONS)"; [[ -n "$value" ]] && WEB_SHELL_MAX_SESSIONS="$value"
  value="$(load_env_value WARGAME_VERSION)"; [[ -n "$value" ]] && WARGAME_VERSION="$value"
  value="$(load_env_value WARGAME_DATA_VOLUME)"; [[ -n "$value" ]] && WARGAME_DATA_VOLUME="$value"
  value="$(load_env_value PUBLIC_HOST)"
  if [[ -n "$value" ]]; then
    PUBLIC_HOST="$value"
    PUBLIC_HOST_SET=1
  elif [[ -z "$PUBLIC_HOST" ]]; then
    value="$(load_env_value PUBLIC_GAME_WS_URL)"
    if [[ "$value" == ws://* || "$value" == wss://* ]]; then
      value="${value#*://}"
      value="${value%%/*}"
      PUBLIC_HOST="${value%%:*}"
      [[ -n "$PUBLIC_HOST" ]] && PUBLIC_HOST_SET=1
    fi
  fi
  if [[ -z "$ADMIN_PASSWORD" && $REUSE_PASSWORD -eq 1 ]]; then
    ADMIN_PASSWORD="$(load_env_value ADMIN_PASSWORD)"
  fi
}

is_public_ipv4() {
  local ip="$1" a b c d
  [[ "$ip" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]] || return 1
  IFS=. read -r a b c d <<<"$ip"
  (( 10#$a <= 255 && 10#$b <= 255 && 10#$c <= 255 && 10#$d <= 255 )) || return 1
  (( 10#$a != 0 && 10#$a != 10 && 10#$a != 127 && 10#$a < 224 )) || return 1
  (( !(10#$a == 100 && 10#$b >= 64 && 10#$b <= 127) )) || return 1
  (( !(10#$a == 169 && 10#$b == 254) )) || return 1
  (( !(10#$a == 172 && 10#$b >= 16 && 10#$b <= 31) )) || return 1
  (( !(10#$a == 192 && (10#$b == 168 || 10#$b == 0)) )) || return 1
  (( !(10#$a == 198 && (10#$b == 18 || 10#$b == 19)) )) || return 1
  (( !(10#$a == 198 && 10#$b == 51 && 10#$c == 100) )) || return 1
  (( !(10#$a == 203 && 10#$b == 0 && 10#$c == 113) )) || return 1
}

local_ipv4_addresses() {
  if command -v ip >/dev/null 2>&1; then
    ip -4 -o addr show scope global 2>/dev/null | sed -n 's/.* inet \([0-9.]*\)\/.*/\1/p'
  else
    hostname -I 2>/dev/null | tr ' ' '\n'
  fi
}

detect_local_public_ipv4() {
  local candidate
  while IFS= read -r candidate; do
    if is_public_ipv4 "$candidate"; then
      printf '%s' "$candidate"
      return 0
    fi
  done < <(local_ipv4_addresses)
  return 1
}

detect_public_host() {
  [[ -n "$PUBLIC_HOST" ]] && return 0
  # VPN 绑定地址通常就是客户端入口；FRP 的公网入口应通过 --public-host 显式指定。
  if [[ $BIND_ADDRESS_SET -eq 1 && "$BIND_ADDRESS" != "0.0.0.0" \
    && "$BIND_ADDRESS" != "127.0.0.1" && "$BIND_ADDRESS" != "localhost" ]]; then
    PUBLIC_HOST="$BIND_ADDRESS"
    return 0
  fi
  PUBLIC_HOST="$(detect_local_public_ipv4 || true)"
  if [[ -z "$PUBLIC_HOST" ]]; then
    PUBLIC_HOST="$(local_ipv4_addresses | sed -n '/^127\./!{/^[0-9]/p;q;}' || true)"
  fi
  [[ -n "$PUBLIC_HOST" ]] || PUBLIC_HOST="localhost"
}

select_bind_address() {
  local public_ip
  public_ip="$(detect_local_public_ipv4 || true)"
  if [[ $BIND_ADDRESS_SET -eq 0 \
    && ( "$BIND_ADDRESS" == "127.0.0.1" || "$BIND_ADDRESS" == "localhost" ) \
    && -n "$public_ip" ]]; then
    BIND_ADDRESS="$public_ip"
  fi
  detect_public_host
}

local_http_base_url() {
  local host="$BIND_ADDRESS"
  [[ "$host" == "0.0.0.0" || "$host" == "127.0.0.1" || "$host" == "localhost" ]] && host="127.0.0.1"
  printf 'http://%s:%s' "$host" "$HTTP_PORT"
}

validate_port() {
  [[ "$1" =~ ^[0-9]+$ ]] && ((1 <= 10#$1 && 10#$1 <= 65535)) || die "端口无效：$1"
}

validate_number() {
  local name="$1" value="$2" minimum="$3" maximum="$4"
  [[ "$value" =~ ^[0-9]+$ ]] && ((10#$value >= minimum && 10#$value <= maximum)) || \
    die "$name 必须是 $minimum 到 $maximum 的整数"
}

validate_configuration() {
  validate_port "$HTTP_PORT"; validate_port "$WS_PORT"
  validate_number "管理员会话时长" "$SESSION_HOURS" 1 168
  [[ "$WEB_SHELL_ENABLED" == "true" || "$WEB_SHELL_ENABLED" == "false" ]] || die "网页 Shell 开关只能是 true 或 false"
  validate_number "Shell 凭证时效" "$WEB_SHELL_TICKET_SECONDS" 30 600
  validate_number "Shell 会话时长" "$WEB_SHELL_SESSION_SECONDS" 60 3600
  validate_number "Shell 并发数" "$WEB_SHELL_MAX_SESSIONS" 1 8
  validate_number "服务启动超时" "$STARTUP_TIMEOUT_SECONDS" 30 600
  [[ "$WARGAME_VERSION" =~ ^[A-Za-z0-9._-]{1,32}$ ]] || die "WARGAME_VERSION 格式无效"
  [[ "$WARGAME_DATA_VOLUME" =~ ^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$ ]] || die "WARGAME_DATA_VOLUME 格式无效"
  [[ "$HTTP_PORT" != "$WS_PORT" ]] || die "管理网页端口和 WebSocket 端口不能相同"
  [[ "$BIND_ADDRESS" != *:* ]] || die "暂不支持带冒号的 IPv6 绑定地址"
  [[ "$ADMIN_USERNAME" =~ ^[A-Za-z0-9_.-]{3,64}$ ]] || die "管理员用户名格式无效"
}

json_escape() {
  local value="$1"
  value="${value//\\/\\\\}"; value="${value//\"/\\\"}"
  value="${value//$'\n'/\\n}"; value="${value//$'\r'/\\r}"; value="${value//$'\t'/\\t}"
  printf '%s' "$value"
}

parse_args() {
  while (($#)); do
    case "$1" in
      --bind-address) BIND_ADDRESS="${2:?缺少 --bind-address 的值}"; BIND_ADDRESS_SET=1; shift 2 ;;
      --public-host) PUBLIC_HOST="${2:?缺少 --public-host 的值}"; PUBLIC_HOST_SET=1; shift 2 ;;
      --http-port) HTTP_PORT="${2:?缺少 --http-port 的值}"; shift 2 ;;
      --ws-port) WS_PORT="${2:?缺少 --ws-port 的值}"; shift 2 ;;
      --admin-username) ADMIN_USERNAME="${2:?缺少 --admin-username 的值}"; shift 2 ;;
      --admin-password) ADMIN_PASSWORD="${2:?缺少 --admin-password 的值}"; shift 2 ;;
      --reuse-admin-password) REUSE_PASSWORD=1; shift ;;
      --session-hours) SESSION_HOURS="${2:?缺少 --session-hours 的值}"; shift 2 ;;
      --shell) WEB_SHELL_ENABLED=true; shift ;;
      --no-shell) WEB_SHELL_ENABLED=false; shift ;;
      --shell-ticket-seconds) WEB_SHELL_TICKET_SECONDS="${2:?缺少参数值}"; shift 2 ;;
      --shell-session-seconds) WEB_SHELL_SESSION_SECONDS="${2:?缺少参数值}"; shift 2 ;;
      --shell-max-sessions) WEB_SHELL_MAX_SESSIONS="${2:?缺少参数值}"; shift 2 ;;
      --startup-timeout-seconds) STARTUP_TIMEOUT_SECONDS="${2:?缺少参数值}"; shift 2 ;;
      --reset-admin) RESET_ADMIN=1; shift ;;
      --no-reset-admin) RESET_ADMIN=0; shift ;;
      --no-build) NO_BUILD=1; shift ;;
      --yes) ASSUME_YES=1; shift ;;
      -h|--help) usage; exit 0 ;;
      *) die "未知选项：$1（使用 --help 查看帮助）" ;;
    esac
  done
  validate_configuration
}

print_banner() {
  printf '%b\n' "${C_CYAN}"
  printf '%s\n' '  ╔══════════════════════════════════════════════════════════════╗'
  printf '%s\n' '  ║              兵 器 推 演 · 联 网 服 务 器                   ║'
  printf '%s\n' '  ║                 一键部署 · 无 sudo 版                       ║'
  printf '%s\n' '  ╚══════════════════════════════════════════════════════════════╝'
  printf '%b\n' "${C_DIM}  已跳过环境、依赖和端口检查${C_RESET}"
  printf '%b\n' "${C_RESET}"
}

prepare_env() {
  step "生成服务配置"
  validate_configuration
  if [[ -z "$ADMIN_PASSWORD" && $REUSE_PASSWORD -eq 1 ]]; then
    ADMIN_PASSWORD="$(load_env_value ADMIN_PASSWORD)"
  fi
  if [[ -z "$ADMIN_PASSWORD" ]]; then
    [[ -t 0 ]] || die "非交互安装必须提供 --admin-password 或现有 .env 密码"
    while :; do
      read -r -s -p "  管理员密码（至少 8 个字符）：" ADMIN_PASSWORD; printf '\n'
      [[ ${#ADMIN_PASSWORD} -ge 8 ]] && break
      warn "密码长度不足 8 个字符"
    done
  fi
  [[ ${#ADMIN_PASSWORD} -ge 8 ]] || die "管理员密码至少需要 8 个字符"
  local internal_key ws_host
  internal_key="$(openssl rand -hex 32 2>/dev/null || od -An -N32 -tx1 /dev/urandom | tr -d ' \n')"
  [[ -n "$internal_key" ]] || die "无法生成内部 API 密钥"
  if [[ -f "$ENV_FILE" ]]; then
    BACKUP_FILE="$ENV_FILE.backup.$(date +%Y%m%d-%H%M%S)"
    cp -p "$ENV_FILE" "$BACKUP_FILE"
    log "已有 .env 已备份为 $(basename "$BACKUP_FILE")"
  fi
  detect_public_host
  ws_host="$PUBLIC_HOST"
  [[ "$BIND_ADDRESS" == "127.0.0.1" || "$BIND_ADDRESS" == "localhost" ]] && ws_host=localhost
  umask 077
  cat > "$ENV_FILE" <<EOF
ADMIN_USERNAME=$ADMIN_USERNAME
ADMIN_PASSWORD=$ADMIN_PASSWORD
INTERNAL_API_KEY=$internal_key
WARGAME_VERSION=$WARGAME_VERSION
WARGAME_DATA_VOLUME=$WARGAME_DATA_VOLUME
HOST_BIND_ADDRESS=$BIND_ADDRESS
PUBLIC_HOST=$PUBLIC_HOST
HTTP_PORT=$HTTP_PORT
WS_PORT=$WS_PORT
PUBLIC_GAME_WS_URL=ws://$ws_host:$WS_PORT
SESSION_HOURS=$SESSION_HOURS
WEB_SHELL_ENABLED=$WEB_SHELL_ENABLED
WEB_SHELL_TICKET_SECONDS=$WEB_SHELL_TICKET_SECONDS
WEB_SHELL_SESSION_SECONDS=$WEB_SHELL_SESSION_SECONDS
WEB_SHELL_MAX_SESSIONS=$WEB_SHELL_MAX_SESSIONS
EOF
  chmod 600 "$ENV_FILE"
  ok "配置已写入 .env（权限 600；网页 Shell：$WEB_SHELL_ENABLED）"
}

confirm_install() {
  ((ASSUME_YES == 1)) && return 0
  printf '\n%b' "${C_YELLOW}  将在 $BIND_ADDRESS 上部署端口 $HTTP_PORT/$WS_PORT，并保留现有数据卷。继续？[y/N] ${C_RESET}"
  local answer; read -r answer
  [[ "$answer" =~ ^[Yy]$ ]] || die "用户取消安装"
}

deploy_services() {
  step "构建并启动兵棋推演服务"
  compose_cmd config --quiet || die "Compose 配置校验失败，请检查 .env 和端口配置"
  if ((NO_BUILD == 1)); then compose_cmd up -d; else compose_cmd up -d --build; fi
  ok "Docker 服务已启动"
}

wait_healthy() {
  step "等待服务健康检查"
  local i http_base
  http_base="$(local_http_base_url)"
  for ((i=1; i<=STARTUP_TIMEOUT_SECONDS; i++)); do
    if curl -fsS --max-time 2 "$http_base/api/health" >/dev/null 2>&1 \
      && (compose_cmd ps --status running | grep -q 'game-server.*healthy'); then
      ok "账号网页和推演服务器已就绪（${i}s）"; return 0
    fi
    sleep 1
  done
  compose_cmd ps
  compose_cmd logs --tail=80 account-web game-server >&2 || true
  die "服务未能在 ${STARTUP_TIMEOUT_SECONDS} 秒内健康启动"
}

reset_admin_if_requested() {
  ((RESET_ADMIN == 1)) || return 0
  step "重置现有管理员密码"
  compose_cmd exec -T account-web /usr/bin/python3 /app/reset_admin.py "$ADMIN_PASSWORD" >/dev/null
  ok "管理员密码已重置，旧管理会话已失效"
}

verify_admin_login() {
  step "验证管理员账号"
  local payload response http_base
  http_base="$(local_http_base_url)"
  payload="{\"username\":\"$(json_escape "$ADMIN_USERNAME")\",\"password\":\"$(json_escape "$ADMIN_PASSWORD")\"}"
  response="$(curl -fsS --max-time 5 -H 'Content-Type: application/json' -d "$payload" "$http_base/api/admin/login")" \
    || die "管理员登录验证失败，请检查账号服务日志"
  ADMIN_TOKEN="$(printf '%s' "$response" | sed -n 's/.*"token":"\([^"]*\)".*/\1/p')"
  [[ -n "$ADMIN_TOKEN" ]] || die "管理员登录验证未返回会话令牌"
  ok "管理员账号验证通过"
}

verify_monitoring() {
  step "验证服务器监控与容器终端"
  local overview terminal_payload terminal_response http_base
  http_base="$(local_http_base_url)"
  overview="$(curl -fsS --max-time 5 -H "Authorization: Bearer $ADMIN_TOKEN" "$http_base/api/admin/monitor/overview")" \
    || die "服务器监控概览验证失败"
  printf '%s' "$overview" | grep -q '"accountStatus":"healthy"' || die "账号服务监控状态异常"
  printf '%s' "$overview" | grep -q '"status":"healthy"' || die "兵棋服务监控状态异常"
  if [[ "$WEB_SHELL_ENABLED" == true ]]; then
    terminal_payload="{\"password\":\"$(json_escape "$ADMIN_PASSWORD")\"}"
    terminal_response="$(curl -fsS --max-time 5 -X POST -H 'Content-Type: application/json' \
      -H "Authorization: Bearer $ADMIN_TOKEN" -d "$terminal_payload" \
      "$http_base/api/admin/monitor/terminal/login")" || die "容器运维终端认证验证失败"
    printf '%s' "$terminal_response" | grep -q '"authenticated":true' || die "容器运维终端认证未通过"
    printf '%s' "$terminal_response" | grep -q '"terminalTicket":"' || die "容器运维终端认证未返回会话凭证"
  fi
  ok "服务器监控、消息审计与容器终端验证通过"
}

logout_verification_session() {
  [[ -n "$ADMIN_TOKEN" ]] || return 0
  local http_base
  http_base="$(local_http_base_url)"
  curl -fsS --max-time 5 -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
    "$http_base/api/admin/logout" >/dev/null || true
}

print_result() {
  local host="$PUBLIC_HOST"
  [[ "$BIND_ADDRESS" == "127.0.0.1" || "$BIND_ADDRESS" == "localhost" ]] && host=localhost
  printf '\n%b\n' "${C_GREEN}  ╔══════════════════════════════════════════════════════════════╗${C_RESET}"
  printf '%b\n' "${C_GREEN}  ║                 Gbr 部署完成 · 服务正常                     ║${C_RESET}"
  printf '%b\n' "${C_GREEN}  ╚══════════════════════════════════════════════════════════════╝${C_RESET}"
  printf '%b\n' "\n  管理网页：${C_WHITE}http://$host:$HTTP_PORT${C_RESET}"
  printf '%b\n' "  客户端 WebSocket：${C_WHITE}ws://$host:$WS_PORT${C_RESET}"
  printf '%b\n' "  管理员用户名：${C_WHITE}$ADMIN_USERNAME${C_RESET}"
  printf '%b\n' "  配置文件：${C_WHITE}$ENV_FILE${C_RESET}"
  printf '%b\n' "\n  查看日志：docker compose --env-file .env -f deploy/compose.yml logs --tail=100"
}

main() {
  print_banner
  read_existing_env
  parse_args "$@"
  select_bind_address
  require_cmd docker; require_cmd curl; require_cmd grep; require_cmd sed; require_cmd openssl
  ensure_docker_access "$@"
  prepare_env
  confirm_install
  deploy_services
  wait_healthy
  reset_admin_if_requested
  verify_admin_login
  verify_monitoring
  logout_verification_session
  print_result
}

main "$@"
