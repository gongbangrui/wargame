#!/usr/bin/env bash
set -Eeuo pipefail

# 兵棋推演联网服务一键安装器
# Copyright (c) 2026 Gbr

INSTALLER_VERSION="2026.08"

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT_DIR/deploy/release-lib.sh"
SOURCE_ROOT="$ROOT_DIR"
SOURCE_INPUT=""
SOURCE_ARCHIVE=""
SOURCE_INPUT_TOP=""
SOURCE_IS_MANAGED_CURRENT=0
INSTALL_ROOT=""
CURRENT_DIR=""
RUNTIME_MARKER=""
COMPOSE_FILE=""
ENV_FILE=""
WARGAME_COMPOSE_PROJECT="wargame"
WARGAME_RUNTIME_ENV_FILE=""
INSTALL_DIR_SET=0
COMPOSE_PROJECT_SET=0
PROJECT_VERSION="$(release_project_version "$ROOT_DIR")"
INTERNAL_API_KEY=""
BACKUP_FILE=""
BIND_ADDRESS="127.0.0.1"
PUBLIC_HOST=""
HTTP_PORT="8080"
WS_PORT="8090"
ADMIN_USERNAME="admin"
ADMIN_PASSWORD_SET="${ADMIN_PASSWORD+x}"
ADMIN_PASSWORD="${ADMIN_PASSWORD-}"
SESSION_HOURS="12"
WEB_SHELL_ENABLED="false"
WEB_SHELL_TICKET_SECONDS="120"
WEB_SHELL_SESSION_SECONDS="900"
WEB_SHELL_MAX_SESSIONS="2"
WEB_SHELL_ALLOWED_ORIGINS=""
STARTUP_TIMEOUT_SECONDS="90"
WARGAME_VERSION="${PROJECT_VERSION:-2.0.0}"
WARGAME_SOURCE_DIGEST="dev"
WARGAME_RELEASE_ID="${WARGAME_VERSION}-dev"
WARGAME_DATA_VOLUME="wargame-data"
PUBLIC_GAME_WS_URL=""
LOGIN_TRUSTED_PROXIES=""
LOGIN_WINDOW_SECONDS="60"
LOGIN_SUBJECT_IP_FAILURE_LIMIT="5"
LOGIN_IP_FAILURE_LIMIT="20"
GAME_ROOM_ID="main"
ROOM_OPERATION_TIMEOUT_SECONDS="20"
AI_PROVIDER="auto"
OLLAMA_BASE_URL="http://host.docker.internal:11434"
OLLAMA_MODEL="auto"
OLLAMA_CONNECT_TIMEOUT_MS="1500"
OLLAMA_TIMEOUT_MS="60000"
OLLAMA_MAX_RESPONSE_BYTES="65536"
WARGAME_ALLOW_RECOVERY_RESET="0"
WARGAME_ENABLE_FASTDDS="OFF"
WARGAME_FASTDDS_MODE="disabled"
FASTDDS_DOMAIN_ID="0"
FASTDDS_DISCOVERY_TIMEOUT_MS="5000"
FASTDDS_STATIC_PEERS=""
REUSE_PASSWORD=0
RESET_ADMIN=1
NO_BUILD=0
NO_CACHE=0
NO_PULL=0
ASSUME_YES=0
SKIP_ENVIRONMENT_CHECK=0
ADMIN_TOKEN=""
BIND_ADDRESS_SET=0
PUBLIC_HOST_SET=0
PUBLIC_GAME_WS_URL_SET=0
HTTP_PORT_SET=0
WS_PORT_SET=0
ADMIN_USERNAME_SET=0
SESSION_HOURS_SET=0
WEB_SHELL_ENABLED_SET=0
WEB_SHELL_TICKET_SECONDS_SET=0
WEB_SHELL_SESSION_SECONDS_SET=0
WEB_SHELL_MAX_SESSIONS_SET=0
STARTUP_TIMEOUT_SECONDS_SET=0
SOURCE_SET=0
ENV_EXISTED=0
DATA_VOLUME_SET=0
ADMIN_PASSWORD_STDIN=0
WARGAME_VERSION_SET=0

if [[ -t 1 ]]; then
  C_RESET=$'\033[0m'; C_DIM=$'\033[2m'; C_CYAN=$'\033[36m'; C_GREEN=$'\033[32m'
  C_YELLOW=$'\033[33m'; C_RED=$'\033[31m'; C_WHITE=$'\033[97m'
else
  C_RESET=""; C_DIM=""; C_CYAN=""; C_GREEN=""; C_YELLOW=""; C_RED=""; C_WHITE=""
fi

log() { printf '%b\n' "${C_CYAN}  [Gbr]${C_RESET} $*"; }
ok() { printf '%b\n' "${C_GREEN}  [  OK ]${C_RESET} $*"; }
warn() { printf '%b\n' "${C_YELLOW}  [WARN ]${C_RESET} $*" >&2; }
die() {
  printf '%b\n' "${C_RED}  [FAIL ]${C_RESET} $*" >&2
  rollback_source_activation 2>/dev/null || true
  exit 1
}
step() { printf '%b\n' "\n${C_WHITE}  >>> $*${C_RESET}"; }

usage() {
  cat <<'EOF'
Gbr 兵棋推演联网服务器安装器

用法:
  sudo ./deploy/install-server.sh [选项]

选项:
  --bind-address IP       Docker 监听 IP；默认 127.0.0.1
  --public-host HOST      客户端连接用 IP 或域名；FRP 场景请填写 FRP 公网入口
  --public-game-ws-url URL 对外 WebSocket 地址；非回环绑定必须使用 wss://
  --http-port PORT        管理网页端口，默认 8080
  --ws-port PORT          推演 WebSocket 端口，默认 8090
  --admin-username NAME   管理员用户名，默认 admin
  --admin-password-stdin  从标准输入读取管理员密码；省略时安全地交互输入
  --wargame-version VER   固定部署版本；默认读取当前源码的项目版本
  --source PATH            完整源码目录或 wargame-server-*.tar.gz 发布包
  --install-dir DIR       受管运行时根目录，默认当前用户主目录下的 wargame
  --compose-project NAME  稳定的 Docker Compose 项目名，默认 wargame
  --data-volume NAME      持久 Docker 数据卷名，默认 wargame-data
  --reuse-admin-password  复用现有 .env 中的管理员密码
  --session-hours HOURS   管理员网页会话时长，1-168，默认 12
  --shell                 显式启用高风险网页容器 Shell（默认禁用）
  --no-shell              禁用网页容器 Shell
  --shell-ticket-seconds N  Shell 一次性凭证时效，30-600，默认 120
  --shell-session-seconds N Shell 会话时长，60-3600，默认 900
  --shell-max-sessions N  Shell 最大并发会话数，1-8，默认 2
  --startup-timeout-seconds N 等待服务就绪时长，30-600，默认 90
  --reset-admin           启动后强制重置现有管理员密码（默认开启）
  --no-reset-admin        启动后保留现有管理员密码
  --no-build              不重新构建镜像，仅启动已有镜像
  --no-cache              重建镜像时忽略 Docker 构建缓存
  --no-pull               构建时不拉取最新基础镜像
  --skip-environment-check 跳过平台、依赖和端口环境检查
  --yes                   跳过确认提示
  -h, --help              显示帮助

默认会拉取基础镜像，并重建当前源码中的账号网页和权威服务器。

示例:
  sudo ./deploy/install-server.sh
  sudo ./deploy/install-server.sh --public-host 192.168.1.20 \
    --session-hours 24 --shell-session-seconds 1800 --shell-max-sessions 3
  sudo ./deploy/install-server.sh --bind-address 10.8.0.2
  sudo ./deploy/install-server.sh --bind-address 127.0.0.1 --public-host game.example.com \
    --admin-password-stdin < /root/wargame-admin-password
EOF
}

