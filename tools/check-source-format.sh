#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

source_globs=(
  --glob '*.cpp'
  --glob '*.h'
  --glob '*.qml'
  --glob '*.cmake'
  --glob 'CMakeLists.txt'
  --glob '*.yml'
  --glob '*.yaml'
  --glob '*.json'
  --glob '*.mjs'
  --glob '*.sh'
  --glob '!build/**'
  --glob '!map/**'
)

trailing="$(rg -n '[[:blank:]]+$' "${source_globs[@]}" . || true)"
if [[ -n "$trailing" ]]; then
  echo "发现行尾空白：" >&2
  echo "$trailing" >&2
  exit 1
fi

if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  git diff --check
fi

echo "源码格式检查通过"
