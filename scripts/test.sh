#!/usr/bin/env bash
set -euo pipefail

source "$(dirname "${BASH_SOURCE[0]}")/_toolchain.sh"

nekomata_preset="${1:-debug}"
if [[ $# -gt 0 ]]; then
  shift
fi

cd "$nekomata_repository_root"
exec "$nekomata_ctest" --preset "$nekomata_preset" "$@"
