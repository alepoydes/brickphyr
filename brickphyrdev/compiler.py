# SPDX-License-Identifier: Apache-2.0
"""Compile Python source with the native ABI compiler exported by the SDK."""

import json
import os
import re
import subprocess
from pathlib import Path


def run(args):
    sdk = args.sdk or os.environ.get("LLEXT_EDK_INSTALL_DIR")
    if not sdk:
        raise ValueError("SDK directory required: pass --sdk or set LLEXT_EDK_INSTALL_DIR")

    python_dir = (Path(sdk) / "python").resolve()
    metadata_file = python_dir / "compiler.json"
    try:
        metadata = json.loads(metadata_file.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"Cannot read SDK compiler metadata {metadata_file}: {error}") from error

    if not isinstance(metadata, dict):
        raise ValueError(f"Invalid SDK compiler metadata {metadata_file}: expected an object")
    executable = metadata.get("executable")
    architecture = metadata.get("architecture")
    if (
        not isinstance(executable, str)
        or not executable
        or not isinstance(architecture, str)
        or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]*", architecture)
    ):
        raise ValueError(
            f"Invalid SDK compiler metadata {metadata_file}: executable and architecture required"
        )

    compiler = Path(executable)
    if compiler.is_absolute() or ".." in compiler.parts:
        raise ValueError(f"SDK compiler executable must be relative to {python_dir}")
    compiler = (python_dir / compiler).resolve()
    if not compiler.is_relative_to(python_dir) or not compiler.is_file():
        raise ValueError(f"SDK compiler executable is missing or outside {python_dir}: {compiler}")

    source = Path(args.source).resolve()
    output = Path(args.output) if args.output else Path(args.source).with_suffix(".mpy")
    command = [str(compiler), f"-march={architecture}", "-o", str(output.resolve()), str(source)]
    result = subprocess.run(command, check=False)
    if result.returncode:
        raise SystemExit(result.returncode)
