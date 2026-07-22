#!/usr/bin/env bash
set -Eeuo pipefail

# 兵器推演联网服务一键安装器
# Copyright (c) 2026 Gbr

INSTALLER_VERSION="2026.07-shell-settings"

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
SKIP_ENVIRONMENT_CHECK=0
ADMIN_TOKEN=""
BIND_ADDRESS_SET=0
PUBLIC_HOST_SET=0
HTTP_PORT_SET=0
WS_PORT_SET=0
ADMIN_USERNAME_SET=0
SESSION_HOURS_SET=0
WEB_SHELL_ENABLED_SET=0
WEB_SHELL_TICKET_SECONDS_SET=0
WEB_SHELL_SESSION_SECONDS_SET=0
WEB_SHELL_MAX_SESSIONS_SET=0
STARTUP_TIMEOUT_SECONDS_SET=0

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

usage() {
  cat <<'EOF'
Gbr 兵器推演联网服务器安装器

用法:
  sudo ./deploy/install-server.sh [选项]

选项:
  --bind-address IP       Docker 监听 IP；可指定 VPN/FRP 所用网卡，默认自动绑定本机公网 IPv4
  --public-host HOST      客户端连接用 IP 或域名；FRP 场景请填写 FRP 公网入口
  --http-port PORT        管理网页端口，默认 8080
  --ws-port PORT          推演 WebSocket 端口，默认 8090
  --admin-username NAME   管理员用户名，默认 admin
  --admin-password PASS   管理员密码；省略时安全地交互输入
  --reuse-admin-password  复用现有 .env 中的管理员密码
  --session-hours HOURS   管理员网页会话时长，1-168，默认 12
  --shell                 启用网页容器 Shell（默认启用）
  --no-shell              禁用网页容器 Shell
  --shell-ticket-seconds N  Shell 一次性凭证时效，30-600，默认 120
  --shell-session-seconds N Shell 会话时长，60-3600，默认 900
  --shell-max-sessions N  Shell 最大并发会话数，1-8，默认 2
  --startup-timeout-seconds N 等待服务就绪时长，30-600，默认 90
  --reset-admin           启动后强制重置现有管理员密码（默认开启）
  --no-reset-admin        启动后保留现有管理员密码
  --no-build              不重新构建镜像，仅启动已有镜像
  --skip-environment-check 跳过平台、依赖和端口环境检查
  --yes                   跳过确认提示
  -h, --help              显示帮助

示例:
  sudo ./deploy/install-server.sh --admin-password '设置一个强管理员密码'
  sudo ./deploy/install-server.sh --public-host 192.168.1.20 --admin-password 'strong-pass' \
    --session-hours 24 --shell-session-seconds 1800 --shell-max-sessions 3
  sudo ./deploy/install-server.sh --bind-address 10.8.0.2 --admin-password 'strong-pass'
  sudo ./deploy/install-server.sh --bind-address 127.0.0.1 --public-host game.example.com \
    --admin-password 'strong-pass'
EOF
}

on_error() {
  local code=$?
  printf '%b\n' "${C_RED}  [FAIL ] 安装在第 ${BASH_LINENO[0]} 行中止（退出码 ${code}）${C_RESET}" >&2
  printf '%b\n' "${C_DIM}          可查看日志：docker compose --env-file .env -f deploy/compose.yml logs --tail=120${C_RESET}" >&2
  exit "$code"
}
trap on_error ERR

require_cmd() { command -v "$1" >/dev/null 2>&1 || die "缺少命令：$1"; }

run_root() {
  if [[ $EUID -eq 0 ]]; then "$@"; else sudo "$@"; fi
}

docker_cmd() {
  # 优先使用当前用户的 Docker 组权限，避免在无交互终端中无谓触发 sudo 密码提示。
  if [[ $EUID -eq 0 ]] || docker info >/dev/null 2>&1; then
    docker "$@"
  else
    sudo docker "$@"
  fi
}

compose_cmd() {
  docker_cmd compose --env-file "$ENV_FILE" -f "$COMPOSE_FILE" "$@"
}

