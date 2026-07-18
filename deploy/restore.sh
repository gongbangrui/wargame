#!/usr/bin/env bash
set -euo pipefail
umask 077

backup_file="${1:?Usage: restore.sh BACKUP_FILE [ROOT_DIR]}"
root_dir="${2:-/opt/wargame}"
test -f "${backup_file}"
test -f "${backup_file}.sha256"
sha256sum --check "${backup_file}.sha256"

docker compose --env-file "${root_dir}/.env" -f "${root_dir}/compose.yml" stop server
mkdir -p "${root_dir}/backups"
if test -f "${root_dir}/data/wargame.sqlite3"; then
  cp -a "${root_dir}/data/wargame.sqlite3" \
    "${root_dir}/backups/pre-restore-$(date -u +%Y%m%dT%H%M%SZ).sqlite3"
fi
temp_database="${root_dir}/data/.wargame.sqlite3.restore.tmp"
cp "${backup_file}" "${temp_database}"
test -s "${temp_database}"
# The backup itself is intentionally mode 0600; the live database is writable
# by the container owner (UID 10001 in production). A wider mode is available
# only for rootless staging directories whose ownership cannot be changed.
restore_mode="${WARGAME_RESTORE_MODE:-660}"
if [[ ! "${restore_mode}" =~ ^6[0-6][0-6]$ ]]; then
  echo "WARGAME_RESTORE_MODE 必须是 600-666 的三位权限" >&2
  exit 2
fi
chmod "${restore_mode}" "${temp_database}"
mv -f "${temp_database}" "${root_dir}/data/wargame.sqlite3"
docker compose --env-file "${root_dir}/.env" -f "${root_dir}/compose.yml" up -d server
if ! "${root_dir}/wait-healthy.sh" "${root_dir}" 90; then
  echo "恢复后的数据库无法通过健康检查，请检查 ${root_dir}/data 的 UID 10001 权限" >&2
  exit 1
fi
