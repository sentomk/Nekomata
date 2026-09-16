#!/usr/bin/env bash
set -euo pipefail

# The mock runner consumes publication events, not machine code. Accept both
# rebuild_hot.sh <source> <output> and compiler -c <source> -o <output>.
if [ "$#" -eq 2 ]; then
  cp "$1" "$2"
  exit 0
fi
source_file=""; output_file=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    -c) source_file="$2"; shift ;;
    -o) output_file="$2"; shift ;;
  esac
  shift
done
cp "$source_file" "$output_file"