on_error() {
  local code=$?
  rollback_source_activation
  printf '%b\n' "${C_RED}  [FAIL ] 安装在第 ${BASH_LINENO[0]} 行中止（退出码 ${code}）${C_RESET}" >&2
  printf '%b\n' "${C_DIM}          保留失败暂存目录并保留运行配置、备份和数据卷${C_RESET}" >&2
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
  docker_cmd compose --project-name "$WARGAME_COMPOSE_PROJECT" \
    --env-file "$WARGAME_RUNTIME_ENV_FILE" -f "$COMPOSE_FILE" "$@"
}

default_install_root() {
  local home_dir
  if [[ $EUID -eq 0 ]]; then
    if [[ -n "${SUDO_USER:-}" && "$SUDO_USER" != "root" ]]; then
      home_dir="$(getent passwd "$SUDO_USER" | cut -d: -f6)"
    else
      home_dir="/root"
    fi
  else
    home_dir="${HOME:-}"
  fi
  [[ -n "$home_dir" && "$home_dir" == /* ]] || die "无法解析安装用户主目录"
  printf '%s/wargame' "${home_dir%/}"
}

path_is_source_or_nested() {
  local candidate="$1" source="$2"
  [[ "$candidate" == "$source" || "$candidate" == "$source"/* \
    || "$source" == "$candidate" || "$source" == "$candidate"/* ]]
}

validate_compose_project() {
  [[ "$WARGAME_COMPOSE_PROJECT" =~ ^[A-Za-z0-9][A-Za-z0-9_-]{0,62}$ ]] \
    || die "Compose 项目名无效：$WARGAME_COMPOSE_PROJECT"
}

parse_bootstrap_args() {
  local arg
  while (($#)); do
    arg="$1"
    case "$arg" in
      --source)
        [[ $# -ge 2 ]] || die "缺少 --source 的值"
        SOURCE_INPUT="$2"
        SOURCE_SET=1
        shift 2
        ;;
      --install-dir|--compose-project)
        [[ $# -ge 2 ]] || die "缺少 $arg 的值"
        shift 2
        ;;
      *) shift ;;
    esac
  done
}

required_source_paths=(
  CMakeLists.txt
  cmake
  src
  server
  'design/vmf设计.docx'
  design/EncoderDecoder/README.txt
  design/EncoderDecoder/dic.xml
  design/EncoderDecoder/dic_content.xml
  design/EncoderDecoder/message_catalog.json
  design/EncoderDecoder/msgStruct
  deploy/compose.yml
  deploy/install-server.sh
  deploy/account.Dockerfile
  deploy/game-server.Dockerfile
  deploy/release-lib.sh
  deploy/release-manifest.env
  map/metadata.json
)

validate_source_tree() {
  local source="$1" path
  for path in "${required_source_paths[@]}"; do
    [[ -e "$source/$path" ]] || die "源码缺少必要构建输入：$path"
    [[ ! -L "$source/$path" ]] || die "源码包含不受支持的特殊文件或链接：$path"
    if [[ -d "$source/$path" ]]; then
      while IFS= read -r -d '' node; do
        [[ ! -L "$node" ]] || die "源码包含不受支持的特殊文件或链接：${node#"$source/"}"
        [[ -f "$node" || -d "$node" ]] || die "源码包含不受支持的特殊文件：${node#"$source/"}"
      done < <(find "$source/$path" -xdev -print0)
    else
      [[ -f "$source/$path" ]] || die "源码包含不受支持的特殊文件：$path"
    fi
  done
}

validate_archive() {
  local archive="$1" listing entry normalized top="" first type
  [[ -f "$archive" ]] || die "发布包不存在：$archive"
  listing="$(tar -tzf "$archive" 2>/dev/null)" || die "无法读取发布包：$archive"
  [[ -n "$listing" ]] || die "发布包为空：$archive"
  while IFS= read -r entry; do
    normalized="${entry%/}"
    [[ -n "$normalized" ]] || continue
    case "$normalized" in
      /*|.|..|./*|../*|*/../*|*/..|*"$'\n'"*|*"$'\r'"*)
        die "发布包包含不安全路径：$entry"
        ;;
    esac
    first="${normalized%%/*}"
    if [[ -z "$top" ]]; then
      top="$first"
    elif [[ "$first" != "$top" ]]; then
      die "发布包必须只有一个顶层目录"
    fi
    [[ "$normalized" == "$top" || "$normalized" == "$top"/* ]] || die "发布包顶层目录无效"
  done <<<"$listing"
  [[ -n "$top" ]] || die "发布包缺少顶层目录"
  while IFS= read -r entry; do
    [[ -z "$entry" ]] && continue
    type="${entry:0:1}"
    case "$type" in
      -|d) ;;
      *) die "发布包包含链接或特殊文件：${entry:0:80}" ;;
    esac
  done < <(tar -tvzf "$archive" 2>/dev/null || die "无法检查发布包文件类型：$archive")
  printf '%s' "$top"
}

resolve_source_input() {
  local source_path top
  if (( SOURCE_SET == 0 )); then
    SOURCE_ROOT="$ROOT_DIR"
    return 0
  fi
  [[ -n "$SOURCE_INPUT" ]] || die "--source 不能为空"
  source_path="$(realpath -- "$SOURCE_INPUT" 2>/dev/null || true)"
  [[ -n "$source_path" ]] || die "无法解析 --source：$SOURCE_INPUT"
  if [[ -d "$source_path" ]]; then
    SOURCE_ROOT="$source_path"
    SOURCE_ARCHIVE=""
    return 0
  fi
  [[ -f "$source_path" ]] || die "--source 必须是源码目录或发布包：$SOURCE_INPUT"
  top="$(validate_archive "$source_path")"
  SOURCE_ROOT="$source_path"
  SOURCE_ARCHIVE="$source_path"
  SOURCE_INPUT_TOP="$top"
}

resolve_runtime_layout() {
  local requested_root="" requested_project="" requested_root_set=0 canonical_root canonical_source
  while (($#)); do
    case "$1" in
      --install-dir)
        [[ $# -ge 2 ]] || die "缺少 --install-dir 的值"
        requested_root="$2"; requested_root_set=1; shift 2 ;;
      --compose-project)
        [[ $# -ge 2 ]] || die "缺少 --compose-project 的值"
        requested_project="$2"; shift 2 ;;
      *) shift ;;
    esac
  done
  if (( requested_root_set == 1 )); then
    INSTALL_ROOT="$requested_root"
    INSTALL_DIR_SET=1
  else
    INSTALL_ROOT="$(default_install_root)"
  fi
  [[ -n "$INSTALL_ROOT" && "$INSTALL_ROOT" == /* ]] \
    || die "安装目录必须是非空绝对路径"
  [[ "$INSTALL_ROOT" != "/" && "$INSTALL_ROOT" != *$'\n'* && "$INSTALL_ROOT" != *$'\r'* ]] \
    || die "安装目录无效"
  [[ "$INSTALL_ROOT" != */../* && "$INSTALL_ROOT" != */.. && "$INSTALL_ROOT" != ../* && "$INSTALL_ROOT" != ".." ]] \
    || die "安装目录不能包含路径遍历"
  [[ ! -L "$INSTALL_ROOT" ]] || die "安装目录不能是符号链接"
  canonical_root="$(realpath -m -- "$INSTALL_ROOT")"
  canonical_source="$(realpath -m -- "$SOURCE_ROOT")"
  [[ "$canonical_root" != "/" ]] || die "安装目录不能是根目录"
  if [[ "$canonical_source" == "$canonical_root/current" ]]; then
    SOURCE_IS_MANAGED_CURRENT=1
  elif path_is_source_or_nested "$canonical_root" "$canonical_source"; then
    die "安装目录不能与源码目录重叠"
  fi
  INSTALL_ROOT="$canonical_root"
  CURRENT_DIR="$INSTALL_ROOT/current"
  RUNTIME_MARKER="$INSTALL_ROOT/.wargame-install"
  ENV_FILE="$INSTALL_ROOT/.env"
  WARGAME_RUNTIME_ENV_FILE="$ENV_FILE"
  COMPOSE_FILE="$CURRENT_DIR/deploy/compose.yml"
  [[ -n "$requested_project" ]] && WARGAME_COMPOSE_PROJECT="$requested_project" && COMPOSE_PROJECT_SET=1
  validate_compose_project
  if (( DATA_VOLUME_SET == 0 )); then WARGAME_DATA_VOLUME="${WARGAME_COMPOSE_PROJECT}-data"; fi
  if [[ -e "$INSTALL_ROOT" ]]; then
    [[ -f "$RUNTIME_MARKER" ]] || die "安装目录已存在且不是受管运行时：$INSTALL_ROOT"
  else
    mkdir -p -- "$INSTALL_ROOT"
  fi
  mkdir -p -- "$INSTALL_ROOT/backups" "$INSTALL_ROOT/.staging"
  [[ -f "$COMPOSE_FILE" ]] || COMPOSE_FILE="$SOURCE_ROOT/deploy/compose.yml"
  if [[ $EUID -eq 0 && -n "${SUDO_USER:-}" && "$SUDO_USER" != "root" ]]; then
    local owner_uid owner_gid
    owner_uid="$(id -u "$SUDO_USER")"; owner_gid="$(id -g "$SUDO_USER")"
    chown -R "$owner_uid:$owner_gid" "$INSTALL_ROOT"
  fi
}

ensure_runtime_marker() {
  if [[ ! -f "$RUNTIME_MARKER" ]]; then
    umask 077
    printf 'version=1\nsource=%s\nproject=%s\n' "$SOURCE_ROOT" "$WARGAME_COMPOSE_PROJECT" >"$RUNTIME_MARKER"
  fi
  chmod 600 "$RUNTIME_MARKER"
}

STAGING_DIR=""
PREVIOUS_CURRENT=""
ACTIVE_NEW=0
ROLLBACK_IN_PROGRESS=0

is_prohibited_source_path() {
  local path="$1"
  local base="${path##*/}"
  [[ "$path" == "deploy/.env.example" ]] && return 1

  case "$path" in
    .env*|*/.env*|build|build/*|*/build|*/build/*|cache|cache/*|*/cache|*/cache/*|.cache|.cache/*|*/.cache|*/.cache/*|logs|logs/*|*/logs|*/logs/*|backups|backups/*|*/backups|*/backups/*|.omo|.omo/*|*/.omo|*/.omo/*|__pycache__|__pycache__/*|*/__pycache__|*/__pycache__/*|.pytest_cache|.pytest_cache/*|*/.pytest_cache|*/.pytest_cache/*)
      return 0
      ;;
  esac

  case "$base" in
    *.db|*.db-*|*.sqlite|*.sqlite-*|*.sqlite3|*.sqlite3-*|*.jsonl|*.log|*.cache|*.pyc|*.pyo|*.pyd|*checkpoint*|*status*|*events*)
      return 0
      ;;
  esac

  return 1
}

prune_runtime_artifacts() {
  local root="$1" candidate relative
  while IFS= read -r -d '' candidate; do
    relative="${candidate#"$root/"}"
    if is_prohibited_source_path "$relative"; then
      rm -rf -- "$candidate"
    fi
  done < <(find "$root" -mindepth 1 -print0)
}

copy_source_inputs() {
  local source="$1" destination="$2" input
  validate_source_tree "$source"
  mkdir -p -- "$destination"
  for input in CMakeLists.txt Main.qml main.cpp qml cmake src server \
      'design/vmf设计.docx' design/EncoderDecoder/README.txt \
      design/EncoderDecoder/dic.xml design/EncoderDecoder/dic_content.xml \
      design/EncoderDecoder/message_catalog.json design/EncoderDecoder/msgStruct \
      deploy map/metadata.json \
      .dockerignore README.md docs release-identity.txt; do
    [[ -e "$source/$input" ]] || continue
    [[ "$input" != */* ]] || mkdir -p -- "$destination/${input%/*}"
    cp -a --no-preserve=ownership "$source/$input" "$destination/$input"
  done
  prune_runtime_artifacts "$destination"
}

