# SPDX-License-Identifier: Apache-2.0
"""Package boundaries and ABI admission through the host builder/parser."""

import json
import struct
from argparse import Namespace

import cbor2
import pytest

from brickphyrdev.package import build, run, validate, validate_native


def native_elf(*, major=2, minor=8, magic=0x31415042):
    names = b"\0.shstrtab\0.brickphyr.abi\0"
    identity = struct.pack("<IHHIII", magic, major, minor, 1, 0, 2048)
    sections_offset = 52 + len(names) + len(identity)
    sections_offset += -sections_offset % 4
    header = struct.pack(
        "<16sHHIIIIIHHHHHH", b"\x7fELF\x01\x01\x01".ljust(16, b"\0"),
        1, 40, 1, 0, 0, sections_offset, 0x05000400, 52, 0, 0, 40, 3, 1,
    )
    def section(*values):
        return struct.pack("<IIIIIIIIII", *values)
    return (header + names + identity).ljust(sections_offset, b"\0") + b"".join((
        section(*([0] * 10)),
        section(1, 3, 0, 0, 52, len(names), 0, 0, 1, 0),
        section(11, 1, 0, 0, 52 + len(names), len(identity), 0, 0, 4, 0),
    ))


def sdk(tmp_path):
    (tmp_path / "include").mkdir(parents=True)
    (tmp_path / "include/brickphyr_abi.h").write_text(
        "#define BP_ABI_MAJOR 2\n#define BP_ABI_MINOR 8\n#define BP_ABI_ARCH 1U\n"
    )
    (tmp_path / "include/brickphyr_api.h").write_text("/* generated */\n")
    (tmp_path / "build.json").write_text(json.dumps({
        "state": {"config_sha256": "a" * 64, "sources": {}},
        "artifacts": {"zephyr.elf": "b" * 64},
    }))
    return tmp_path


def test_composite_package_and_older_native_minor(tmp_path):
    entry = tmp_path / "main.py"
    controller = tmp_path / "control.elf"
    resource = tmp_path / "model.bin"
    entry.write_text("print('hello')\n")
    controller.write_bytes(native_elf(minor=7))
    resource.write_bytes(b"model")

    image = build(entry, components=[controller], resources={"data/model.bin": resource},
                  sdk=sdk(tmp_path / "sdk"))
    info = validate(image, abi="bp2.8-armv7em-hf")
    assert (info.engine, info.entry, info.components) == (1, "main.py", ("control.elf",))
    assert info.files["data/model.bin"] == b"model"
    assert cbor2.loads(info.files[""]) == [1, 1, "main.py", ["control.elf"]]
    assert build(entry, components=[controller], resources={"data/model.bin": resource},
                 sdk=tmp_path / "sdk") == image


def test_native_abi_requires_compatible_major_minor_and_identity():
    assert validate_native(native_elf(minor=7), "bp2.8-armv7em-hf").minor == 7
    with pytest.raises(ValueError, match="incompatible"):
        validate_native(native_elf(minor=9), "bp2.8-armv7em-hf")
    with pytest.raises(ValueError, match="incompatible"):
        validate_native(native_elf(major=3), "bp2.8-armv7em-hf")
    with pytest.raises(ValueError, match="identity"):
        validate_native(native_elf(magic=0))


def test_legacy_entry_forms_are_accepted():
    assert validate(cbor2.dumps({"main.py": b"pass"})).entry == "main.py"
    assert validate(cbor2.dumps({"": b"run.mpy", "run.mpy": b"M\0\0\0"})).engine == 2


@pytest.mark.parametrize("path", ["/absolute", "", "a//b", "a/./b", "a/../b", "a\0b",
                                       "x" * 129])
def test_invalid_package_paths(path):
    image = cbor2.dumps({"": cbor2.dumps([1, 1, "main.py", []]), "main.py": b"pass",
                         path: b"resource"})
    with pytest.raises(ValueError):
        validate(image)


def test_duplicate_and_parent_file_collision():
    # Duplicate CBOR keys must be rejected before a dict can drop one value.
    duplicate = b"\xa2" + cbor2.dumps("main.py") + cbor2.dumps(b"one") + \
        cbor2.dumps("main.py") + cbor2.dumps(b"two")
    with pytest.raises(ValueError, match="CBOR"):
        validate(duplicate)
    with pytest.raises(ValueError, match="collision"):
        validate(cbor2.dumps({"main.py": b"pass", "data": b"a", "data/file": b"b"}))


def test_manifest_requires_declared_component_and_file_limit():
    manifest = cbor2.dumps([1, 1, "main.py", ["missing.elf"]])
    with pytest.raises(ValueError, match="component"):
        validate(cbor2.dumps({"": manifest, "main.py": b"pass"}))
    many = {f"file{i}": b"" for i in range(65)}
    many["main.py"] = b"pass"
    with pytest.raises(ValueError, match="too many"):
        validate(cbor2.dumps(many))


def test_build_needs_sdk_for_elf_and_rejects_stale_sdk(tmp_path):
    entry = tmp_path / "main.elf"
    entry.write_bytes(native_elf())
    with pytest.raises(ValueError, match="SDK"):
        build(entry)
    with pytest.raises(ValueError, match="SDK"):
        build(entry, sdk=tmp_path / "missing")


def test_pack_command_writes_one_explicit_output(tmp_path):
    entry = tmp_path / "main.py"
    resource = tmp_path / "notes.txt"
    output = tmp_path / "program.cbor"
    entry.write_bytes(b"pass\n")
    resource.write_bytes(b"notes")
    result = run(Namespace(entry=entry, component=[], resource=[f"data/notes={resource}"],
                           sdk=None, output=output))
    assert result == output
    assert validate(output.read_bytes()).files["data/notes"] == b"notes"


def test_python_payloads_checked_before_install():
    with pytest.raises(ValueError, match="NUL"):
        validate(cbor2.dumps({"main.py": b"pass\0"}))
    with pytest.raises(ValueError, match="MPY"):
        validate(cbor2.dumps({"main.mpy": b"bad"}))
    # A native package treats Python-named resources as ordinary files.
    native = native_elf()
    image = cbor2.dumps({"": cbor2.dumps([1, 0, "main.elf", []]),
                         "main.elf": native, "data.py": b"\0"})
    assert validate(image).engine == 0
