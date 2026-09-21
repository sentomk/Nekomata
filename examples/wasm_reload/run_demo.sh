#!/usr/bin/env bash
# One-command demo entrypoint: builds the main module if needed, then
# serves the page. Publish generations from another terminal with
# python3 publish.py <1|2|3>.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ ! -f "$here/public/main.js" || "$here/main.cpp" -nt "$here/public/main.js" ]]; then
  bash "$here/build_main.sh"
fi
exec python3 "$here/serve.py" "$@"
