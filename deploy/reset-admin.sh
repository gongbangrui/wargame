#!/usr/bin/env sh
set -eu

cd "$(dirname "$0")/.."
if [ "$#" -gt 0 ]; then
    docker compose -f deploy/compose.yml exec account-web /usr/bin/python3 /app/reset_admin.py "$1"
else
    docker compose -f deploy/compose.yml exec account-web /usr/bin/python3 /app/reset_admin.py
fi