read_existing_env() {
  [[ -f "$ENV_FILE" ]] || return 0
  local value load_value
  load_value() { sed -n "s/^$1=//p" "$ENV_FILE" | head -n1 || true; }
  value="$(load_value ADMIN_USERNAME)"; [[ $ADMIN_USERNAME_SET -eq 0 && -n "$value" ]] && ADMIN_USERNAME="$value"
  value="$(load_value HOST_BIND_ADDRESS)"
  if [[ $BIND_ADDRESS_SET -eq 0 && -n "$value" ]]; then
    BIND_ADDRESS="$value"
    BIND_ADDRESS_SET=1
  fi
  value="$(load_value HTTP_PORT)"; [[ $HTTP_PORT_SET -eq 0 && -n "$value" ]] && HTTP_PORT="$value"
  value="$(load_value WS_PORT)"; [[ $WS_PORT_SET -eq 0 && -n "$value" ]] && WS_PORT="$value"
  value="$(load_value SESSION_HOURS)"; [[ $SESSION_HOURS_SET -eq 0 && -n "$value" ]] && SESSION_HOURS="$value"
  value="$(load_value WEB_SHELL_ENABLED)"; [[ $WEB_SHELL_ENABLED_SET -eq 0 && -n "$value" ]] && WEB_SHELL_ENABLED="$value"
  value="$(load_value WEB_SHELL_TICKET_SECONDS)"; [[ $WEB_SHELL_TICKET_SECONDS_SET -eq 0 && -n "$value" ]] && WEB_SHELL_TICKET_SECONDS="$value"
  value="$(load_value WEB_SHELL_SESSION_SECONDS)"; [[ $WEB_SHELL_SESSION_SECONDS_SET -eq 0 && -n "$value" ]] && WEB_SHELL_SESSION_SECONDS="$value"
  value="$(load_value WEB_SHELL_MAX_SESSIONS)"; [[ $WEB_SHELL_MAX_SESSIONS_SET -eq 0 && -n "$value" ]] && WEB_SHELL_MAX_SESSIONS="$value"
  value="$(load_value WARGAME_VERSION)"; [[ -n "$value" ]] && WARGAME_VERSION="$value"
  value="$(load_value WARGAME_DATA_VOLUME)"; [[ -n "$value" ]] && WARGAME_DATA_VOLUME="$value"
  value="$(load_value PUBLIC_HOST)"
  if [[ $PUBLIC_HOST_SET -eq 0 && -n "$value" ]]; then
    PUBLIC_HOST="$value"
    PUBLIC_HOST_SET=1
  elif [[ $PUBLIC_HOST_SET -eq 0 && -z "$PUBLIC_HOST" ]]; then
    value="$(load_value PUBLIC_GAME_WS_URL)"
    if [[ "$value" == ws://* || "$value" == wss://* ]]; then
      value="${value#*://}"
      value="${value%%/*}"
      PUBLIC_HOST="${value%%:*}"
      [[ -n "$PUBLIC_HOST" ]] && PUBLIC_HOST_SET=1
    fi
  fi
  if [[ -z "$ADMIN_PASSWORD" && $REUSE_PASSWORD -eq 1 ]]; then
    ADMIN_PASSWORD="$(load_value ADMIN_PASSWORD)"
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
    # prepare_env 会再次读取旧 .env；标记为已选择以保留新的自动公网绑定。
    BIND_ADDRESS_SET=1
  fi
  detect_public_host
}

local_http_base_url() {
  local host="$BIND_ADDRESS"
  [[ "$host" == "0.0.0.0" || "$host" == "127.0.0.1" || "$host" == "localhost" ]] && host="127.0.0.1"
  printf 'http://%s:%s' "$host" "$HTTP_PORT"
}

validate_port() {
  [[ "$1" =~ ^[0-9]+$ ]] && (( 1 <= 10#$1 && 10#$1 <= 65535 )) || die "端口无效：$1"
}

validate_number() {
  local name="$1" value="$2" minimum="$3" maximum="$4"
  [[ "$value" =~ ^[0-9]+$ ]] && (( 10#$value >= minimum && 10#$value <= maximum )) || \
    die "$name 必须是 $minimum 到 $maximum 的整数"
}

validate_boolean() {
  [[ "$1" == "true" || "$1" == "false" ]] || die "$2 只能是 true 或 false"
}

json_escape() {
  local value="$1"
  value="${value//\\/\\\\}"
  value="${value//\"/\\\"}"
  value="${value//$'\n'/\\n}"
  value="${value//$'\r'/\\r}"
  value="${value//$'\t'/\\t}"
  printf '%s' "$value"
}

parse_args() {
  while (($#)); do
    case "$1" in
      --bind-address) BIND_ADDRESS="${2:?缺少 --bind-address 的值}"; BIND_ADDRESS_SET=1; shift 2 ;;
      --public-host) PUBLIC_HOST="${2:?缺少 --public-host 的值}"; PUBLIC_HOST_SET=1; shift 2 ;;
      --http-port) HTTP_PORT="${2:?缺少 --http-port 的值}"; HTTP_PORT_SET=1; shift 2 ;;
      --ws-port) WS_PORT="${2:?缺少 --ws-port 的值}"; WS_PORT_SET=1; shift 2 ;;
      --admin-username) ADMIN_USERNAME="${2:?缺少 --admin-username 的值}"; ADMIN_USERNAME_SET=1; shift 2 ;;
      --admin-password) ADMIN_PASSWORD="${2:?缺少 --admin-password 的值}"; shift 2 ;;
      --reuse-admin-password) REUSE_PASSWORD=1; shift ;;
      --session-hours) SESSION_HOURS="${2:?缺少 --session-hours 的值}"; SESSION_HOURS_SET=1; shift 2 ;;
      --shell) WEB_SHELL_ENABLED="true"; WEB_SHELL_ENABLED_SET=1; shift ;;
      --no-shell) WEB_SHELL_ENABLED="false"; WEB_SHELL_ENABLED_SET=1; shift ;;
      --shell-ticket-seconds) WEB_SHELL_TICKET_SECONDS="${2:?缺少 --shell-ticket-seconds 的值}"; WEB_SHELL_TICKET_SECONDS_SET=1; shift 2 ;;
      --shell-session-seconds) WEB_SHELL_SESSION_SECONDS="${2:?缺少 --shell-session-seconds 的值}"; WEB_SHELL_SESSION_SECONDS_SET=1; shift 2 ;;
      --shell-max-sessions) WEB_SHELL_MAX_SESSIONS="${2:?缺少 --shell-max-sessions 的值}"; WEB_SHELL_MAX_SESSIONS_SET=1; shift 2 ;;
      --startup-timeout-seconds) STARTUP_TIMEOUT_SECONDS="${2:?缺少 --startup-timeout-seconds 的值}"; STARTUP_TIMEOUT_SECONDS_SET=1; shift 2 ;;
      --reset-admin) RESET_ADMIN=1; shift ;;
      --no-reset-admin) RESET_ADMIN=0; shift ;;
      --no-build) NO_BUILD=1; shift ;;
      --skip-environment-check) SKIP_ENVIRONMENT_CHECK=1; shift ;;
      --yes) ASSUME_YES=1; shift ;;
      -h|--help) usage; exit 0 ;;
      *) die "未知选项：$1（使用 --help 查看帮助）" ;;
    esac
  done
  validate_configuration
}

