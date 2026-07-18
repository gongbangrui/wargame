#!/usr/bin/env bash
set -euo pipefail

binary="${1:-./build/wargame_network_soak}"
output_dir="${2:-output/network-tests/netem-$(date -u +%Y%m%dT%H%M%SZ)}"
test -x "${binary}"
mkdir -p "${output_dir}"

cleanup() {
  tc qdisc del dev lo root 2>/dev/null || true
}
trap cleanup EXIT INT TERM

if ! tc qdisc show dev lo | rg -q 'noqueue'; then
  echo "loopback 已有非 noqueue qdisc，拒绝覆盖" >&2
  exit 2
fi

run_delay_case() {
  local name="$1"
  local delay="$2"
  local jitter="$3"
  tc qdisc replace dev lo root netem delay "${delay}" "${jitter}" distribution normal
  "${binary}" --duration-seconds 75 --clients 32 --units 500 \
    --report "${output_dir}/${name}.json"
  tc qdisc del dev lo root
}

run_delay_case delay50 50ms 10ms
run_delay_case delay200 200ms 100ms
run_delay_case delay500 500ms 100ms

"${binary}" --duration-seconds 85 --clients 32 --units 500 --reconnect-probe \
  --report "${output_dir}/outage30.json" &
soak_pid=$!
sleep 20
tc qdisc replace dev lo root netem loss 100%
sleep 30
tc qdisc del dev lo root
wait "${soak_pid}"

printf 'Reports: %s\n' "${output_dir}"
