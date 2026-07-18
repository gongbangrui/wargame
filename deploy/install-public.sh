#!/usr/bin/env bash
set -euo pipefail
umask 077

readonly SCRIPT_AUTHOR="GBR"
readonly DEFAULT_ROOT="/opt/wargame"
readonly DEFAULT_SOURCE_DIR="/opt/wargame-source"
readonly DEFAULT_SOURCE_URL="https://github.com/gongbangrui/wargame.git"
readonly DEFAULT_VERSION="0.1.0"

COLOR_RESET='\033[0m'
COLOR_BLUE='\033[38;5;39m'
COLOR_CYAN='\033[38;5;51m'
COLOR_GREEN='\033[38;5;82m'
COLOR_YELLOW='\033[38;5;220m'
COLOR_RED='\033[38;5;203m'
COLOR_GRAY='\033[38;5;245m'

install_root="$DEFAULT_ROOT"
source_dir="$DEFAULT_SOURCE_DIR"
source_url="$DEFAULT_SOURCE_URL"
public_host=""
admin_user="admin"
admin_password=""
version="$DEFAULT_VERSION"
assume_yes=0
force=0

print_banner() {
    printf '\n'
    printf "${COLOR_CYAN}============================================================${COLOR_RESET}\n"
    printf "${COLOR_BLUE}                 兵器推演公网部署向导${COLOR_RESET}\n"
    printf "${COLOR_GRAY}                    作者：%s${COLOR_RESET}\n" "$SCRIPT_AUTHOR"
    printf "${COLOR_CYAN}============================================================${COLOR_RESET}\n\n"
}

step() { printf "${COLOR_BLUE}[%s]${COLOR_RESET} %s\n" "$1" "$2"; }
ok() { printf "${COLOR_GREEN}  OK${COLOR_RESET}  %s\n" "$1"; }
warn() { printf "${COLOR_YELLOW}WARN${COLOR_RESET}  %s\n" "$1" >&2; }
die() { printf "${COLOR_RED}ERROR${COLOR_RESET} %s\n" "$1" >&2; exit 1; }

usage() {
    cat <<'EOF'
用法：sudo bash deploy/install-public.sh [选项]

选项：
  --domain DOMAIN          公网域名，例如 game.example.com。
  --admin-user USER        管理员账号，默认 admin。
  --admin-password PASS    管理员密码；省略则随机生成。
  --install-dir DIR        部署目录，默认 /opt/wargame。
  --source-url URL         源码 Git 地址，默认 GBR 官方仓库。
  --source-dir DIR         裸机下载源码的目录，默认 /opt/wargame-source。
  --version VERSION        本地镜像标签，默认 0.1.0。
  --yes                    跳过确认提示。
  --force                  替换已有但尚未创建数据库的部署配置。
  --help                   显示此帮助。

前置条件：
  - DOMAIN 在安装前必须已解析至本服务器。
  - TCP 80 和 443 必须可从公网访问，才能启用 HTTPS/WSS。
  - 脚本可单独上传至裸服务器；它会自动安装依赖并下载完整源码。
EOF
}

while (($#)); do
    case "$1" in
        --domain) public_host="${2:?--domain requires a value}"; shift 2 ;;
        --admin-user) admin_user="${2:?--admin-user requires a value}"; shift 2 ;;
        --admin-password) admin_password="${2:?--admin-password requires a value}"; shift 2 ;;
        --install-dir) install_root="${2:?--install-dir requires a value}"; shift 2 ;;
        --source-url) source_url="${2:?--source-url requires a value}"; shift 2 ;;
        --source-dir) source_dir="${2:?--source-dir requires a value}"; shift 2 ;;
        --version) version="${2:?--version requires a value}"; shift 2 ;;
        --yes) assume_yes=1; shift ;;
        --force) force=1; shift ;;
        --help|-h) usage; exit 0 ;;
        *) die "未知参数: $1" ;;
    esac
done

source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

require_root() {
    [[ "${EUID}" -eq 0 ]] || die "请使用 sudo 运行此安装器。"
}