validate_configuration() {
  validate_port "$HTTP_PORT"; validate_port "$WS_PORT"
  validate_number "管理员会话时长" "$SESSION_HOURS" 1 168
  validate_boolean "$WEB_SHELL_ENABLED" "网页 Shell 开关"
  validate_number "Shell 凭证时效" "$WEB_SHELL_TICKET_SECONDS" 30 600
  validate_number "Shell 会话时长" "$WEB_SHELL_SESSION_SECONDS" 60 3600
  validate_number "Shell 并发数" "$WEB_SHELL_MAX_SESSIONS" 1 8
  validate_number "服务启动超时" "$STARTUP_TIMEOUT_SECONDS" 30 600
  [[ "$HTTP_PORT" != "$WS_PORT" ]] || die "管理网页端口和 WebSocket 端口不能相同"
  [[ "$BIND_ADDRESS" != *:* ]] || die "暂不支持带冒号的 IPv6 绑定地址，请使用 IPv4"
  [[ "$ADMIN_USERNAME" =~ ^[A-Za-z0-9_.-]{3,64}$ ]] || die "管理员用户名格式无效"
  [[ "$WARGAME_VERSION" =~ ^[A-Za-z0-9._-]{1,32}$ ]] || die "WARGAME_VERSION 格式无效"
  [[ "$WARGAME_DATA_VOLUME" =~ ^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$ ]] || die "WARGAME_DATA_VOLUME 格式无效"
}

print_banner() {
  printf '%b\n' "${C_CYAN}"
  printf '%s\n' '  ╔══════════════════════════════════════════════════════════════╗'
  printf '%s\n' '  ║              兵 器 推 演 · 联 网 服 务 器                   ║'
  printf '%s\n' '  ║            一键部署与监控校验 · Gbr                         ║'
  printf '%s\n' '  ╚══════════════════════════════════════════════════════════════╝'
  printf '%b\n' "${C_DIM}  安装器版本：$INSTALLER_VERSION${C_RESET}"
  printf '%b\n' "${C_RESET}"
}

