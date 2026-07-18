#!/usr/bin/env bash
set -euo pipefail
umask 077

root_dir="${1:-/opt/wargame}"
timestamp="$(date -u +%Y%m%dT%H%M%SZ)"
backup_dir="${root_dir}/backups"
backup_file="${backup_dir}/wargame-${timestamp}.sqlite3"
container_backup="/app/data/.wargame-backup-${timestamp}.sqlite3"
mkdir -p "${backup_dir}"
test -d "${root_dir}/data"

cleanup() {
  rm -f "${root_dir}/data/.wargame-backup-${timestamp}.sqlite3"
}
trap cleanup EXIT

docker compose --env-file "${root_dir}/.env" \
  -f "${root_dir}/compose.yml" exec -T server \
  /app/bin/wargame_server --config /app/config/server.json \
  --backup "${container_backup}"
test -s "${root_dir}/data/.wargame-backup-${timestamp}.sqlite3"
mv "${root_dir}/data/.wargame-backup-${timestamp}.sqlite3" "${backup_file}"
sha256sum "${backup_file}" > "${backup_file}.sha256"
sha256sum --check "${backup_file}.sha256"
find "${backup_dir}" -type f -name 'wargame-*.sqlite3*' -mtime +7 -delete
printf 'Backup: %s\n' "${backup_file}"