validate_inputs() {
    [[ "${public_host}" =~ ^[A-Za-z0-9]([A-Za-z0-9.-]*[A-Za-z0-9])?$ ]] \
        || die "域名格式无效。"
    [[ "${public_host}" != *..* && "${public_host}" != *://* && "${public_host}" != *:* ]] \
        || die "域名不能包含协议、端口或连续的点。"
    [[ "${admin_user}" =~ ^[A-Za-z0-9_.-]{1,64}$ ]] || die "管理员账号只能使用字母、数字、点、下划线和连字符。"
    [[ "${version}" =~ ^[A-Za-z0-9._-]+$ ]] || die "版本号只能使用字母、数字、点、下划线和连字符。"
    [[ "${source_url}" == https://* && "${source_url}" != *$'\n'* && "${source_url}" != *' '* ]] \
        || die "源码地址必须是有效的 HTTPS Git 地址。"
    [[ "${source_dir}" != "${install_root}" ]] || die "源码目录不能与部署目录相同。"
    if [[ -n "${admin_password}" && ${#admin_password} -lt 12 ]]; then
        die "管理员密码至少需要 12 个字符。"
    fi
}

ask() {
    local prompt="$1"
    local default_value="${2:-}"
    local value
    if ((assume_yes)); then
        printf '%s' "${default_value}"
        return
    fi
    if [[ -n "${default_value}" ]]; then
        read -r -p "${prompt} [${default_value}]: " value
        printf '%s' "${value:-${default_value}}"
    else
        read -r -p "${prompt}: " value
        printf '%s' "${value}"
    fi
}

bootstrap_host() {
    step "1/8" "检测裸机环境并安装基础依赖"
    command -v apt-get >/dev/null 2>&1 || die "当前仅支持 Debian/Ubuntu 的 apt-get 自动安装。"
    . /etc/os-release
    [[ "${ID}" == "debian" || "${ID}" == "ubuntu" ]] || die "当前仅支持 Debian 或 Ubuntu。"
    apt-get update
    apt-get install -y ca-certificates curl git gnupg iproute2 openssl
    ok "基础工具已就绪（curl/git/gpg/openssl/iproute2）"
    if command -v docker >/dev/null 2>&1 && docker compose version >/dev/null 2>&1; then
        systemctl enable --now docker
        ok "Docker Engine 和 Compose 插件已就绪"
        return
    fi
    step "1/8" "安装 Docker Engine 与 Compose 插件"
    install -m 0755 -d /etc/apt/keyrings
    curl -fsSL "https://download.docker.com/linux/${ID}/gpg" | gpg --dearmor --yes -o /etc/apt/keyrings/docker.gpg
    chmod a+r /etc/apt/keyrings/docker.gpg
    printf 'deb [arch=%s signed-by=/etc/apt/keyrings/docker.gpg] https://download.docker.com/linux/%s %s stable\n' \
        "$(dpkg --print-architecture)" "${ID}" "${VERSION_CODENAME}" > /etc/apt/sources.list.d/docker.list
    apt-get update
    apt-get install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin
    systemctl enable --now docker
    docker compose version >/dev/null || die "Docker Compose 插件安装失败。"
    command -v openssl >/dev/null 2>&1 || die "缺少 openssl，无法生成安全凭据。"
    ok "Docker 已安装"
}

ensure_source() {
    if [[ -f "${source_root}/CMakeLists.txt" && -f "${source_root}/deploy/Containerfile" ]]; then
        ok "使用当前完整源码目录: ${source_root}"
        return
    fi
    step "2/8" "下载 GBR 兵器推演源码"
    if [[ -e "${source_dir}" ]]; then
        if [[ -f "${source_dir}/CMakeLists.txt" && -f "${source_dir}/deploy/Containerfile" ]]; then
            source_root="${source_dir}"
            ok "复用已下载源码: ${source_root}"
            return
        fi
        [[ ${force} -eq 1 ]] || die "源码目录已存在但不完整: ${source_dir}；确认替换请添加 --force。"
        rm -rf "${source_dir}"
    fi
    git clone --depth 1 "${source_url}" "${source_dir}"
    source_root="${source_dir}"
    [[ -f "${source_root}/CMakeLists.txt" && -f "${source_root}/deploy/Containerfile" ]] \
        || die "下载的源码不包含服务器部署文件，请检查 --source-url。"
    ok "源码下载完成: ${source_root}"
}

check_ports() {
    step "3/8" "检查公网端口与 DNS"
    if command -v ss >/dev/null 2>&1; then
        local occupied
        occupied="$(ss -ltnH '( sport = :80 or sport = :443 )' 2>/dev/null || true)"
        [[ -z "${occupied}" ]] || die "TCP 80 或 443 已被占用，请先停止占用服务。"
    fi
    if command -v getent >/dev/null 2>&1 && ! getent ahosts "${public_host}" >/dev/null; then
        warn "${public_host} 当前未解析到 IP；Caddy 无法签发 HTTPS 证书。"
        ((assume_yes)) && die "非交互模式要求域名已解析。"
    else
        ok "DNS 查询已通过或将在 Caddy 启动时验证"
    fi
    if command -v ufw >/dev/null 2>&1 && ufw status | grep -q '^Status: active'; then
        ufw allow 80/tcp
        ufw allow 443/tcp
        ok "已放行 UFW TCP 80/443"
    fi
}

generate_secret() { openssl rand -hex 32; }
generate_password() { openssl rand -base64 24 | tr -d '\n'; }

hash_token() {
    local raw_token="$1"
    printf '%s:%s' "${token_pepper}" "${raw_token}" | sha256sum | awk '{print $1}'
}

hash_password() {
    local password="$1"
    local output salt hash
    output="$(printf '%s\n' "${password}" | docker run -i --rm "${image_name}" --hash-password-stdin)"
    salt="$(sed -n 's/.*"salt":"\([0-9a-f]*\)".*/\1/p' <<<"${output}")"
    hash="$(sed -n 's/.*"passwordHash":"\([0-9a-f]*\)".*/\1/p' <<<"${output}")"
    [[ "${salt}" =~ ^[0-9a-f]{32}$ && "${hash}" =~ ^[0-9a-f]{64}$ ]] \
        || die "密码哈希生成失败。"
    printf '%s:%s' "${salt}" "${hash}"
}

stage_deployment() {
    step "5/8" "生成服务器配置与初始凭据"
    if [[ -e "${install_root}" && ! -d "${install_root}" ]]; then
        die "部署路径不是目录: ${install_root}"
    fi
    if [[ -f "${install_root}/.env" && ${force} -ne 1 ]]; then
        die "检测到已有部署: ${install_root}。确认覆盖后请添加 --force。"
    fi
    if [[ -e "${install_root}/data/wargame.sqlite3" ]]; then
        die "检测到已有 SQLite 数据库；请使用 upgrade.sh 管理现有部署，安装器只用于首次部署。"
    fi
    install -d -m 0750 "${install_root}" "${install_root}/data" "${install_root}/logs" "${install_root}/backups"
    cp -a "${source_root}/deploy/." "${install_root}/"
    rm -f "${install_root}/.env" "${install_root}/server.prod.json"
    chown -R 10001:10001 "${install_root}/data" "${install_root}/logs"

    token_pepper="$(generate_secret)"
    red_token="$(generate_secret)"
    blue_token="$(generate_secret)"
    director_token="$(generate_secret)"
    red_password="$(generate_password)"
    blue_password="$(generate_password)"
    director_password="$(generate_password)"
    [[ -n "${admin_password}" ]] || admin_password="$(generate_password)"
    expires_at="$(date -u -d '+5 years' +%Y-%m-%dT%H:%M:%SZ)"

    IFS=: read -r admin_salt admin_hash <<<"$(hash_password "${admin_password}")"
    IFS=: read -r red_salt red_hash <<<"$(hash_password "${red_password}")"
    IFS=: read -r blue_salt blue_hash <<<"$(hash_password "${blue_password}")"
    IFS=: read -r director_salt director_hash <<<"$(hash_password "${director_password}")"

    cat > "${install_root}/.env" <<EOF
WARGAME_VERSION=${version}
WARGAME_SERVER_IMAGE=wargame-server
WARGAME_PUBLIC_HOST=${public_host}
WARGAME_TOKEN_PEPPER=${token_pepper}
EOF
    cat > "${install_root}/server.prod.json" <<EOF
{
  "listenAddress": "0.0.0.0",
  "port": 8080,
  "healthAddress": "0.0.0.0",
  "healthPort": 9090,
  "adminAddress": "0.0.0.0",
  "adminPort": 9091,
  "databasePath": "/app/data/wargame.sqlite3",
  "scenarioPath": "",
  "roomId": "main",
  "snapshotIntervalMs": 100,
  "checkpointIntervalMs": 10000,
  "maxConnections": 32,
  "maxPacketBytes": 262144,
  "maxSendQueueBytes": 1048576,
  "commandRatePerSecond": 20,
  "commandBurst": 40,
  "allowPublicListen": true,
  "tokens": [
    {"tokenHash": "$(hash_token "${red_token}")", "userId": "red-token", "role": "red", "side": "red", "roomId": "main", "expiresAt": "${expires_at}"},
    {"tokenHash": "$(hash_token "${blue_token}")", "userId": "blue-token", "role": "blue", "side": "blue", "roomId": "main", "expiresAt": "${expires_at}"},
    {"tokenHash": "$(hash_token "${director_token}")", "userId": "director-token", "role": "director", "side": "", "roomId": "main", "expiresAt": "${expires_at}"}
  ],
  "accounts": [
    {"username": "red", "salt": "${red_salt}", "passwordHash": "${red_hash}", "userId": "red-player", "role": "red", "side": "red", "roomId": "main", "expiresAt": "${expires_at}"},
    {"username": "blue", "salt": "${blue_salt}", "passwordHash": "${blue_hash}", "userId": "blue-player", "role": "blue", "side": "blue", "roomId": "main", "expiresAt": "${expires_at}"},
    {"username": "director", "salt": "${director_salt}", "passwordHash": "${director_hash}", "userId": "director-player", "role": "director", "side": "", "roomId": "main", "expiresAt": "${expires_at}"}
  ],
  "admin": {"username": "${admin_user}", "salt": "${admin_salt}", "passwordHash": "${admin_hash}"}
}
EOF
    cat > "${install_root}/INITIAL_CREDENTIALS.txt" <<EOF
兵器推演初始凭据
生成时间: $(date -u +%Y-%m-%dT%H:%M:%SZ)
公网域名: ${public_host}
客户端 WebSocket: wss://${public_host}/ws
管理员平台: https://${public_host}/admin

管理员
  账号: ${admin_user}
  密码: ${admin_password}

账号密码登录
  red       密码: ${red_password}
  blue      密码: ${blue_password}
  director  密码: ${director_password}

Token 登录
  red:      ${red_token}
  blue:     ${blue_token}
  director: ${director_token}
EOF
    chmod 600 "${install_root}/.env" "${install_root}/server.prod.json" "${install_root}/INITIAL_CREDENTIALS.txt"
    ok "已写入安全配置与初始凭据"
}

build_image() {
    step "4/8" "构建无头服务器镜像（首次执行可能需要数分钟）"
    image_name="wargame-server:${version}"
    docker build -f "${source_root}/deploy/Containerfile" -t "${image_name}" "${source_root}"
    ok "镜像构建完成: ${image_name}"
}

launch() {
    step "6/8" "校验并启动容器服务"
    docker compose --env-file "${install_root}/.env" -f "${install_root}/compose.yml" config --quiet
    "${install_root}/validate-prod.sh" "${install_root}"
    docker compose --env-file "${install_root}/.env" -f "${install_root}/compose.yml" up -d
    "${install_root}/wait-healthy.sh" "${install_root}" 180
    ok "权威服务器健康检查通过"
}

show_result() {
    step "7/8" "等待 HTTPS/WSS 就绪"
    local deadline=$((SECONDS + 180))
    until curl --fail --silent --show-error "https://${public_host}/healthz" | grep -q '"status":"ok"'; do
        if ((SECONDS >= deadline)); then
            die "HTTPS 未在 180 秒内就绪。请确认 DNS 已指向本机且 TCP 80/443 已放通，再查看 docker compose logs caddy。"
        fi
        sleep 5
    done
    step "8/8" "部署完成"
    printf '\n'
    printf "${COLOR_GREEN}  客户端 WebSocket${COLOR_RESET}  wss://%s/ws\n" "${public_host}"
    printf "${COLOR_GREEN}  管理平台         ${COLOR_RESET}  https://%s/admin\n" "${public_host}"
    printf "${COLOR_GREEN}  初始凭据         ${COLOR_RESET}  %s/INITIAL_CREDENTIALS.txt（权限 600）\n" "${install_root}"
    printf "${COLOR_GRAY}  客户端连接窗口可选择 Token 或账号密码登录。${COLOR_RESET}\n\n"
}

main() {
    print_banner
    require_root
    if [[ -z "${public_host}" ]]; then
        public_host="$(ask '公网域名（DNS 已指向本服务器）')"
    fi
    if [[ -z "${admin_password}" && ${assume_yes} -eq 0 ]]; then
        read -r -s -p '管理员密码（留空将自动生成）: ' admin_password
        printf '\n'
    fi
    validate_inputs
    if ((assume_yes == 0)); then
        printf "\n将部署到 ${COLOR_CYAN}%s${COLOR_RESET}，公开域名为 ${COLOR_CYAN}%s${COLOR_RESET}。\n" "${install_root}" "${public_host}"
        read -r -p '继续部署？[y/N] ' confirmation
        [[ "${confirmation}" =~ ^[Yy]$ ]] || die "已取消。"
    fi
    bootstrap_host
    ensure_source
    check_ports
    build_image
    stage_deployment
    launch
    show_result
}

main "$@"