check_platform() {
  step "检查服务器环境"
  [[ "$OSTYPE" == linux* ]] || die "仅支持 Linux 服务器"
  if [[ $EUID -ne 0 ]] && ! command -v sudo >/dev/null 2>&1; then die "需要 root 权限或 sudo"; fi
  require_cmd awk; require_cmd sed
  local free_mb mem_mb
  free_mb="$(df -Pm "$ROOT_DIR" | awk 'NR==2 {print $4}')"
  mem_mb="$(awk '/MemTotal/ {printf "%d", $2/1024}' /proc/meminfo 2>/dev/null || echo 0)"
  (( free_mb >= 4096 )) || warn "可用磁盘少于 4 GiB，首次构建 Qt 服务端可能失败（当前 ${free_mb} MiB）"
  (( mem_mb == 0 || mem_mb >= 2048 )) || warn "可用内存少于 2 GiB，首次构建可能较慢（当前 ${mem_mb} MiB）"
  ok "Linux、权限、磁盘和内存检查完成"
}

install_dependencies() {
  step "检查并安装软件依赖"
  if ! command -v docker >/dev/null 2>&1; then
    log "未检测到 Docker，开始安装"
    if command -v apt-get >/dev/null 2>&1; then
      run_root apt-get update
      run_root apt-get install -y ca-certificates curl openssl iproute2 docker.io docker-compose-plugin || \
        run_root apt-get install -y ca-certificates curl openssl iproute2 docker.io docker-compose-v2
    elif command -v dnf >/dev/null 2>&1; then
      run_root dnf install -y ca-certificates curl openssl iproute docker docker-compose-plugin
    elif command -v yum >/dev/null 2>&1; then
      run_root yum install -y ca-certificates curl openssl iproute docker docker-compose-plugin
    else
      die "未找到 apt-get、dnf 或 yum，无法自动安装 Docker"
    fi
  fi
  require_cmd docker
  if ! command -v curl >/dev/null 2>&1 || ! command -v ss >/dev/null 2>&1 || ! command -v openssl >/dev/null 2>&1; then
    if command -v apt-get >/dev/null 2>&1; then
      run_root apt-get update
      run_root apt-get install -y curl iproute2 openssl
    elif command -v dnf >/dev/null 2>&1; then
      run_root dnf install -y curl iproute openssl
    elif command -v yum >/dev/null 2>&1; then
      run_root yum install -y curl iproute openssl
    fi
  fi
  require_cmd curl; require_cmd ss; require_cmd openssl
  if ! docker_cmd compose version >/dev/null 2>&1; then
    if command -v apt-get >/dev/null 2>&1; then run_root apt-get update && run_root apt-get install -y docker-compose-plugin || true; fi
  fi
  docker_cmd compose version >/dev/null 2>&1 || die "Docker Compose v2 插件不可用"
  if ! docker_cmd info >/dev/null 2>&1; then
    run_root systemctl enable --now docker 2>/dev/null || run_root service docker start 2>/dev/null || true
  fi
  docker_cmd info >/dev/null 2>&1 || die "Docker 服务未运行，无法继续"
  ok "$(docker_cmd --version); $(docker_cmd compose version --short)"
}

check_ports() {
  step "检查端口与网络参数"
  local port
  for port in "$HTTP_PORT" "$WS_PORT"; do
    if ss -H -ltn "sport = :$port" 2>/dev/null | grep -q .; then
      if compose_cmd ps -q 2>/dev/null | grep -q .; then
        warn "端口 $port 已由现有兵棋服务占用，安装完成时将由 Compose 接管"
      else
        die "端口 $port 已被其他服务占用，请使用 --http-port/--ws-port 更换端口"
      fi
    fi
  done
  detect_public_host
  [[ "$PUBLIC_HOST" != "localhost" || "$BIND_ADDRESS" == "127.0.0.1" || "$BIND_ADDRESS" == "localhost" ]] || \
    log "客户端地址将使用自动探测的服务器地址：$PUBLIC_HOST"
  ok "绑定地址 $BIND_ADDRESS；客户端主机 $PUBLIC_HOST"
}

