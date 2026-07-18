#!/usr/bin/env bash
set -euo pipefail

new_version="${1:?Usage: upgrade.sh VERSION [ROOT_DIR]}"
root_dir="${2:-/opt/wargame}"
env_file="${root_dir}/.env"
compose=(docker compose --env-file "${env_file}" -f "${root_dir}/compose.yml")

if [[ ! "${new_version}" =~ ^[A-Za-z0-9._-]+$ ]]; then
  echo "版本号只能包含字母、数字、点、下划线和连字符" >&2
  exit 2
fi
test -f "${env_file}"

previous_version="$(sed -n 's/^WARGAME_VERSION=//p' "${env_file}" | head -n 1)"
if [[ -z "${previous_version}" ]]; then
  echo "${env_file} 缺少 WARGAME_VERSION" >&2
  exit 2
fi
if [[ "${previous_version}" == "${new_version}" ]]; then
  echo "已在运行版本 ${new_version}" >&2
  exit 0
fi

"${root_dir}/backup.sh" "${root_dir}"
sed -i "s/^WARGAME_VERSION=.*/WARGAME_VERSION=${new_version}/" "${env_file}"

rollback() {
  echo "升级失败，回滚至 ${previous_version}" >&2
  sed -i "s/^WARGAME_VERSION=.*/WARGAME_VERSION=${previous_version}/" "${env_file}"
  "${compose[@]}" up -d --no-deps server
  "${root_dir}/wait-healthy.sh" "${root_dir}" 90 || true
}

if ! "${compose[@]}" pull server \
    || ! "${compose[@]}" up -d --no-deps server \
    || ! "${root_dir}/wait-healthy.sh" "${root_dir}" 90; then
  rollback
  exit 1
fi

echo "服务器已升级至 ${new_version}"
