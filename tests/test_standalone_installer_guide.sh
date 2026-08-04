#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
GUIDE="$ROOT_DIR/deploy/QUICK_START.md"

test -f "$GUIDE"
grep -Eiq 'external.*(hosting|release)|release.*(hosting|external)' "$GUIDE"
grep -Fq 'sha256sum -c' "$GUIDE"
grep -Fq 'tar -xzf' "$GUIDE"
grep -Fq 'deploy/install-server.sh' "$GUIDE"
grep -Fq -- '--install-dir' "$GUIDE"
grep -Fq -- '--no-build' "$GUIDE"
grep -Fq '/api/health' "$GUIDE"
grep -Fq 'deploy/reset-room.sh' "$GUIDE"
grep -Fq 'deploy/reset-admin.sh' "$GUIDE"
grep -Fq 'deploy/uninstall-server.sh' "$GUIDE"
grep -Fq 'account-web' "$GUIDE"
grep -Fq 'game-server' "$GUIDE"
grep -Fqi 'persistent' "$GUIDE"
grep -Eiq 'data|volume' "$GUIDE"
! grep -Eiq 'git[[:space:]]+clone|password123|replace-with|strong-pass' "$GUIDE"
