#!/usr/bin/env bash

# Shared release-contract helpers. Keep this file dependency-free so the
# package builder and fresh-host installer calculate the same release identity.

release_project_version() {
  local source_root="$1"
  sed -nE 's/^project\([^)]* VERSION ([0-9][0-9A-Za-z._-]*).*/\1/p' \
    "$source_root/CMakeLists.txt" | head -n1
}

release_manifest_value() {
  local manifest="$1" wanted="$2"
  awk -F= -v wanted="$wanted" '$1 == wanted { print substr($0, index($0, "=") + 1); count++ } END { if (count != 1) exit 1 }' \
    "$manifest"
}

release_validate_manifest() {
  local manifest="$1" line key value expected actual
  [[ -f "$manifest" ]] || {
    printf 'missing release manifest: %s\n' "$manifest" >&2
    return 1
  }

  while IFS= read -r line || [[ -n "$line" ]]; do
    [[ -z "$line" || "$line" == \#* ]] && continue
    [[ "$line" == *=* ]] || {
      printf 'invalid release manifest line: %s\n' "$line" >&2
      return 1
    }
    key="${line%%=*}"
    value="${line#*=}"
    [[ "$key" =~ ^[A-Za-z][A-Za-z0-9]*$ && "$value" != *$'\r'* && "$value" != *$'\n'* ]] || {
      printf 'invalid release manifest field: %s\n' "$key" >&2
      return 1
    }
    case "$key" in
      releaseManifestVersion|protocolVersion|schemaVersion|clientArtifact|accountArtifact|serverArtifact|authoritativeDataPlane) ;;
      *)
        printf 'unknown release manifest field: %s\n' "$key" >&2
        return 1
        ;;
    esac
  done < "$manifest"

  for expected in \
    'releaseManifestVersion=1' \
    'protocolVersion=7' \
    'schemaVersion=7' \
    'clientArtifact=appindex' \
    'accountArtifact=account-web' \
    'serverArtifact=game-server' \
    'authoritativeDataPlane=websocket'; do
    key="${expected%%=*}"
    actual="$(release_manifest_value "$manifest" "$key" 2>/dev/null || true)"
    [[ "$actual" == "${expected#*=}" ]] || {
      printf 'release manifest contract mismatch: %s\n' "$expected" >&2
      return 1
    }
  done
}

release_compute_source_digest() {
  local source_root="$1"
  (
    cd -- "$source_root"
    local input
    local -a digest_inputs=()
    for input in CMakeLists.txt Main.qml main.cpp qml cmake src server deploy \
        'design/vmf设计.docx' design/EncoderDecoder/README.txt \
        design/EncoderDecoder/dic.xml design/EncoderDecoder/dic_content.xml \
        design/EncoderDecoder/message_catalog.json design/EncoderDecoder/msgStruct \
        map/metadata.json .dockerignore README.md docs; do
      [[ -e "$input" ]] && digest_inputs+=("$input")
    done
    find "${digest_inputs[@]}" \
      -type d \( -name __pycache__ -o -name .pytest_cache -o -name .omo \) -prune -o \
      -type f \( -name '*.pyc' -o -name '*.pyo' -o -name '*.pyd' -o -name '.env.example' -o -name 'release-identity.txt' \) -prune -o \
      -type f -print0 \
      | LC_ALL=C sort -z \
      | xargs -0 sha256sum \
      | sha256sum \
      | cut -c1-64
  )
}

release_write_identity() {
  local output="$1" version="$2" digest="$3" manifest="$4"
  [[ "$version" =~ ^[A-Za-z0-9._-]{1,32}$ ]] || return 1
  [[ "$digest" =~ ^[0-9a-f]{64}$ ]] || return 1
  release_validate_manifest "$manifest" || return 1
  {
    printf 'releaseIdentityVersion=1\n'
    printf 'wargameVersion=%s\n' "$version"
    printf 'sourceDigest=%s\n' "$digest"
    printf 'releaseId=%s-%s\n' "$version" "${digest:0:12}"
    printf 'protocolVersion=%s\n' "$(release_manifest_value "$manifest" protocolVersion)"
    printf 'schemaVersion=%s\n' "$(release_manifest_value "$manifest" schemaVersion)"
    printf 'clientArtifact=%s\n' "$(release_manifest_value "$manifest" clientArtifact)"
    printf 'accountArtifact=%s\n' "$(release_manifest_value "$manifest" accountArtifact)"
    printf 'serverArtifact=%s\n' "$(release_manifest_value "$manifest" serverArtifact)"
    printf 'authoritativeDataPlane=%s\n' "$(release_manifest_value "$manifest" authoritativeDataPlane)"
  } > "$output"
}

release_identity_value() {
  local identity="$1" wanted="$2"
  awk -F= -v wanted="$wanted" '$1 == wanted { print substr($0, index($0, "=") + 1); count++ } END { if (count != 1) exit 1 }' \
    "$identity"
}

release_validate_identity() {
  local identity="$1" manifest="$2" expected_version="$3" expected_digest="$4"
  local line key value expected actual
  [[ -f "$identity" ]] || {
    printf 'missing release identity: %s\n' "$identity" >&2
    return 1
  }
  release_validate_manifest "$manifest" || return 1

  while IFS= read -r line || [[ -n "$line" ]]; do
    [[ -z "$line" || "$line" == \#* ]] && continue
    [[ "$line" == *=* ]] || {
      printf 'invalid release identity line: %s\n' "$line" >&2
      return 1
    }
    key="${line%%=*}"
    value="${line#*=}"
    [[ "$key" =~ ^[A-Za-z][A-Za-z0-9]*$ && "$value" != *$'\r'* && "$value" != *$'\n'* ]] || {
      printf 'invalid release identity field: %s\n' "$key" >&2
      return 1
    }
    case "$key" in
      releaseIdentityVersion|wargameVersion|sourceDigest|releaseId|protocolVersion|schemaVersion|clientArtifact|accountArtifact|serverArtifact|authoritativeDataPlane) ;;
      *)
        printf 'unknown release identity field: %s\n' "$key" >&2
        return 1
        ;;
    esac
  done < "$identity"

  for expected in \
    'releaseIdentityVersion=1' \
    "wargameVersion=$expected_version" \
    "sourceDigest=$expected_digest" \
    "releaseId=$expected_version-${expected_digest:0:12}" \
    'protocolVersion=7' \
    'schemaVersion=7' \
    'clientArtifact=appindex' \
    'accountArtifact=account-web' \
    'serverArtifact=game-server' \
    'authoritativeDataPlane=websocket'; do
    key="${expected%%=*}"
    actual="$(release_identity_value "$identity" "$key" 2>/dev/null || true)"
    [[ "$actual" == "${expected#*=}" ]] || {
      printf 'release identity mismatch: %s\n' "$expected" >&2
      return 1
    }
  done
}
