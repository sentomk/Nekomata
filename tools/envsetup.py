#!/usr/bin/env python3
"""Install and verify Nekomata's pinned development environment."""

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


def setup_environment(verbose: bool = False) -> None:
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


def usage() -> None:
    print("usage: tools/envsetup.py [--versions]", file=sys.stderr)


def main() -> int:
    arguments = sys.argv[1:]
    if arguments in [["-h"], ["--help"]]:
        usage()
        return 0
    if arguments not in [[], ["--versions"]]:
        usage()
        return 2

    setup_environment(verbose=arguments == ["--versions"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