stage_source_tree() {
  local source="$SOURCE_ROOT" extracted top input
  STAGING_DIR="$(mktemp -d "$INSTALL_ROOT/.staging/source.XXXXXX")"
  if [[ -n "$SOURCE_ARCHIVE" ]]; then
    extracted="$(mktemp -d "$INSTALL_ROOT/.staging/archive.XXXXXX")"
    tar -xzf "$SOURCE_ARCHIVE" --no-same-owner --no-same-permissions -C "$extracted"
    top="$SOURCE_INPUT_TOP"
    source="$extracted/$top"
    copy_source_inputs "$source" "$STAGING_DIR"
    rm -rf -- "$extracted"
  else
    copy_source_inputs "$source" "$STAGING_DIR"
  fi
  for input in "${required_source_paths[@]}"; do
    [[ -e "$STAGING_DIR/$input" ]] || die "暂存源码缺少必要输入：$input"
  done
  release_validate_manifest "$STAGING_DIR/deploy/release-manifest.env" \
    || die "三端发布清单校验失败"
}

activate_staged_source() {
  local previous_name
  [[ -d "$STAGING_DIR" ]] || die "源码暂存目录不存在"
  previous_name="$INSTALL_ROOT/.staging/previous.$(date +%Y%m%d-%H%M%S).$$"
  if [[ -e "$CURRENT_DIR" ]]; then
    mv -- "$CURRENT_DIR" "$previous_name"
    PREVIOUS_CURRENT="$previous_name"
  fi
  mv -- "$STAGING_DIR" "$CURRENT_DIR"
  STAGING_DIR=""
  ACTIVE_NEW=1
  COMPOSE_FILE="$CURRENT_DIR/deploy/compose.yml"
  ensure_runtime_marker
}

