#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "用法: $0 <Debug 构建目录> <Sanitizer 构建目录>" >&2
  exit 2
fi

debug_build="$1"
sanitizer_build="$2"
for build_dir in "$debug_build" "$sanitizer_build"; do
  [[ -f "$build_dir/CTestTestfile.cmake" ]] || {
    echo "未找到 CTest 配置: $build_dir" >&2
    exit 2
  }
done

extract_test_names() {
  ctest --test-dir "$1" -N \
    | sed -n 's/^[[:space:]]*Test[[:space:]]*#[[:space:]]*[0-9][0-9]*: //p' \
    | LC_ALL=C sort
}

debug_tests="$(mktemp)"
sanitizer_tests="$(mktemp)"
trap 'rm -f "$debug_tests" "$sanitizer_tests"' EXIT
extract_test_names "$debug_build" >"$debug_tests"
extract_test_names "$sanitizer_build" >"$sanitizer_tests"

if ! diff -u "$debug_tests" "$sanitizer_tests"; then
  echo "Debug 与 Sanitizer 的测试注册不一致，请重新配置或修复 CMake。" >&2
  exit 1
fi

count="$(wc -l <"$debug_tests" | tr -d ' ')"
[[ "$count" -gt 0 ]] || { echo "未发现任何测试。" >&2; exit 1; }
echo "测试基线一致：$count 个测试。"
