#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
if [ "$(uname -m)" != arm64 ]; then
  echo 'Build natively on Apple Silicon.' >&2
  exit 1
fi
if [ "${1:-}" = bridge ]; then
  shift
  exec /usr/bin/python3 tools/build_bridge.py "$@"
fi
exec /usr/bin/python3 tools/build_app.py "$@"