rollback_source_activation() {
  local failed_name
  (( ROLLBACK_IN_PROGRESS == 0 )) || return 0
  ROLLBACK_IN_PROGRESS=1
  if (( ENV_EXISTED == 1 )) && [[ -n "$BACKUP_FILE" && -f "$BACKUP_FILE" ]]; then
    cp -p -- "$BACKUP_FILE" "$ENV_FILE" 2>/dev/null || true
  fi
  if (( ACTIVE_NEW != 1 )); then
    ROLLBACK_IN_PROGRESS=0
    return 0
  fi
  compose_cmd down --remove-orphans >/dev/null 2>&1 || true
  failed_name="$INSTALL_ROOT/backups/failed-staging-$(date -u +%Y%m%d-%H%M%S)-$$"
  if [[ -d "$CURRENT_DIR" ]]; then
    mv -- "$CURRENT_DIR" "$failed_name" 2>/dev/null || true
  fi
  if [[ -n "$PREVIOUS_CURRENT" && -d "$PREVIOUS_CURRENT" ]]; then
    mv -- "$PREVIOUS_CURRENT" "$CURRENT_DIR" 2>/dev/null || true
    COMPOSE_FILE="$CURRENT_DIR/deploy/compose.yml"
    compose_cmd up -d --no-build >/dev/null 2>&1 || true
  fi
  ACTIVE_NEW=0
}

