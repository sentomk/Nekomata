#!/usr/bin/env bash
set -euo pipefail

source "$(dirname "${BASH_SOURCE[0]}")/_toolchain.sh"

case "${1:---check}" in
  --check) nekomata_format_arguments=(--dry-run --Werror) ;;
  --fix) nekomata_format_arguments=(-i) ;;
  *) echo "Usage: bash scripts/format.sh [--check|--fix]" >&2; exit 2 ;;
esac

cd "$nekomata_repository_root"

# Include new, non-ignored files as well as tracked files. NUL delimiters
# preserve filenames containing spaces. Build outputs and vendor code stay out.
nekomata_source_files=()
while IFS= read -r -d '' nekomata_source_file; do
  [[ -f "$nekomata_source_file" ]] && nekomata_source_files+=("$nekomata_source_file")
done < <(git -c "safe.directory=$nekomata_repository_root" \
  ls-files --cached --others --exclude-standard -z -- \
  '*.c' '*.cc' '*.cpp' '*.cxx' '*.h' '*.hh' '*.hpp' '*.hxx' \
  '*.h.in' '*.hpp.in' \
  ':(exclude)tests/support/doctest/**' ':(exclude)third_party/**' ':(exclude)vendor/**')

if [[ ${#nekomata_source_files[@]} -eq 0 ]]; then
  echo "No C/C++ files to format."
  exit 0
fi

nekomata_formatter_version=$("$nekomata_clang_format" --version)
echo "$nekomata_formatter_version; ${#nekomata_source_files[@]} C/C++ files"
exec "$nekomata_clang_format" "${nekomata_format_arguments[@]}" \
  "${nekomata_source_files[@]}"
