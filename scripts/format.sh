#!/usr/bin/env bash
set -euo pipefail

case "${1:---check}" in
  --check) args=(--dry-run --Werror) ;;
  --fix) args=(-i) ;;
  *) echo "Usage: bash scripts/format.sh [--check|--fix]" >&2; exit 2 ;;
esac

cd "$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)"

formatter="${CLANG_FORMAT:-clang-format-18}"
if [[ -z "${CLANG_FORMAT:-}" ]] && ! command -v "$formatter" >/dev/null; then
  formatter=clang-format
fi
version=$("$formatter" --version)
if [[ ! "$version" =~ version[[:space:]]18\. ]]; then
  echo "Expected clang-format 18, got: $version" >&2
  exit 1
fi

# Include new, non-ignored files as well as tracked files. NUL delimiters
# preserve filenames containing spaces. Build outputs and vendor code stay out.
files=()
while IFS= read -r -d '' file; do
  [[ -f "$file" ]] && files+=("$file")
done < <(git ls-files --cached --others --exclude-standard -z -- \
  '*.c' '*.cc' '*.cpp' '*.cxx' '*.h' '*.hh' '*.hpp' '*.hxx' \
  '*.h.in' '*.hpp.in' \
  ':(exclude)tests/vendor/**' ':(exclude)third_party/**' ':(exclude)vendor/**')

if [[ ${#files[@]} -eq 0 ]]; then
  echo "No C/C++ files to format."
  exit 0
fi

echo "$version; ${#files[@]} C/C++ files"
"$formatter" "${args[@]}" "${files[@]}"
