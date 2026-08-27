#!/usr/bin/env bash
set -Eeuo pipefail

# Build the desktop client and both server artifacts with one release identity.

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT_DIR/deploy/release-lib.sh"

PROJECT_VERSION="$(release_project_version "$ROOT_DIR")"
VERSION="${WARGAME_VERSION:-${PROJECT_VERSION:-2.0.0}}"
BUILD_ROOT="${WARGAME_BUILD_ROOT:-$ROOT_DIR/build/release}"
SERVER_BUILD_ROOT="${WARGAME_SERVER_BUILD_ROOT:-$ROOT_DIR/build/server-release}"
OUTPUT_ROOT="${WARGAME_RELEASE_OUTPUT:-}"
MANIFEST="$ROOT_DIR/deploy/release-manifest.env"

usage() {
  cat <<'EOF'
Usage: tools/build-release.sh [--version VERSION] [--clean]

Builds appindex, the root-project game server, and the Qt 6.4 standalone
game server with one v8/schema 8 release identity. Outputs are placed under
dist/release-VERSION unless WARGAME_RELEASE_OUTPUT is set.
EOF
}

CLEAN=0
while (($#)); do
  case "$1" in
    --version)
      [[ $# -ge 2 ]] || { printf 'missing --version value\n' >&2; exit 2; }
      VERSION="$2"
      shift 2
      ;;
    --clean)
      CLEAN=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      printf 'unknown option: %s\n' "$1" >&2
      exit 2
      ;;
  esac
done

[[ -n "$OUTPUT_ROOT" ]] || OUTPUT_ROOT="$ROOT_DIR/dist/release-$VERSION"
[[ "$VERSION" =~ ^[A-Za-z0-9._-]{1,32}$ ]] || {
  printf 'invalid release version: %s\n' "$VERSION" >&2
  exit 2
}
release_validate_manifest "$MANIFEST"
SOURCE_DIGEST="$(release_compute_source_digest "$ROOT_DIR")"
[[ "$SOURCE_DIGEST" =~ ^[0-9a-f]{64}$ ]] || {
  printf 'could not compute source digest\n' >&2
  exit 1
}

if (( CLEAN == 1 )); then
  rm -rf -- "$BUILD_ROOT" "$SERVER_BUILD_ROOT"
  # Remove only the exact versioned output directory so a clean build cannot
  # leave stale binaries or checksums beside the newly published artifacts.
  rm -rf -- "$OUTPUT_ROOT"
fi
mkdir -p -- "$OUTPUT_ROOT"

cmake -S "$ROOT_DIR" -B "$BUILD_ROOT" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF \
  -DWARGAME_BUILD_SERVER=ON \
  -DWARGAME_BUILD_VMF_TOOLS=ON \
  -DWARGAME_VERSION="$VERSION" \
  -DWARGAME_SOURCE_DIGEST="$SOURCE_DIGEST"
cmake --build "$BUILD_ROOT" --target appindex wargame_server --parallel

if cmake --build "$BUILD_ROOT" --target help | grep -q 'vmf_design_regression'; then
  cmake --build "$BUILD_ROOT" --target vmf_design_regression --parallel
else
  printf 'VMF design regression: disabled (tinyxml2 was not found)\n'
fi

cmake -S "$ROOT_DIR/server" -B "$SERVER_BUILD_ROOT" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DWARGAME_VERSION="$VERSION" \
  -DWARGAME_SOURCE_DIGEST="$SOURCE_DIGEST"
cmake --build "$SERVER_BUILD_ROOT" --target wargame_server --parallel

release_write_identity "$OUTPUT_ROOT/release-identity.txt" "$VERSION" "$SOURCE_DIGEST" "$MANIFEST"
cp -- "$BUILD_ROOT/appindex" "$OUTPUT_ROOT/appindex"
cp -- "$BUILD_ROOT/server/wargame_server" "$OUTPUT_ROOT/wargame_server-root-qt"
cp -- "$SERVER_BUILD_ROOT/wargame_server" "$OUTPUT_ROOT/wargame_server-standalone-qt64"
cp -- "$ROOT_DIR/deploy/release-manifest.env" "$OUTPUT_ROOT/release-manifest.env"
rm -rf -- "$OUTPUT_ROOT/map"
mkdir -p -- "$OUTPUT_ROOT/map"
cp -- "$BUILD_ROOT/map/metadata.json" "$BUILD_ROOT/map/tilejson.json" "$OUTPUT_ROOT/map/"
while IFS= read -r -d '' map_zoom_dir; do
  map_zoom_name="${map_zoom_dir##*/}"
  [[ "$map_zoom_name" =~ ^[0-9]+$ ]] || continue
  cp -a -- "$map_zoom_dir" "$OUTPUT_ROOT/map/$map_zoom_name"
done < <(find "$BUILD_ROOT/map" -mindepth 1 -maxdepth 1 -type d -print0)
MAP_TILE_COUNT="$(find "$OUTPUT_ROOT/map" -type f -name '*.png' -print | wc -l)"
(( MAP_TILE_COUNT > 0 )) || {
  printf 'release GIS map contains no PNG tiles\n' >&2
  exit 1
}
chmod 755 "$OUTPUT_ROOT/appindex" "$OUTPUT_ROOT/wargame_server-root-qt" "$OUTPUT_ROOT/wargame_server-standalone-qt64"
(
  cd "$OUTPUT_ROOT"
  {
    printf '%s\0' \
      appindex \
      wargame_server-root-qt \
      wargame_server-standalone-qt64 \
      release-identity.txt \
      release-manifest.env
    find map -type f -print0 | LC_ALL=C sort -z
  } | xargs -0 sha256sum > SHA256SUMS
)

printf 'releaseVersion=%s\n' "$VERSION"
printf 'sourceDigest=%s\n' "$SOURCE_DIGEST"
printf 'output=%s\n' "$OUTPUT_ROOT"
