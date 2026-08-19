#!/usr/bin/env bash
set -Eeuo pipefail

# Run the design/EncoderDecoder binaries against the checked-in fixtures.  The
# output directory is always supplied by the caller (normally under build/),
# so this gate never writes generated artifacts into the source tree.

SOURCE_DIR="$(pwd)"
BUILD_DIR=""
OUT_DIR=""

usage() {
  cat <<'EOF'
Usage: tools/vmf-design-regression.sh [--source-dir DIR] --build-dir DIR --out-dir DIR
EOF
}

while (($#)); do
  case "$1" in
    --source-dir)
      [[ $# -ge 2 ]] || { echo "missing --source-dir value" >&2; exit 2; }
      SOURCE_DIR="$2"
      shift 2
      ;;
    --build-dir)
      [[ $# -ge 2 ]] || { echo "missing --build-dir value" >&2; exit 2; }
      BUILD_DIR="$2"
      shift 2
      ;;
    --out-dir)
      [[ $# -ge 2 ]] || { echo "missing --out-dir value" >&2; exit 2; }
      OUT_DIR="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

[[ -n "$BUILD_DIR" && -n "$OUT_DIR" ]] || {
  usage >&2
  exit 2
}

SOURCE_DIR="$(cd -- "$SOURCE_DIR" && pwd)"
BUILD_DIR="$(cd -- "$BUILD_DIR" && pwd)"
DESIGN_DIR="$SOURCE_DIR/design/EncoderDecoder"
OUT_DIR="$(mkdir -p -- "$OUT_DIR" && cd -- "$OUT_DIR" && pwd)"

for binary in vmf_validate vmf_encode vmf_decode xml_compare; do
  [[ -x "$BUILD_DIR/$binary" ]] || {
    echo "missing VMF design executable: $BUILD_DIR/$binary" >&2
    exit 1
  }
done

DICT="$DESIGN_DIR/msgStruct/msg0_1.xml"
CONTENT="$DESIGN_DIR/dic_content.xml"
MESSAGE="$DESIGN_DIR/msg_pass.xml"
NETWORK_BIN="$OUT_DIR/network_monitoring.bin"
NETWORK_XML="$OUT_DIR/network_monitoring_decoded.xml"

echo "==> Validate NetworkMonitoring fixture"
"$BUILD_DIR/vmf_validate" "$DICT" "$CONTENT" "$MESSAGE"
echo "==> Encode NetworkMonitoring fixture"
"$BUILD_DIR/vmf_encode" "$DICT" "$CONTENT" "$MESSAGE" "$NETWORK_BIN"
echo "==> Decode NetworkMonitoring fixture"
"$BUILD_DIR/vmf_decode" "$DICT" "$CONTENT" "$NETWORK_BIN" "$NETWORK_XML"
echo "==> Compare NetworkMonitoring fixture"
"$BUILD_DIR/xml_compare" "$MESSAGE" "$NETWORK_XML"

echo "==> Run Land Route text/XML/binary pipeline"
(
  cd -- "$DESIGN_DIR"
  bash tools/run_route_pipeline.sh \
    --desc "$DESIGN_DIR/路线描述.txt" \
    --msg-struct "$DESIGN_DIR/msgStruct/msg4_2.xml" \
    --content-dict "$CONTENT" \
    --build-dir "$BUILD_DIR" \
    --out-dir "$OUT_DIR/route"
)

echo "VMF design regression PASS"
