#!/usr/bin/env bash
set -euo pipefail

DESC="路线描述.txt"
MSG_STRUCT="msgStruct/msg4_2.xml"
CONTENT_DICT="dic_content.xml"
BUILD_DIR="build"
OUT_DIR="route_pipeline_out"

usage() {
  cat <<'EOF'
Usage: bash tools/run_route_pipeline.sh [options]

Run the full Land Route sample pipeline:
  route_desc_to_msg.py -> vmf_validate -> vmf_encode -> vmf_decode ->
  vmf_validate -> xml_compare -> route_msg_to_desc.py

Options:
  --desc PATH          Route description text input (default: 路线描述.txt)
  --msg-struct PATH    Message structure XML (default: msgStruct/msg4_2.xml)
  --content-dict PATH  Content dictionary XML (default: dic_content.xml)
  --build-dir PATH     Directory containing vmf_* binaries (default: build)
  --out-dir PATH       Output directory (default: route_pipeline_out)
  -h, --help           Show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --desc)
      DESC="$2"
      shift 2
      ;;
    --msg-struct)
      MSG_STRUCT="$2"
      shift 2
      ;;
    --content-dict)
      CONTENT_DICT="$2"
      shift 2
      ;;
    --build-dir)
      BUILD_DIR="$2"
      shift 2
      ;;
    --out-dir)
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

require_file() {
  local path="$1"
  if [[ ! -f "$path" ]]; then
    echo "missing file: $path" >&2
    exit 1
  fi
}

require_exe() {
  local path="$1"
  if [[ ! -x "$path" ]]; then
    echo "missing executable: $path" >&2
    exit 1
  fi
}

run_step() {
  local name="$1"
  shift
  echo
  echo "==> $name"
  printf '+'
  printf ' %q' "$@"
  echo
  "$@"
}

require_file "$DESC"
require_file "$MSG_STRUCT"
require_file "$CONTENT_DICT"
require_file "tools/route_desc_to_msg.py"
require_file "tools/route_msg_to_desc.py"
require_exe "$BUILD_DIR/vmf_validate"
require_exe "$BUILD_DIR/vmf_encode"
require_exe "$BUILD_DIR/vmf_decode"
require_exe "$BUILD_DIR/xml_compare"

mkdir -p "$OUT_DIR"

ROUTE_XML="$OUT_DIR/route_message.xml"
ROUTE_BIN="$OUT_DIR/route_message.bin"
DECODED_XML="$OUT_DIR/decoded.xml"
DESC_OUT="$OUT_DIR/路线描述_out.txt"

run_step "Generate route MessageContent XML" \
  python3 tools/route_desc_to_msg.py "$DESC" -o "$ROUTE_XML"

run_step "Validate generated XML" \
  "$BUILD_DIR/vmf_validate" "$MSG_STRUCT" "$CONTENT_DICT" "$ROUTE_XML"

run_step "Encode generated XML" \
  "$BUILD_DIR/vmf_encode" "$MSG_STRUCT" "$CONTENT_DICT" "$ROUTE_XML" "$ROUTE_BIN"

run_step "Decode binary" \
  "$BUILD_DIR/vmf_decode" "$MSG_STRUCT" "$CONTENT_DICT" "$ROUTE_BIN" "$DECODED_XML"

run_step "Validate decoded XML" \
  "$BUILD_DIR/vmf_validate" "$MSG_STRUCT" "$CONTENT_DICT" "$DECODED_XML"

run_step "Compare source XML with decoded XML" \
  "$BUILD_DIR/xml_compare" "$ROUTE_XML" "$DECODED_XML"

run_step "Convert decoded XML back to route text" \
  python3 tools/route_msg_to_desc.py "$DECODED_XML" -o "$DESC_OUT"

echo
echo "Pipeline PASS"
echo "Outputs:"
echo "  XML:        $ROUTE_XML"
echo "  Binary:     $ROUTE_BIN"
echo "  Decoded:    $DECODED_XML"
echo "  Text:       $DESC_OUT"