read_existing_env() {
  [[ -f "$ENV_FILE" ]] || return 0
  local value load_value
  load_value() {
    local loaded
    loaded="$(sed -n "s/^$1=//p" "$ENV_FILE" | head -n1 || true)"
    if [[ "$loaded" == \'*\' ]]; then
      loaded="${loaded:1:${#loaded}-2}"
      loaded="${loaded//\\\'/\'}"
    fi
    printf '%s' "$loaded"
  }
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
  value="$(load_value WEB_SHELL_ALLOWED_ORIGINS)"; [[ -n "$value" ]] && WEB_SHELL_ALLOWED_ORIGINS="$value"
  value="$(load_value WARGAME_DATA_VOLUME)"; [[ -n "$value" ]] && WARGAME_DATA_VOLUME="$value"
  value="$(load_value INTERNAL_API_KEY)"; [[ -n "$value" ]] && INTERNAL_API_KEY="$value"
  value="$(load_value WARGAME_COMPOSE_PROJECT)"
  if [[ $COMPOSE_PROJECT_SET -eq 0 && -n "$value" ]]; then WARGAME_COMPOSE_PROJECT="$value"; fi
  value="$(load_value WARGAME_RUNTIME_ENV_FILE)"
  [[ -z "$value" || "$value" == "$ENV_FILE" ]] || die "WARGAME_RUNTIME_ENV_FILE 必须指向绝对运行时配置：$ENV_FILE"
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
  value="$(load_value PUBLIC_GAME_WS_URL)"; [[ $PUBLIC_GAME_WS_URL_SET -eq 0 && -n "$value" ]] && PUBLIC_GAME_WS_URL="$value"
  value="$(load_value LOGIN_TRUSTED_PROXIES)"; [[ -n "$value" ]] && LOGIN_TRUSTED_PROXIES="$value"
  value="$(load_value LOGIN_WINDOW_SECONDS)"; [[ -n "$value" ]] && LOGIN_WINDOW_SECONDS="$value"
  value="$(load_value LOGIN_SUBJECT_IP_FAILURE_LIMIT)"; [[ -n "$value" ]] && LOGIN_SUBJECT_IP_FAILURE_LIMIT="$value"
  value="$(load_value LOGIN_IP_FAILURE_LIMIT)"; [[ -n "$value" ]] && LOGIN_IP_FAILURE_LIMIT="$value"
  value="$(load_value GAME_ROOM_ID)"; [[ -n "$value" ]] && GAME_ROOM_ID="$value"
  value="$(load_value ROOM_OPERATION_TIMEOUT_SECONDS)"; [[ -n "$value" ]] && ROOM_OPERATION_TIMEOUT_SECONDS="$value"
  value="$(load_value AI_PROVIDER)"; [[ -n "$value" ]] && AI_PROVIDER="$value"
  value="$(load_value OLLAMA_BASE_URL)"; [[ -n "$value" ]] && OLLAMA_BASE_URL="$value"
  value="$(load_value OLLAMA_MODEL)"; [[ -n "$value" ]] && OLLAMA_MODEL="$value"
  value="$(load_value OLLAMA_CONNECT_TIMEOUT_MS)"; [[ -n "$value" ]] && OLLAMA_CONNECT_TIMEOUT_MS="$value"
  value="$(load_value OLLAMA_TIMEOUT_MS)"; [[ -n "$value" ]] && OLLAMA_TIMEOUT_MS="$value"
  # 15 seconds was the documented default before local structured planning
  # support. Promote that exact legacy default during upgrades; explicit newer
  # values continue through the normal validation path below.
  [[ "$OLLAMA_TIMEOUT_MS" != "15000" ]] || OLLAMA_TIMEOUT_MS="60000"
  value="$(load_value OLLAMA_MAX_RESPONSE_BYTES)"; [[ -n "$value" ]] && OLLAMA_MAX_RESPONSE_BYTES="$value"
  value="$(load_value WARGAME_ALLOW_RECOVERY_RESET)"; [[ -n "$value" ]] && WARGAME_ALLOW_RECOVERY_RESET="$value"
  value="$(load_value WARGAME_ENABLE_FASTDDS)"; [[ -n "$value" ]] && WARGAME_ENABLE_FASTDDS="$value"
  value="$(load_value WARGAME_FASTDDS_MODE)"; [[ -n "$value" ]] && WARGAME_FASTDDS_MODE="$value"
  value="$(load_value FASTDDS_DOMAIN_ID)"; [[ -n "$value" ]] && FASTDDS_DOMAIN_ID="$value"
  value="$(load_value FASTDDS_DISCOVERY_TIMEOUT_MS)"; [[ -n "$value" ]] && FASTDDS_DISCOVERY_TIMEOUT_MS="$value"
  value="$(load_value FASTDDS_STATIC_PEERS)" && FASTDDS_STATIC_PEERS="$value"
  if [[ $REUSE_PASSWORD -eq 1 ]]; then
    ADMIN_PASSWORD="$(load_value ADMIN_PASSWORD)"
    ADMIN_PASSWORD_SET=1
  fi
}

migrate_legacy_env() {
  [[ -f "$ENV_FILE" ]] && return 0
  [[ "$SOURCE_ROOT" != "$INSTALL_ROOT" ]] || return 0
  if [[ -f "$SOURCE_ROOT/.env" ]]; then
    umask 077
    cp -p -- "$SOURCE_ROOT/.env" "$ENV_FILE"
    chmod 600 "$ENV_FILE"
    log "已将旧源码根目录的 .env 迁移到 $ENV_FILE"
  fi
}

is_ipv4() {
  local ip="$1" a b c d
  [[ "$ip" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]] || return 1
  IFS=. read -r a b c d <<<"$ip"
  (( 10#$a <= 255 && 10#$b <= 255 && 10#$c <= 255 && 10#$d <= 255 )) || return 1
}

is_public_ipv4() {
  local ip="$1" a b c d
  is_ipv4 "$ip" || return 1
  IFS=. read -r a b c d <<<"$ip"
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

dotenv_quote() {
  local value="$1"
  [[ "$value" != *$'\n'* && "$value" != *$'\r'* ]] || die "配置值不能包含换行符"
  value="${value//\'/\\\'}"
  printf "'%s'" "$value"
}

parse_args() {
  while (($#)); do
    case "$1" in
      --bind-address) BIND_ADDRESS="${2:?缺少 --bind-address 的值}"; BIND_ADDRESS_SET=1; shift 2 ;;
      --public-host) PUBLIC_HOST="${2:?缺少 --public-host 的值}"; PUBLIC_HOST_SET=1; shift 2 ;;
      --public-game-ws-url) PUBLIC_GAME_WS_URL="${2:?缺少 --public-game-ws-url 的值}"; PUBLIC_GAME_WS_URL_SET=1; shift 2 ;;
      --http-port) HTTP_PORT="${2:?缺少 --http-port 的值}"; HTTP_PORT_SET=1; shift 2 ;;
      --ws-port) WS_PORT="${2:?缺少 --ws-port 的值}"; WS_PORT_SET=1; shift 2 ;;
      --admin-username) ADMIN_USERNAME="${2:?缺少 --admin-username 的值}"; ADMIN_USERNAME_SET=1; shift 2 ;;
      --admin-password)
        die "--admin-password 会泄露凭据；请改用交互输入或 --admin-password-stdin" ;;
      --admin-password-stdin) ADMIN_PASSWORD_STDIN=1; shift ;;
      --source) SOURCE_INPUT="${2:?缺少 --source 的值}"; SOURCE_SET=1; shift 2 ;;
      --install-dir) INSTALL_ROOT="${2:?缺少 --install-dir 的值}"; INSTALL_DIR_SET=1; shift 2 ;;
      --compose-project) WARGAME_COMPOSE_PROJECT="${2:?缺少 --compose-project 的值}"; COMPOSE_PROJECT_SET=1; shift 2 ;;
      --data-volume) WARGAME_DATA_VOLUME="${2:?缺少 --data-volume 的值}"; DATA_VOLUME_SET=1; shift 2 ;;
      --wargame-version) WARGAME_VERSION="${2:?缺少 --wargame-version 的值}"; WARGAME_VERSION_SET=1; shift 2 ;;
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
      --no-cache) NO_CACHE=1; shift ;;
      --no-pull) NO_PULL=1; shift ;;
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
  [[ "$BIND_ADDRESS" == "localhost" ]] || is_ipv4 "$BIND_ADDRESS" \
    || die "绑定地址必须是 IPv4 地址或 localhost"
  [[ -z "$PUBLIC_HOST" || "$PUBLIC_HOST" =~ ^[A-Za-z0-9]([A-Za-z0-9.-]{0,251}[A-Za-z0-9])?$ ]] \
    || die "客户端主机必须是有效的 IPv4 地址或域名"
  [[ -z "$PUBLIC_GAME_WS_URL" || "$PUBLIC_GAME_WS_URL" == ws://* || "$PUBLIC_GAME_WS_URL" == wss://* ]] \
    || die "PUBLIC_GAME_WS_URL 必须使用 ws:// 或 wss://"
  [[ "$BIND_ADDRESS" == "127.0.0.1" || "$BIND_ADDRESS" == "localhost" ]] \
    || die "为避免明文 HTTP 暴露，安装器只允许回环绑定；远程访问请在反向代理后转发到回环端口"
  [[ "$ADMIN_USERNAME" =~ ^[A-Za-z0-9_.-]{3,64}$ ]] || die "管理员用户名格式无效"
  [[ "$WARGAME_VERSION" =~ ^[A-Za-z0-9._-]{1,32}$ ]] || die "WARGAME_VERSION 格式无效"
  [[ "$WARGAME_DATA_VOLUME" =~ ^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$ ]] || die "WARGAME_DATA_VOLUME 格式无效"
  validate_number "登录限流窗口" "$LOGIN_WINDOW_SECONDS" 10 3600
  validate_number "账号/IP 登录失败上限" "$LOGIN_SUBJECT_IP_FAILURE_LIMIT" 1 100
  validate_number "IP 登录失败上限" "$LOGIN_IP_FAILURE_LIMIT" 1 1000
  validate_number "房间操作超时" "$ROOM_OPERATION_TIMEOUT_SECONDS" 10 300
  case "$AI_PROVIDER" in
    rules|auto|ollama) ;;
    *) die "AI_PROVIDER 必须是 rules、auto 或 ollama" ;;
  esac
  [[ "$OLLAMA_BASE_URL" != *$'\n'* && "$OLLAMA_BASE_URL" != *$'\r'* \
    && "$OLLAMA_BASE_URL" != *'?'* && "$OLLAMA_BASE_URL" != *'#'* \
    && "$OLLAMA_BASE_URL" != *'@'* && ("$OLLAMA_BASE_URL" == http://* || "$OLLAMA_BASE_URL" == https://*) ]] \
    || die "OLLAMA_BASE_URL 必须是无用户信息、查询参数和片段的 http/https URL"
  [[ -n "$OLLAMA_MODEL" && ${#OLLAMA_MODEL} -le 128 && "$OLLAMA_MODEL" != *$'\n'* && "$OLLAMA_MODEL" != *$'\r'* ]] \
    || die "OLLAMA_MODEL 无效"
  validate_number "Ollama 连接超时" "$OLLAMA_CONNECT_TIMEOUT_MS" 100 1500
  validate_number "Ollama 总超时" "$OLLAMA_TIMEOUT_MS" 30000 120000
  validate_number "Ollama 响应大小上限" "$OLLAMA_MAX_RESPONSE_BYTES" 1024 65536
  [[ "$WARGAME_ALLOW_RECOVERY_RESET" == "0" || "$WARGAME_ALLOW_RECOVERY_RESET" == "1" ]] \
    || die "WARGAME_ALLOW_RECOVERY_RESET 只能是 0 或 1"
  [[ "$WARGAME_ENABLE_FASTDDS" == "ON" || "$WARGAME_ENABLE_FASTDDS" == "OFF" ]] \
    || die "WARGAME_ENABLE_FASTDDS 只能是 ON 或 OFF"
  case "$WARGAME_FASTDDS_MODE" in
    compatibility|dds|disabled|off|none) ;;
    *) die "WARGAME_FASTDDS_MODE 必须是 compatibility、dds 或 disabled" ;;
  esac
  validate_number "Fast DDS Domain ID" "$FASTDDS_DOMAIN_ID" 0 232
  validate_number "Fast DDS 发现超时" "$FASTDDS_DISCOVERY_TIMEOUT_MS" 0 3600000
  [[ "$FASTDDS_STATIC_PEERS" != *$'\n'* && "$FASTDDS_STATIC_PEERS" != *$'\r'* ]] \
    || die "FASTDDS_STATIC_PEERS 不能包含换行符"
  validate_compose_project
  [[ "$WARGAME_RUNTIME_ENV_FILE" == /* && "$WARGAME_RUNTIME_ENV_FILE" == "$ENV_FILE" ]] \
    || die "WARGAME_RUNTIME_ENV_FILE 必须是运行时 .env 的绝对路径"
}

print_banner() {
  printf '%b\n' "${C_CYAN}"
  printf '%s\n' '  ╔══════════════════════════════════════════════════════════════╗'
  printf '%s\n' '  ║              兵 棋 推 演 · 联 网 服 务 器                   ║'
  printf '%s\n' '  ║              网页与权威服务一键部署 · Gbr                   ║'
  printf '%s\n' '  ╚══════════════════════════════════════════════════════════════╝'
  printf '%b\n' "${C_DIM}  安装器版本：$INSTALLER_VERSION${C_RESET}"
  printf '%b\n' "${C_RESET}"
}

check_platform() {
  step "检查服务器环境"
  [[ "$OSTYPE" == linux* ]] || die "仅支持 Linux 服务器"
  if [[ $EUID -ne 0 ]] && ! command -v sudo >/dev/null 2>&1; then die "需要 root 权限或 sudo"; fi
  local free_mb mem_mb
  if command -v df >/dev/null 2>&1 && command -v awk >/dev/null 2>&1; then
    free_mb="$(df -Pm "$ROOT_DIR" | awk 'NR==2 {print $4}')"
    mem_mb="$(awk '/MemTotal/ {printf "%d", $2/1024}' /proc/meminfo 2>/dev/null || echo 0)"
    (( free_mb >= 4096 )) || warn "可用磁盘少于 4 GiB，首次构建 Qt 服务端可能失败（当前 ${free_mb} MiB）"
    (( mem_mb == 0 || mem_mb >= 2048 )) || warn "可用内存少于 2 GiB，首次构建可能较慢（当前 ${mem_mb} MiB）"
  else
    warn "缺少 df/awk，跳过磁盘和内存容量提示；依赖安装阶段会补齐基础工具"
  fi
  ok "Linux、权限、磁盘和内存检查完成"
}

install_dependencies() {
  step "检查并安装软件依赖"
  if ! command -v docker >/dev/null 2>&1; then
    log "未检测到 Docker，开始安装"
    if command -v apt-get >/dev/null 2>&1; then
      run_root apt-get update
      run_root apt-get install -y ca-certificates curl openssl iproute2 docker.io docker-compose-plugin \
        coreutils findutils tar gawk || \
        run_root apt-get install -y ca-certificates curl openssl iproute2 docker.io docker-compose-v2 \
          coreutils findutils tar gawk
    elif command -v dnf >/dev/null 2>&1; then
      run_root dnf install -y ca-certificates curl openssl iproute docker docker-compose-plugin \
        coreutils findutils tar gawk
    elif command -v yum >/dev/null 2>&1; then
      run_root yum install -y ca-certificates curl openssl iproute docker docker-compose-plugin \
        coreutils findutils tar gawk
    else
      die "未找到 apt-get、dnf 或 yum，无法自动安装 Docker"
    fi
  fi
  require_cmd docker
  if ! command -v curl >/dev/null 2>&1 || ! command -v ss >/dev/null 2>&1 || ! command -v openssl >/dev/null 2>&1; then
    if command -v apt-get >/dev/null 2>&1; then
      run_root apt-get update
      run_root apt-get install -y curl iproute2 openssl coreutils findutils tar gawk
    elif command -v dnf >/dev/null 2>&1; then
      run_root dnf install -y curl iproute openssl coreutils findutils tar gawk
    elif command -v yum >/dev/null 2>&1; then
      run_root yum install -y curl iproute openssl coreutils findutils tar gawk
    fi
  fi
  require_cmd curl; require_cmd ss; require_cmd openssl
  if ! docker_cmd compose version >/dev/null 2>&1; then
    if command -v apt-get >/dev/null 2>&1; then
      run_root apt-get update
      run_root apt-get install -y docker-compose-plugin || run_root apt-get install -y docker-compose-v2
    elif command -v dnf >/dev/null 2>&1; then
      run_root dnf install -y docker-compose-plugin || run_root dnf install -y docker-compose
    elif command -v yum >/dev/null 2>&1; then
      run_root yum install -y docker-compose-plugin || run_root yum install -y docker-compose
    fi
  fi
  for bootstrap_cmd in awk sed find tar realpath sha256sum; do
    require_cmd "$bootstrap_cmd"
  done
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
  if (( ADMIN_PASSWORD_STDIN == 1 )); then
    [[ -z "$ADMIN_PASSWORD_SET" ]] || die "不能同时使用 ADMIN_PASSWORD 和 --admin-password-stdin"
    IFS= read -r ADMIN_PASSWORD || die "无法从 stdin 读取管理员密码"
    ADMIN_PASSWORD_SET=1
  elif [[ -z "$ADMIN_PASSWORD_SET" ]]; then
    if [[ ! -t 0 ]]; then die "非交互安装必须使用 --admin-password-stdin 或 ADMIN_PASSWORD"; fi
    read -r -s -p "  管理员密码：" ADMIN_PASSWORD; printf '\n'
    ADMIN_PASSWORD_SET=1
  fi
  (( ${#ADMIN_PASSWORD} >= 8 )) || die "管理员密码必须至少为 8 个字符"
  local internal_key
  internal_key="$INTERNAL_API_KEY"
  if [[ -z "$internal_key" ]]; then
    internal_key="$(openssl rand -hex 32 2>/dev/null || od -An -N32 -tx1 /dev/urandom | tr -d ' \n')"
  fi
  [[ -n "$internal_key" ]] || die "无法生成内部 API 密钥"
  if [[ -f "$ENV_FILE" ]]; then
    ENV_EXISTED=1
    BACKUP_FILE="$INSTALL_ROOT/backups/.env.backup.$(date +%Y%m%d-%H%M%S)"
    cp -p "$ENV_FILE" "$BACKUP_FILE"
    log "已有 .env 已备份为 $(basename "$BACKUP_FILE")"
  fi
  local ws_host="$PUBLIC_HOST"
  [[ "$BIND_ADDRESS" == "127.0.0.1" || "$BIND_ADDRESS" == "localhost" ]] && ws_host="localhost"
  [[ -n "$PUBLIC_GAME_WS_URL" ]] || PUBLIC_GAME_WS_URL="ws://$ws_host:$WS_PORT"
  if [[ -z "$WEB_SHELL_ALLOWED_ORIGINS" ]]; then
    if [[ "$PUBLIC_GAME_WS_URL" == wss://* ]]; then
      WEB_SHELL_ALLOWED_ORIGINS="https://$PUBLIC_HOST"
    else
      WEB_SHELL_ALLOWED_ORIGINS="http://$PUBLIC_HOST:$HTTP_PORT"
    fi
  fi
  umask 077
  local env_tmp
  env_tmp="$(mktemp "$INSTALL_ROOT/.env.new.XXXXXX")"
  cat > "$env_tmp" <<EOF
ADMIN_USERNAME=$ADMIN_USERNAME
ADMIN_PASSWORD=$(dotenv_quote "$ADMIN_PASSWORD")
INTERNAL_API_KEY=$internal_key
WARGAME_VERSION=$WARGAME_VERSION
WARGAME_SOURCE_DIGEST=$WARGAME_SOURCE_DIGEST
WARGAME_RELEASE_ID=$WARGAME_RELEASE_ID
WARGAME_DATA_VOLUME=$WARGAME_DATA_VOLUME
WARGAME_COMPOSE_PROJECT=$WARGAME_COMPOSE_PROJECT
WARGAME_RUNTIME_ENV_FILE=$WARGAME_RUNTIME_ENV_FILE
HOST_BIND_ADDRESS=$BIND_ADDRESS
PUBLIC_HOST=$PUBLIC_HOST
HTTP_PORT=$HTTP_PORT
WS_PORT=$WS_PORT
PUBLIC_GAME_WS_URL=$PUBLIC_GAME_WS_URL
SESSION_HOURS=$SESSION_HOURS
WEB_SHELL_ENABLED=$WEB_SHELL_ENABLED
WEB_SHELL_TICKET_SECONDS=$WEB_SHELL_TICKET_SECONDS
WEB_SHELL_SESSION_SECONDS=$WEB_SHELL_SESSION_SECONDS
WEB_SHELL_MAX_SESSIONS=$WEB_SHELL_MAX_SESSIONS
WEB_SHELL_ALLOWED_ORIGINS=$WEB_SHELL_ALLOWED_ORIGINS
LOGIN_TRUSTED_PROXIES=$LOGIN_TRUSTED_PROXIES
LOGIN_WINDOW_SECONDS=$LOGIN_WINDOW_SECONDS
LOGIN_SUBJECT_IP_FAILURE_LIMIT=$LOGIN_SUBJECT_IP_FAILURE_LIMIT
LOGIN_IP_FAILURE_LIMIT=$LOGIN_IP_FAILURE_LIMIT
GAME_ROOM_ID=$GAME_ROOM_ID
ROOM_OPERATION_TIMEOUT_SECONDS=$ROOM_OPERATION_TIMEOUT_SECONDS
AI_PROVIDER=$AI_PROVIDER
OLLAMA_BASE_URL=$(dotenv_quote "$OLLAMA_BASE_URL")
OLLAMA_MODEL=$(dotenv_quote "$OLLAMA_MODEL")
OLLAMA_CONNECT_TIMEOUT_MS=$OLLAMA_CONNECT_TIMEOUT_MS
OLLAMA_TIMEOUT_MS=$OLLAMA_TIMEOUT_MS
OLLAMA_MAX_RESPONSE_BYTES=$OLLAMA_MAX_RESPONSE_BYTES
WARGAME_ALLOW_RECOVERY_RESET=$WARGAME_ALLOW_RECOVERY_RESET
WARGAME_ENABLE_FASTDDS=$WARGAME_ENABLE_FASTDDS
WARGAME_FASTDDS_MODE=$WARGAME_FASTDDS_MODE
FASTDDS_DOMAIN_ID=$FASTDDS_DOMAIN_ID
FASTDDS_DISCOVERY_TIMEOUT_MS=$FASTDDS_DISCOVERY_TIMEOUT_MS
FASTDDS_STATIC_PEERS=$FASTDDS_STATIC_PEERS
EOF
  if [[ -n "$BACKUP_FILE" && -f "$BACKUP_FILE" ]]; then
    awk -F= '
      $1 !~ /^(ADMIN_USERNAME|ADMIN_PASSWORD|INTERNAL_API_KEY|WARGAME_VERSION|WARGAME_SOURCE_DIGEST|WARGAME_RELEASE_ID|WARGAME_DATA_VOLUME|WARGAME_COMPOSE_PROJECT|WARGAME_RUNTIME_ENV_FILE|HOST_BIND_ADDRESS|PUBLIC_HOST|HTTP_PORT|WS_PORT|PUBLIC_GAME_WS_URL|SESSION_HOURS|WEB_SHELL_ENABLED|WEB_SHELL_TICKET_SECONDS|WEB_SHELL_SESSION_SECONDS|WEB_SHELL_MAX_SESSIONS|WEB_SHELL_ALLOWED_ORIGINS|LOGIN_TRUSTED_PROXIES|LOGIN_WINDOW_SECONDS|LOGIN_SUBJECT_IP_FAILURE_LIMIT|LOGIN_IP_FAILURE_LIMIT|GAME_ROOM_ID|ROOM_OPERATION_TIMEOUT_SECONDS|AI_PROVIDER|OLLAMA_BASE_URL|OLLAMA_MODEL|OLLAMA_CONNECT_TIMEOUT_MS|OLLAMA_TIMEOUT_MS|OLLAMA_MAX_RESPONSE_BYTES|WARGAME_ALLOW_RECOVERY_RESET|WARGAME_ENABLE_FASTDDS|WARGAME_FASTDDS_MODE|FASTDDS_DOMAIN_ID|FASTDDS_DISCOVERY_TIMEOUT_MS|FASTDDS_STATIC_PEERS)$/ { print }
    ' "$BACKUP_FILE" >> "$env_tmp"
  fi
  mv -- "$env_tmp" "$ENV_FILE"
  chmod 600 "$ENV_FILE"
  ok "配置已写入 .env（密钥文件权限 600；网页 Shell：$WEB_SHELL_ENABLED）"
}

confirm_install() {
  (( ASSUME_YES == 1 )) && return 0
  printf '\n%b' "${C_YELLOW}  将在 $BIND_ADDRESS 上部署端口 $HTTP_PORT/$WS_PORT，网页 Shell：$WEB_SHELL_ENABLED，并保留现有数据卷。继续？[y/N] ${C_RESET}"
  local answer; read -r answer
  [[ "$answer" =~ ^[Yy]$ ]] || die "用户取消安装"
}

verify_release_images() {
  local image
  for image in \
    "wargame-account-web:$WARGAME_RELEASE_ID" \
    "wargame-game-server:$WARGAME_RELEASE_ID"; do
    if ! docker_cmd image inspect "$image" >/dev/null 2>&1; then
      die "--no-build 要求已有发布镜像：$image；请去掉 --no-build 重新构建"
    fi
  done
}

deploy_services() {
  step "更新并启动兵棋推演服务"
  compose_cmd config --quiet || die "Compose 配置校验失败，请检查 .env 和端口配置"
  if (( NO_BUILD == 1 )); then
    verify_release_images
    compose_cmd up -d --force-recreate --remove-orphans
  else
    local -a build_args=(build)
    (( NO_CACHE == 1 )) && build_args+=(--no-cache)
    (( NO_PULL == 0 )) && build_args+=(--pull)
    compose_cmd "${build_args[@]}"
    compose_cmd up -d --force-recreate --remove-orphans
  fi
  ok "当前源码的账号网页和权威服务器已更新并启动"
}

wait_healthy() {
  step "等待服务健康检查"
  local i status http_base health
  http_base="$(local_http_base_url)"
  for ((i=1; i<=STARTUP_TIMEOUT_SECONDS; i++)); do
    health="$(curl -fsS --max-time 2 "$http_base/api/health" 2>/dev/null || true)"
    if [[ "$health" == *"\"sourceDigest\":\"$WARGAME_SOURCE_DIGEST\""* ]] \
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
  printf '%s\n' "$ADMIN_PASSWORD" | compose_cmd exec -T account-web \
    python /app/reset_admin.py >/dev/null
  ok "管理员密码已重置，旧管理会话已失效"
}

verify_admin_login() {
  step "验证管理员账号"
  local response payload http_base
  http_base="$(local_http_base_url)"
  payload="{\"username\":\"$(json_escape "$ADMIN_USERNAME")\",\"password\":\"$(json_escape "$ADMIN_PASSWORD")\"}"
  response="$(printf '%s' "$payload" | curl -fsS --max-time 5 \
    -H 'Content-Type: application/json' \
    --data-binary @- \
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
  printf '%s' "$overview" | grep -q "\"sourceDigest\":\"$WARGAME_SOURCE_DIGEST\"" \
    || die "账号网页与游戏服务源码摘要不一致"
  if [[ "$WEB_SHELL_ENABLED" == "true" ]]; then
    terminal_payload="{\"password\":\"$(json_escape "$ADMIN_PASSWORD")\"}"
    terminal_response="$(printf '%s' "$terminal_payload" | curl -fsS --max-time 5 -X POST \
      -H 'Content-Type: application/json' \
      -H "Authorization: Bearer $ADMIN_TOKEN" \
      --data-binary @- \
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
  local web_host="$PUBLIC_HOST" ws_host="$PUBLIC_HOST" web_url
  if [[ "$BIND_ADDRESS" == "127.0.0.1" || "$BIND_ADDRESS" == "localhost" ]]; then web_host="localhost"; ws_host="localhost"; fi
  web_url="http://$web_host:$HTTP_PORT"
  [[ "$PUBLIC_GAME_WS_URL" == wss://* ]] && web_url="https://$web_host"
  printf '\n%b\n' "${C_GREEN}  ╔══════════════════════════════════════════════════════════════╗${C_RESET}"
  printf '%b\n' "${C_GREEN}  ║                 Gbr 部署完成 · 服务正常                     ║${C_RESET}"
  printf '%b\n' "${C_GREEN}  ╚══════════════════════════════════════════════════════════════╝${C_RESET}"
  printf '%b\n' "\n  管理网页：${C_WHITE}$web_url${C_RESET}"
  printf '%b\n' "  服务器监控：${C_WHITE}$web_url（管理员登录后）${C_RESET}"
  printf '%b\n' "  客户端账号服务器：${C_WHITE}$web_url${C_RESET}"
  printf '%b\n' "  客户端 WebSocket：${C_WHITE}$PUBLIC_GAME_WS_URL${C_RESET}"
  printf '%b\n' "  源码摘要：${C_WHITE}$WARGAME_SOURCE_DIGEST${C_RESET}"
  printf '%b\n' "  管理员用户名：${C_WHITE}$ADMIN_USERNAME${C_RESET}"
  printf '%b\n' "  管理员会话：${C_WHITE}${SESSION_HOURS} 小时${C_RESET}"
  printf '%b\n' "  网页 Shell：${C_WHITE}${WEB_SHELL_ENABLED}（凭证 ${WEB_SHELL_TICKET_SECONDS}s，会话 ${WEB_SHELL_SESSION_SECONDS}s，并发 ${WEB_SHELL_MAX_SESSIONS}）${C_RESET}"
  printf '%b\n' "  配置文件：${C_WHITE}$ENV_FILE${C_RESET}"
  printf '%b\n' "  查看日志：docker compose --project-name $WARGAME_COMPOSE_PROJECT --env-file $WARGAME_RUNTIME_ENV_FILE -f $COMPOSE_FILE logs --tail=100"
}

main() {
  print_banner
  for arg in "$@"; do
    case "$arg" in
      -h|--help) usage; exit 0 ;;
    esac
  done
  parse_bootstrap_args "$@"
  resolve_source_input
  if [[ -f "$SOURCE_ROOT/CMakeLists.txt" ]]; then
    source_version="$(sed -nE 's/^project\([^)]* VERSION ([0-9][0-9A-Za-z._-]*).*/\1/p' "$SOURCE_ROOT/CMakeLists.txt" | head -n1)"
    [[ -z "$source_version" ]] || WARGAME_VERSION="$source_version"
  fi
  resolve_runtime_layout "$@"
  read_existing_env
  parse_args "$@"
  stage_source_tree
  command -v sha256sum >/dev/null 2>&1 || die "缺少命令：sha256sum"
  WARGAME_SOURCE_DIGEST="$(release_compute_source_digest "$STAGING_DIR")"
  [[ "$WARGAME_SOURCE_DIGEST" =~ ^[0-9a-f]{64}$ ]] || die "无法计算源码摘要"
  if [[ -f "$STAGING_DIR/release-identity.txt" ]]; then
    local packaged_version
    packaged_version="$(release_identity_value "$STAGING_DIR/release-identity.txt" wargameVersion 2>/dev/null || true)"
    [[ "$packaged_version" =~ ^[A-Za-z0-9._-]{1,32}$ ]] \
      || die "发布包三端身份缺少有效版本"
    if (( WARGAME_VERSION_SET == 1 )) && [[ "$WARGAME_VERSION" != "$packaged_version" ]]; then
      die "--wargame-version 与发布包三端身份不一致"
    fi
    WARGAME_VERSION="$packaged_version"
    release_validate_identity "$STAGING_DIR/release-identity.txt" \
      "$STAGING_DIR/deploy/release-manifest.env" "$WARGAME_VERSION" "$WARGAME_SOURCE_DIGEST" \
      || die "发布包三端身份校验失败"
  else
    warn "发布输入未提供 release-identity.txt；将只校验 v8/schema 8 清单"
  fi
  WARGAME_RELEASE_ID="${WARGAME_VERSION}-${WARGAME_SOURCE_DIGEST:0:12}"
  migrate_legacy_env
  read_existing_env
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
  activate_staged_source
  deploy_services
  wait_healthy
  reset_admin_if_requested
  verify_admin_login
  verify_monitoring
  logout_verification_session
  print_result
}

main "$@"
