#!/usr/bin/env bash
set -euo pipefail

repository_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
if command -v python3 >/dev/null 2>&1; then
  exec python3 "$repository_root/tools/dev.py" format "$@"
fi
exec python "$repository_root/tools/dev.py" format "$@"
