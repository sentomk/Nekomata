#!/usr/bin/env python3

from __future__ import annotations

from contextlib import ExitStack, redirect_stderr
import importlib.util
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


def load_envsetup(path: Path):
    spec = importlib.util.spec_from_file_location("nekomata_envsetup", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


envsetup = load_envsetup(Path(sys.argv[1]))


class envsetup_tests(unittest.TestCase):
    def test_incomplete_environment_is_recreated(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            environment = root / "venv"
            stamp = root / "requirements.sha256"

            with ExitStack() as stack:
                stack.enter_context(mock.patch.object(envsetup, "tool_root", root))
                stack.enter_context(mock.patch.object(envsetup, "environment_path", environment))
                stack.enter_context(mock.patch.object(envsetup, "stamp_path", stamp))
                stack.enter_context(
                    mock.patch.object(
                        envsetup,
                        "read_pins",
                        return_value={
                            "cmake": "1",
                            "ninja": "1",
                            "clang-format": "1",
                        },
                    )
                )
                stack.enter_context(mock.patch.object(envsetup, "tool_paths", return_value={}))
                stack.enter_context(
                    mock.patch.object(envsetup, "requirements_digest", return_value="digest")
                )
                stack.enter_context(mock.patch.object(envsetup, "verify_tools", return_value=[]))
                stack.enter_context(
                    mock.patch.object(envsetup, "pip_available", side_effect=[False, True])
                )
                builder_type = stack.enter_context(mock.patch.object(envsetup.venv, "EnvBuilder"))
                run = stack.enter_context(mock.patch.object(envsetup.subprocess, "run"))
                expected_python = envsetup.environment_python()
                expected_python.parent.mkdir(parents=True)
                expected_python.touch()

                envsetup.setup_environment()

            builder_type.assert_called_once_with(with_pip=True, clear=True)
            builder_type.return_value.create.assert_called_once_with(environment)
            command = run.call_args.args[0]
            self.assertEqual(
                command[:4],
                [str(expected_python), "-m", "pip", "install"],
            )
            self.assertEqual(stamp.read_text(encoding="utf-8"), "digest\n")

    def test_missing_venv_support_has_actionable_error(self) -> None:
        error = subprocess.CalledProcessError(1, ["python", "-m", "venv"])
        stderr = io.StringIO()
        with ExitStack() as stack:
            builder_type = stack.enter_context(mock.patch.object(envsetup.venv, "EnvBuilder"))
            stack.enter_context(redirect_stderr(stderr))
            with self.assertRaises(SystemExit) as raised:
                builder_type.return_value.create.side_effect = error
                envsetup.create_environment()

        self.assertEqual(raised.exception.code, 2)
        self.assertIn("python3-venv", stderr.getvalue())

    def test_created_environment_without_pip_is_rejected(self) -> None:
        stderr = io.StringIO()
        with ExitStack() as stack:
            stack.enter_context(mock.patch.object(envsetup.venv, "EnvBuilder"))
            stack.enter_context(mock.patch.object(envsetup, "pip_available", return_value=False))
            stack.enter_context(redirect_stderr(stderr))
            with self.assertRaises(SystemExit) as raised:
                envsetup.create_environment()

        self.assertEqual(raised.exception.code, 2)
        self.assertIn("created .tools/venv without pip", stderr.getvalue())


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
