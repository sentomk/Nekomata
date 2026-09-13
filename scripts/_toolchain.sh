# Shared pinned-tool discovery for repository scripts. Source this file.

nekomata_repository_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

if command -v python3 >/dev/null 2>&1; then
  nekomata_python=python3
elif command -v python >/dev/null 2>&1; then
  nekomata_python=python
else
  echo "Python 3.8 or newer is required to set up build tools." >&2
  return 1
fi

"$nekomata_python" "$nekomata_repository_root/tools/envsetup.py"

if [[ -d "$nekomata_repository_root/.tools/venv/Scripts" ]]; then
  nekomata_tool_bin="$nekomata_repository_root/.tools/venv/Scripts"
  nekomata_executable_suffix=.exe
else
  nekomata_tool_bin="$nekomata_repository_root/.tools/venv/bin"
  nekomata_executable_suffix=
fi

export PATH="$nekomata_tool_bin:$PATH"
nekomata_cmake="$nekomata_tool_bin/cmake$nekomata_executable_suffix"
nekomata_ctest="$nekomata_tool_bin/ctest$nekomata_executable_suffix"
nekomata_ninja="$nekomata_tool_bin/ninja$nekomata_executable_suffix"
nekomata_clang_format="$nekomata_tool_bin/clang-format$nekomata_executable_suffix"
