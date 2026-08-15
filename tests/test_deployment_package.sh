#!/usr/bin/env bash
set -Eeuo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
work_dir="$(mktemp -d)"
trap 'rm -rf -- "$work_dir"' EXIT

failures=0

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    failures=$((failures + 1))
}

require_file() {
    [[ -f "$1" ]] || fail "missing file: $1"
}

make_fixture() {
    local fixture="$1"
    mkdir -p "$fixture"
    tar -C "$repo_root" \
        --exclude='*/__pycache__' \
        --exclude='*.py[cod]' \
        --exclude='*.db*' \
        --exclude='*.sqlite*' \
        --exclude='*.jsonl' \
        --exclude='*.log' \
        --exclude='*/build' \
        --exclude='*/cache' \
        --exclude='*/.cache' \
        --exclude='*/logs' \
        --exclude='*/backups' \
        -cf - CMakeLists.txt cmake src server map/metadata.json deploy .dockerignore README.md docs \
        Main.qml main.cpp qml \
        | tar -C "$fixture" -xf -
}

clean_fixture="$work_dir/clean"
make_fixture "$clean_fixture"
clean_dist="$clean_fixture/dist"
WARGAME_VERSION=fixture DIST_DIR="$clean_dist" "$clean_fixture/deploy/package-one-click.sh"

archive="$clean_dist/wargame-server-fixture.tar.gz"
manifest="$archive.sha256"
require_file "$archive"
require_file "$manifest"
if [[ -f "$archive" && -f "$manifest" ]]; then
    (cd "$clean_dist" && sha256sum -c "$(basename "$manifest")") || fail "checksum verification failed"
fi

if [[ -f "$archive" ]]; then
    listing="$work_dir/listing.txt"
    tar -tzf "$archive" >"$listing"
    for required in \
        'wargame-server-fixture/server/account/static/index.html' \
        'wargame-server-fixture/server/account/static/app.js' \
        'wargame-server-fixture/server/account/static/app.css' \
        'wargame-server-fixture/server/game/main.cpp' \
        'wargame-server-fixture/map/metadata.json' \
        'wargame-server-fixture/deploy/install-server.sh' \
        'wargame-server-fixture/deploy/account.Dockerfile' \
        'wargame-server-fixture/deploy/game-server.Dockerfile' \
        'wargame-server-fixture/deploy/release-lib.sh' \
        'wargame-server-fixture/deploy/release-manifest.env' \
        'wargame-server-fixture/release-identity.txt' \
        'wargame-server-fixture/Main.qml' \
        'wargame-server-fixture/main.cpp' \
        'wargame-server-fixture/deploy/QUICK_START.md'; do
        grep -Fqx "$required" "$listing" || fail "archive is missing: $required"
    done
    if grep -Eq '(^|/)(\.env[^/]*|[^/]+\.(db|sqlite|sqlite3|jsonl|log|cache|pyc|pyo)|__pycache__|build|cache|\.cache|logs|backups)(/|$)|(^|/)(room-)?(checkpoint|status|events)([^/]*)(/|$)' "$listing"; then
        fail "archive contains a prohibited runtime artifact"
    fi
fi

deterministic_first="$work_dir/deterministic-first"
deterministic_second="$work_dir/deterministic-second"
mkdir -p "$deterministic_first" "$deterministic_second"
WARGAME_VERSION=deterministic DIST_DIR="$deterministic_first" \
    "$clean_fixture/deploy/package-one-click.sh" >/dev/null
touch -d '2001-02-03 04:05:06 UTC' "$clean_fixture/src/core/SimulationEngine.cpp"
WARGAME_VERSION=deterministic DIST_DIR="$deterministic_second" \
    "$clean_fixture/deploy/package-one-click.sh" >/dev/null
cmp -s \
    "$deterministic_first/wargame-server-deterministic.tar.gz" \
    "$deterministic_second/wargame-server-deterministic.tar.gz" \
    || fail "package archive is not deterministic"
cmp -s \
    "$deterministic_first/wargame-server-deterministic.tar.gz.sha256" \
    "$deterministic_second/wargame-server-deterministic.tar.gz.sha256" \
    || fail "package checksum is not deterministic"

rejected_fixture="$work_dir/rejected"
make_fixture "$rejected_fixture"
touch "$rejected_fixture/server/account/runtime.db"
rejected_dist="$rejected_fixture/dist"
if WARGAME_VERSION=rejected DIST_DIR="$rejected_dist" "$rejected_fixture/deploy/package-one-click.sh"; then
    fail "package accepted a runtime database fixture"
fi
[[ ! -e "$rejected_dist/wargame-server-rejected.tar.gz" ]] \
    || fail "runtime fixture published a final archive"

stale_fixture="$work_dir/stale"
make_fixture "$stale_fixture"
stale_dist="$stale_fixture/dist"
mkdir -p "$stale_dist"
stale_archive="$stale_dist/wargame-server-stale.tar.gz"
printf 'stale\n' >"$stale_archive"
if WARGAME_VERSION=stale DIST_DIR="$stale_dist" "$stale_fixture/deploy/package-one-click.sh"; then
    fail "package accepted a pre-existing final archive"
fi
[[ ! -e "$stale_archive.sha256" ]] || fail "stale archive gained a checksum sidecar"

if (( failures > 0 )); then
    printf 'deployment package test failed: %d assertion(s)\n' "$failures" >&2
    exit 1
fi

printf 'PASS: deployment package artifact contract\n'
