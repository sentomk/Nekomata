#!/usr/bin/env python3
"""Compiles one behavior generation and publishes it atomically.

The artifact lands at an immutable sequence-named path below offers/modules/
and the mutable manifest at offers/latest is replaced with os.rename, so a
page never observes a half-published generation. The compile flags below are
the generation's build information; changing them changes demo semantics.
"""

import argparse
import hashlib
import os
import pathlib
import re
import shutil
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
PUBLIC = HERE / "public"
OFFERS = PUBLIC / "offers"
MODULES = OFFERS / "modules"

FLAGS = [
    "-std=c++20", "-O0", "-g", "-Wall", "-Wextra", "-Werror",
    "-fno-exceptions", "-fno-rtti", "-sASSERTIONS=2", "-sSIDE_MODULE=1",
    f"-I{ROOT / 'src'}",
]


def next_sequence() -> int:
    best = 0
    if MODULES.exists():
        for name in MODULES.iterdir():
            match = re.fullmatch(r"gen-(\d+)-[0-9a-f]+\.wasm", name.name)
            if match:
                best = max(best, int(match.group(1)))
    return best + 1


def sha256_of(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("behavior", type=int, choices=(1, 2, 3),
                        help="1=linear bounce, 2=gravity, 3=center spring")
    args = parser.parse_args()

    emxx = os.environ.get("NEKOMATA_EMXX") or shutil.which("em++")
    if not emxx:
        sys.exit("em++ not found; install Emscripten or set NEKOMATA_EMXX")

    MODULES.mkdir(parents=True, exist_ok=True)
    sequence = next_sequence()
    generation = f"gen-{sequence}-{args.behavior}"
    artifact = MODULES / f"{generation}.wasm"

    subprocess.run(
        [emxx, *FLAGS, f"-DBEHAVIOR={args.behavior}", str(HERE / "hot.cpp"), "-o", str(artifact)],
        check=True)

    manifest = (
        "nekomata-wasm/1\n"
        f'group_id "demo-ball"\n'
        f"sequence {sequence}\n"
        f'generation_id "{generation}"\n'
        f'abi_id "demo-ball-v1"\n'
        f'artifact "modules/{generation}.wasm" "{sha256_of(artifact)}"\n'
        'entry "identify"\n'
        'entry "update_world"\n')

    staging = OFFERS / "latest.staging"
    staging.write_text(manifest)
    os.replace(staging, OFFERS / "latest")
    print(f"published {generation}: behavior {args.behavior}, sequence {sequence}")


if __name__ == "__main__":
    main()
