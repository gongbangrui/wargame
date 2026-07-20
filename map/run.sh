#!/usr/bin/env bash
set -euo pipefail

map_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd -- "$map_dir/.." && pwd)"

if [[ -x "$repo_dir/build/appindex" ]]; then
    app="$repo_dir/build/appindex"
elif [[ -x "$repo_dir/build/Debug/appindex" ]]; then
    app="$repo_dir/build/Debug/appindex"
else
    echo "未找到 appindex，请先按 AGENTS.md 使用 CMake/Ninja 构建项目。" >&2
    exit 1
fi

exec "$app" "$@"
