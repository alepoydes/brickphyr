# SPDX-License-Identifier: Apache-2.0
"""Exercise the SDK compiler through the installed CLI argument path."""

import json
import sys

import pytest

from brickphyrdev.cli import main


def test_compile_uses_sdk_metadata_and_preserves_compiler_error(tmp_path, monkeypatch, capfd):
    sdk = tmp_path / "sdk"
    python_dir = sdk / "python"
    python_dir.mkdir(parents=True)
    compiler = python_dir / "mpy-cross"
    compiler.write_text(
        f"#!{sys.executable}\n"
        "import os, pathlib, sys\n"
        "if os.environ.get('FAKE_COMPILER_FAIL'):\n"
        "    print('compiler syntax error', file=sys.stderr)\n"
        "    sys.exit(7)\n"
        "pathlib.Path(sys.argv[sys.argv.index('-o') + 1]).write_text(\n"
        "    '|'.join(sys.argv[1:]))\n"
    )
    compiler.chmod(0o755)
    (python_dir / "compiler.json").write_text(
        json.dumps({"executable": "mpy-cross", "architecture": "armv7emsp"})
    )
    source = tmp_path / "program.py"
    source.write_text("pass\n")
    monkeypatch.setenv("LLEXT_EDK_INSTALL_DIR", str(sdk))
    monkeypatch.setattr(sys, "argv", ["brickphyrdev", "compile", str(source)])
    main()
    output = source.with_suffix(".mpy")
    assert output.read_text() == (f"-march=armv7emsp|-o|{output}|{source}")

    custom = tmp_path / "custom.mpy"
    monkeypatch.setattr(
        sys,
        "argv",
        ["brickphyrdev", "compile", str(source), "--output", str(custom), "--sdk", str(sdk)],
    )
    main()
    assert custom.read_text().startswith("-march=armv7emsp|-o|")

    monkeypatch.setenv("FAKE_COMPILER_FAIL", "1")
    with pytest.raises(SystemExit) as error:
        main()
    assert error.value.code == 7
    assert "compiler syntax error" in capfd.readouterr().err
    assert custom.read_text().startswith("-march=armv7emsp|-o|")


def test_compile_rejects_missing_sdk_and_escaping_compiler(tmp_path, monkeypatch, capsys):
    source = tmp_path / "program.py"
    source.write_text("pass\n")
    monkeypatch.delenv("LLEXT_EDK_INSTALL_DIR", raising=False)
    monkeypatch.setattr(sys, "argv", ["brickphyrdev", "compile", str(source)])
    with pytest.raises(SystemExit) as error:
        main()
    assert error.value.code == 1
    assert "--sdk or set LLEXT_EDK_INSTALL_DIR" in capsys.readouterr().err

    python_dir = tmp_path / "sdk/python"
    python_dir.mkdir(parents=True)
    (python_dir / "compiler.json").write_text(
        json.dumps({"executable": "../mpy-cross", "architecture": "armv7emsp"})
    )
    monkeypatch.setattr(
        sys, "argv", ["brickphyrdev", "compile", str(source), "--sdk", str(tmp_path / "sdk")]
    )
    with pytest.raises(SystemExit) as error:
        main()
    assert error.value.code == 1
    assert "relative to" in capsys.readouterr().err