prepare_env() {
  step "生成服务配置"
  read_existing_env
  validate_configuration
  if [[ -z "$ADMIN_PASSWORD" ]]; then
    if [[ ! -t 0 ]]; then die "非交互安装必须提供 --admin-password 或 ADMIN_PASSWORD"; fi
    while :; do
      read -r -s -p "  管理员密码（至少 8 个字符）：" ADMIN_PASSWORD; printf '\n'
      [[ ${#ADMIN_PASSWORD} -ge 8 ]] && break
      warn "密码长度不足 8 个字符"
    done
  fi
  [[ ${#ADMIN_PASSWORD} -ge 8 ]] || die "管理员密码至少需要 8 个字符"
  local internal_key
  internal_key="$(openssl rand -hex 32 2>/dev/null || od -An -N32 -tx1 /dev/urandom | tr -d ' \n')"
  [[ -n "$internal_key" ]] || die "无法生成内部 API 密钥"
  if [[ -f "$ENV_FILE" ]]; then
    BACKUP_FILE="$ENV_FILE.backup.$(date +%Y%m%d-%H%M%S)"
    cp -p "$ENV_FILE" "$BACKUP_FILE"
    log "已有 .env 已备份为 $(basename "$BACKUP_FILE")"
  fi
  local ws_host="$PUBLIC_HOST"
  [[ "$BIND_ADDRESS" == "127.0.0.1" || "$BIND_ADDRESS" == "localhost" ]] && ws_host="localhost"
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
  ok "配置已写入 .env（密钥文件权限 600；网页 Shell：$WEB_SHELL_ENABLED）"
}

confirm_install() {
  (( ASSUME_YES == 1 )) && return 0
  printf '\n%b' "${C_YELLOW}  将在 $BIND_ADDRESS 上部署端口 $HTTP_PORT/$WS_PORT，网页 Shell：$WEB_SHELL_ENABLED，并保留现有数据卷。继续？[y/N] ${C_RESET}"
  local answer; read -r answer
  [[ "$answer" =~ ^[Yy]$ ]] || die "用户取消安装"
}

deploy_services() {
  step "构建并启动兵棋推演服务"
  compose_cmd config --quiet || die "Compose 配置校验失败，请检查 .env 和端口配置"
  if (( NO_BUILD == 1 )); then
    compose_cmd up -d
  else
    compose_cmd up -d --build
  fi
  ok "Docker 服务已启动"
}

wait_healthy() {
  step "等待服务健康检查"
  local i status http_base
  http_base="$(local_http_base_url)"
  for ((i=1; i<=STARTUP_TIMEOUT_SECONDS; i++)); do
    if curl -fsS --max-time 2 "$http_base/api/health" >/dev/null 2>&1 \
      && (compose_cmd ps --status running | grep -q 'game-server.*healthy'); then
      ok "账号网页和推演服务器已就绪（${i}s）"
      return 0
    fi
    sleep 1
  done
  compose_cmd ps
  compose_cmd logs --tail=80 account-web game-server >&2 || true
  die "服务未能在 ${STARTUP_TIMEOUT_SECONDS} 秒内健康启动"
}

reset_admin_if_requested() {
  (( RESET_ADMIN == 1 )) || return 0
  step "重置现有管理员密码"
  compose_cmd exec -T account-web \
    /usr/bin/python3 /app/reset_admin.py "$ADMIN_PASSWORD" >/dev/null
  ok "管理员密码已重置，旧管理会话已失效"
}

verify_admin_login() {
  step "验证管理员账号"
  local response payload http_base
  http_base="$(local_http_base_url)"
  payload="{\"username\":\"$(json_escape "$ADMIN_USERNAME")\",\"password\":\"$(json_escape "$ADMIN_PASSWORD")\"}"
  response="$(curl -fsS --max-time 5 \
    -H 'Content-Type: application/json' \
    -d "$payload" \
    "$http_base/api/admin/login")" \
    || die "管理员登录验证失败，请检查账号服务日志"
  ADMIN_TOKEN="$(printf '%s' "$response" | sed -n 's/.*"token":"\([^"]*\)".*/\1/p')"
  [[ -n "$ADMIN_TOKEN" ]] || die "管理员登录验证未返回会话令牌"
  ok "管理员账号验证通过"
}

verify_monitoring() {
  step "验证服务器监控与容器终端"
  [[ -n "$ADMIN_TOKEN" ]] || die "缺少管理员验证会话"
  local overview terminal_payload terminal_response http_base
  http_base="$(local_http_base_url)"
  overview="$(curl -fsS --max-time 5 \
    -H "Authorization: Bearer $ADMIN_TOKEN" \
    "$http_base/api/admin/monitor/overview")" \
    || die "服务器监控概览验证失败"
  printf '%s' "$overview" | grep -q '"accountStatus":"healthy"' \
    || die "账号服务监控状态异常"
  printf '%s' "$overview" | grep -q '"status":"healthy"' \
    || die "兵棋服务监控状态异常"
  if [[ "$WEB_SHELL_ENABLED" == "true" ]]; then
    terminal_payload="{\"password\":\"$(json_escape "$ADMIN_PASSWORD")\"}"
    terminal_response="$(curl -fsS --max-time 5 -X POST \
      -H 'Content-Type: application/json' \
      -H "Authorization: Bearer $ADMIN_TOKEN" \
      -d "$terminal_payload" \
      "$http_base/api/admin/monitor/terminal/login")" \
      || die "容器运维终端认证验证失败"
    printf '%s' "$terminal_response" | grep -q '"authenticated":true' \
      || die "容器运维终端认证未通过"
    printf '%s' "$terminal_response" | grep -q '"terminalTicket":"' \
      || die "容器运维终端认证未返回一次性会话凭证"
    ok "服务器监控、消息审计与容器运维终端验证通过"
  else
    ok "服务器监控、消息审计验证通过；网页 Shell 已禁用"
  fi
}

logout_verification_session() {
  [[ -n "$ADMIN_TOKEN" ]] || return 0
  local http_base
  http_base="$(local_http_base_url)"
  curl -fsS --max-time 5 -X POST \
    -H "Authorization: Bearer $ADMIN_TOKEN" \
    "$http_base/api/admin/logout" >/dev/null || true
  ADMIN_TOKEN=""
}

print_result() {
  local web_host="$PUBLIC_HOST" ws_host="$PUBLIC_HOST"
  if [[ "$BIND_ADDRESS" == "127.0.0.1" || "$BIND_ADDRESS" == "localhost" ]]; then web_host="localhost"; ws_host="localhost"; fi
  printf '\n%b\n' "${C_GREEN}  ╔══════════════════════════════════════════════════════════════╗${C_RESET}"
  printf '%b\n' "${C_GREEN}  ║                 Gbr 部署完成 · 服务正常                     ║${C_RESET}"
  printf '%b\n' "${C_GREEN}  ╚══════════════════════════════════════════════════════════════╝${C_RESET}"
  printf '%b\n' "\n  管理网页：${C_WHITE}http://$web_host:$HTTP_PORT${C_RESET}"
  printf '%b\n' "  服务器监控：${C_WHITE}http://$web_host:$HTTP_PORT（管理员登录后）${C_RESET}"
  printf '%b\n' "  客户端账号服务器：${C_WHITE}http://$web_host:$HTTP_PORT${C_RESET}"
  printf '%b\n' "  客户端 WebSocket：${C_WHITE}ws://$ws_host:$WS_PORT${C_RESET}"
  printf '%b\n' "  管理员用户名：${C_WHITE}$ADMIN_USERNAME${C_RESET}"
  printf '%b\n' "  管理员会话：${C_WHITE}${SESSION_HOURS} 小时${C_RESET}"
  printf '%b\n' "  网页 Shell：${C_WHITE}${WEB_SHELL_ENABLED}（凭证 ${WEB_SHELL_TICKET_SECONDS}s，会话 ${WEB_SHELL_SESSION_SECONDS}s，并发 ${WEB_SHELL_MAX_SESSIONS}）${C_RESET}"
  printf '%b\n' "  配置文件：${C_WHITE}$ENV_FILE${C_RESET}"
  printf '%b\n' "\n  下一步：打开管理网页创建账号，并在“服务器监控”查看服务状态与审计信息。"
  printf '%b\n' "  查看日志：docker compose --env-file .env -f deploy/compose.yml logs --tail=100"
  printf '%b\n' "\n${C_DIM}  Copyright (c) 2026 Gbr · 兵器推演联网服务器${C_RESET}\n"
}

main() {
  print_banner
  read_existing_env
  parse_args "$@"
  select_bind_address
  if (( SKIP_ENVIRONMENT_CHECK == 1 )); then
    step "跳过服务器环境、依赖和端口检查"
    warn "请确认 Docker、Docker Compose、curl、ss、openssl 等依赖已准备就绪"
  else
    check_platform
    install_dependencies
    check_ports
  fi
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
