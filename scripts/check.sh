#!/usr/bin/env bash
set -euo pipefail

nekomata_script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
nekomata_preset="${1:-debug}"
if [[ $# -gt 0 ]]; then
  shift
fi
if [[ $# -gt 0 ]]; then
  echo "Usage: bash scripts/check.sh [preset]" >&2
  exit 2
fi

bash "$nekomata_script_dir/configure.sh" "$nekomata_preset"
bash "$nekomata_script_dir/build.sh" "$nekomata_preset"
bash "$nekomata_script_dir/test.sh" "$nekomata_preset"
bash "$nekomata_script_dir/format.sh" --check
