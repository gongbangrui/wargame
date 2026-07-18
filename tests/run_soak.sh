#!/usr/bin/env bash
set -euo pipefail

binary="${1:-./build/wargame_network_soak}"
output_dir="${2:-output/network-tests}"
duration_seconds="${3:-28800}"
test -x "${binary}"
mkdir -p "${output_dir}"
report="${output_dir}/soak-$(date -u +%Y%m%dT%H%M%SZ).json"

exec "${binary}" --duration-seconds "${duration_seconds}" --clients 32 --units 500 \
  --report "${report}"
