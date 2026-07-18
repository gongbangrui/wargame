#!/usr/bin/env bash
set -euo pipefail

root_dir="${1:-/opt/wargame}"
base_url="${2:-https://${WARGAME_PUBLIC_HOST:-game.example.com}}"
url_host="${base_url#*://}"
url_host="${url_host%%/*}"
if [[ -n "${WARGAME_WS_HOST:-}" ]]; then
  ws_host="${WARGAME_WS_HOST}"
  ws_port="${WARGAME_WS_PORT:-443}"
elif [[ "${url_host}" == *:* ]]; then
  ws_host="${url_host%:*}"
  ws_port="${url_host##*:}"
else
  ws_host="${url_host}"
  ws_port="${WARGAME_WS_PORT:-443}"
fi
ws_tls="${WARGAME_WS_TLS:-1}"
curl_args=(--fail --silent --show-error)
if [[ "${WARGAME_CURL_INSECURE:-0}" == "1" ]]; then
  curl_args+=(--insecure)
fi

test -f "${root_dir}/compose.yml"
curl "${curl_args[@]}" "${base_url}/healthz" | rg '"status":"ok"'
curl "${curl_args[@]}" "${base_url}/healthz" >/dev/null
curl "${curl_args[@]}" "${base_url}/metrics" | grep -q '^wargame_connections '

if [[ -n "${WARGAME_VALID_TOKEN:-}" && -n "${WARGAME_PROBE_SCRIPT:-}" ]]; then
  WARGAME_WS_HOST="${ws_host}" WARGAME_WS_PORT="${ws_port}" WARGAME_WS_TLS="${ws_tls}" \
    WARGAME_VALID_TOKEN="${WARGAME_VALID_TOKEN}" \
    WARGAME_INVALID_TOKEN="${WARGAME_INVALID_TOKEN:-invalid-token}" \
    python3 "${WARGAME_PROBE_SCRIPT}"
fi

docker compose --env-file "${root_dir}/.env" -f "${root_dir}/compose.yml" ps
printf 'staging check passed\n'
