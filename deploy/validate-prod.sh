#!/usr/bin/env bash
set -euo pipefail

root_dir="${1:-/opt/wargame}"
env_file="${root_dir}/.env"
config_file="${root_dir}/server.prod.json"

for required_file in "${env_file}" "${config_file}" "${root_dir}/compose.yml"; do
  if [[ ! -f "${required_file}" ]]; then
    echo "缺少生产部署文件: ${required_file}" >&2
    exit 1
  fi
done
test "$(stat -c '%a' "${env_file}")" = 600
pepper="$(sed -n 's/^WARGAME_TOKEN_PEPPER=//p' "${env_file}" | head -n 1)"
version="$(sed -n 's/^WARGAME_VERSION=//p' "${env_file}" | head -n 1)"
host="$(sed -n 's/^WARGAME_PUBLIC_HOST=//p' "${env_file}" | head -n 1)"
test "${#pepper}" -ge 32
test -n "${version}" && test "${version}" != "latest"
test -n "${host}" && test "${host}" != "game.example.com"
if grep -q '0000000000000000000000000000000000000000000000000000000000000000' "${config_file}"; then
  echo "生产配置仍使用零 token hash" >&2
  exit 1
fi
if grep -q 'replace-with\|example.com' "${env_file}"; then
  echo "生产环境文件仍包含占位符" >&2
  exit 1
fi
docker compose --env-file "${env_file}" -f "${root_dir}/compose.yml" config --quiet
printf 'production configuration passed\n'
