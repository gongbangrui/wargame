#!/usr/bin/env bash
set -euo pipefail

base_url="${1:-https://game.example.com}"
response="$(curl --fail --silent --show-error "${base_url}/healthz")"
printf '%s\n' "${response}" | rg '"status":"ok"'
curl --fail --silent --show-error "${base_url}/healthz" >/dev/null
curl --fail --silent --show-error "${base_url}/metrics" | grep -q '^wargame_connections '
printf 'smoke test passed\n'
