#!/usr/bin/env bash
set -euo pipefail

root_dir="${1:-/opt/wargame}"
timeout_seconds="${2:-90}"
compose=(docker compose --env-file "${root_dir}/.env" -f "${root_dir}/compose.yml")

if ! [[ "${timeout_seconds}" =~ ^[1-9][0-9]*$ ]]; then
  echo "健康检查超时必须是正整数秒" >&2
  exit 2
fi

deadline=$((SECONDS + timeout_seconds))
until "${compose[@]}" exec -T server /app/bin/wargame_server \
    --health-probe http://127.0.0.1:9090/healthz; do
  if (( SECONDS >= deadline )); then
    echo "服务器在 ${timeout_seconds} 秒内未通过健康检查" >&2
    exit 1
  fi
  sleep 3
done
