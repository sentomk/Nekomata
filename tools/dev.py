#!/usr/bin/env python3
"""Run Nekomata's pinned build and formatting tools."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import subprocess
import sys
import venv
from typing import NoReturn


repository_root = Path(__file__).resolve().parents[1]
requirements_path = repository_root / "tools" / "requirements.txt"
tool_root = repository_root / ".tools"
environment_path = tool_root / "venv"
stamp_path = tool_root / "requirements.sha256"


def fail(message: str) -> NoReturn:
    print(message, file=sys.stderr)
    raise SystemExit(2)


def read_pins() -> dict[str, str]:
    pins: dict[str, str] = {}
    for raw_line in requirements_path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split("==")
        if len(parts) != 2 or not all(parts):
            fail(f"build-tool requirement must be an exact pin: {line}")
        pins[parts[0].lower().replace("_", "-")] = parts[1]
    required = {"cmake", "ninja", "clang-format"}
    if set(pins) != required:
        names = ", ".join(sorted(required))
        fail(f"build-tool requirements must pin exactly: {names}")
    return pins


def scripts_path() -> Path:
    return environment_path / ("Scripts" if os.name == "nt" else "bin")


def executable(name: str) -> Path:
    suffix = ".exe" if os.name == "nt" else ""
    return scripts_path() / f"{name}{suffix}"


def tool_paths() -> dict[str, Path]:
    return {
        "cmake": executable("cmake"),
        "ctest": executable("ctest"),
        "ninja": executable("ninja"),
        "clang-format": executable("clang-format"),
    }


def environment_python() -> Path:
    return executable("python")


def requirements_digest() -> str:
    return hashlib.sha256(requirements_path.read_bytes()).hexdigest()


def capture_first_line(command: list[str]) -> str:
    result = subprocess.run(
        command,
        cwd=repository_root,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    return result.stdout.splitlines()[0]


def verify_tools(paths: dict[str, Path], pins: dict[str, str]) -> list[str]:
    versions = [
        capture_first_line([str(paths["cmake"]), "--version"]),
        capture_first_line([str(paths["ninja"]), "--version"]),
        capture_first_line([str(paths["clang-format"]), "--version"]),
    ]
    ctest_version = capture_first_line([str(paths["ctest"]), "--version"])
    expected = {
        "cmake": f"cmake version {pins['cmake']}",
        "ninja": pins["ninja"],
        "clang-format": f"version {pins['clang-format']}",
    }
    if versions[0] != expected["cmake"]:
        fail(f"expected {expected['cmake']}, got: {versions[0]}")
    if ctest_version != f"ctest version {pins['cmake']}":
        fail(f"expected CTest {pins['cmake']}, got: {ctest_version}")
    if versions[1] != expected["ninja"] and not versions[1].startswith(f"{expected['ninja']}."):
        fail(f"expected Ninja {expected['ninja']}, got: {versions[1]}")
    if versions[2] != f"clang-format {expected['clang-format']}":
        fail(f"expected clang-format {pins['clang-format']}, got: {versions[2]}")
    return versions


def bootstrap(verbose: bool = False) -> dict[str, Path]:
    pins = read_pins()
    paths = tool_paths()
    digest = requirements_digest()
    installed = (
        stamp_path.is_file()
        and stamp_path.read_text(encoding="utf-8").strip() == digest
        and all(path.is_file() for path in paths.values())
    )

    if not installed:
        print("Installing pinned build tools into .tools/venv ...", flush=True)
        tool_root.mkdir(parents=True, exist_ok=True)
        if not environment_python().is_file():
            venv.EnvBuilder(with_pip=True, clear=True).create(environment_path)
        subprocess.run(
            [
                str(environment_python()),
                "-m",
                "pip",
                "install",
                "--disable-pip-version-check",
                "--no-input",
                "--no-deps",
                "--upgrade",
                "--requirement",
                str(requirements_path),
            ],
            cwd=repository_root,
            check=True,
        )
        versions = verify_tools(paths, pins)
        stamp_path.write_text(f"{digest}\n", encoding="utf-8")
    else:
        versions = verify_tools(paths, pins)

    if verbose:
        for version in versions:
            print(version)
    return paths


def child_environment() -> dict[str, str]:
    child = os.environ.copy()
    child["PATH"] = f"{scripts_path()}{os.pathsep}{child.get('PATH', '')}"
    return child


def run(command: list[str]) -> int:
    return subprocess.run(
        command,
        cwd=repository_root,
        env=child_environment(),
        check=False,
    ).returncode


def split_preset(arguments: list[str]) -> tuple[str, list[str]]:
    arguments = list(arguments)
    preset = "debug"
    if arguments and arguments[0] != "--":
        preset = arguments.pop(0)
    if arguments[:1] == ["--"]:
        arguments.pop(0)
    return preset, arguments


def source_files() -> list[str]:
    pathspecs = [
        "*.c",
        "*.cc",
        "*.cpp",
        "*.cxx",
        "*.h",
        "*.hh",
        "*.hpp",
        "*.hxx",
        "*.h.in",
        "*.hpp.in",
        ":(exclude)tests/vendor/**",
        ":(exclude)third_party/**",
        ":(exclude)vendor/**",
    ]
    result = subprocess.run(
        [
            "git",
            "-c",
            f"safe.directory={repository_root.as_posix()}",
            "ls-files",
            "--cached",
            "--others",
            "--exclude-standard",
            "-z",
            "--",
            *pathspecs,
        ],
        cwd=repository_root,
        check=True,
        stdout=subprocess.PIPE,
    )
    return [os.fsdecode(path) for path in result.stdout.split(b"\0") if path]


def format_sources(formatter: Path, arguments: list[str]) -> int:
    mode = arguments or ["--check"]
    if mode == ["--check"]:
        formatter_arguments = ["--dry-run", "--Werror"]
    elif mode == ["--fix"]:
        formatter_arguments = ["-i"]
    else:
        fail("usage: tools/dev.py format [--check|--fix]")

    files = source_files()
    if not files:
        print("No C/C++ files to format.")
        return 0
    print(f"{capture_first_line([str(formatter), '--version'])}; {len(files)} C/C++ files")
    return run([str(formatter), *formatter_arguments, *files])


def run_action(action: str, paths: dict[str, Path], arguments: list[str]) -> int:
    preset, forwarded = split_preset(arguments)
    if action == "configure":
        ninja = paths["ninja"].resolve().as_posix()
        return run(
            [
                str(paths["cmake"]),
                "--preset",
                preset,
                f"-DCMAKE_MAKE_PROGRAM:FILEPATH={ninja}",
                *forwarded,
            ]
        )
    if action == "build":
        return run([str(paths["cmake"]), "--build", "--preset", preset, *forwarded])
    if action == "test":
        return run([str(paths["ctest"]), "--preset", preset, *forwarded])
    fail(f"unknown action: {action}")


def usage() -> None:
    print(
        "usage: tools/dev.py <bootstrap|versions|configure|build|test|format|check> [arguments]\n"
        "  configure [preset] [-- cmake arguments]\n"
        "  build     [preset] [-- build arguments]\n"
        "  test      [preset] [-- ctest arguments]\n"
        "  format    [--check|--fix]\n"
        "  check     [preset]",
        file=sys.stderr,
    )


def main() -> int:
    if len(sys.argv) < 2 or sys.argv[1] in {"-h", "--help"}:
        usage()
        return 0 if len(sys.argv) >= 2 else 2

    action = sys.argv[1]
    arguments = sys.argv[2:]
    if action not in {"bootstrap", "versions", "configure", "build", "test", "format", "check"}:
        usage()
        return 2

    paths = bootstrap(verbose=action in {"bootstrap", "versions"})
    if action in {"bootstrap", "versions"}:
        return 0
    if action == "format":
        return format_sources(paths["clang-format"], arguments)
    if action in {"configure", "build", "test"}:
        return run_action(action, paths, arguments)

    preset, forwarded = split_preset(arguments)
    if forwarded:
        fail("usage: tools/dev.py check [preset]")
    for step in ("configure", "build", "test"):
        result = run_action(step, paths, [preset])
        if result != 0:
            return result
    return format_sources(paths["clang-format"], ["--check"])


if __name__ == "__main__":
    raise SystemExit(main())
