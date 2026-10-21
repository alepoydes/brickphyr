# SPDX-License-Identifier: Apache-2.0
"""Build and validate bounded Brickphyr application file maps."""

from __future__ import annotations

import argparse
import json
import re
import struct
from dataclasses import dataclass
from io import BytesIO
from pathlib import Path
from tempfile import NamedTemporaryFile
from typing import Mapping, Sequence

import cbor2
from elftools.common.exceptions import ELFError
from elftools.elf.elffile import ELFFile

_ABI = re.compile(r"^bp([0-9]+)\.([0-9]+)-armv7em-hf$")
_FORMAT = {".elf": 0, ".py": 1, ".mpy": 2}
_ABI_MAGIC = 0x31415042
_ARCH = 1
_FILE_LIMIT = 64
_PATH_LIMIT = 128


@dataclass(frozen=True)
class NativeIdentity:
    major: int
    minor: int
    architecture: int
    capabilities: int
    stack_size: int


@dataclass(frozen=True)
class PackageInfo:
    entry: str
    engine: int
    components: tuple[str, ...]
    files: Mapping[str, bytes]


def _abi_version(abi: str) -> tuple[int, int]:
    match = _ABI.fullmatch(abi)
    if not match:
        raise ValueError(f"Unsupported Brickphyr ABI: {abi}")
    return int(match[1]), int(match[2])


def validate_native(image: bytes, abi: str | None = None) -> NativeIdentity:
    """Check a native artifact's ELF envelope and declared Brickphyr ABI."""
    try:
        elf = ELFFile(BytesIO(image))
        header = elf.header
        if (elf.elfclass != 32 or not elf.little_endian or
                header["e_type"] != "ET_REL" or header["e_machine"] != "EM_ARM" or
                header["e_version"] != "EV_CURRENT" or header["e_phnum"] != 0 or
                header["e_flags"] & 0xFF000000 != 0x05000000 or
                header["e_flags"] & 0x600 not in (0, 0x400)):
            raise ValueError("Expected a relocatable ARM EABI5 hard-float ELF")
        sections = [part for part in elf.iter_sections() if part.name == ".brickphyr.abi"]
        if len(sections) != 1 or sections[0]["sh_type"] != "SHT_PROGBITS":
            raise ValueError("Missing or repeated .brickphyr.abi section")
        data = sections[0].data()
        if len(data) != 20:
            raise ValueError("Invalid .brickphyr.abi size")
        magic, major, minor, architecture, capabilities, stack_size = struct.unpack(
            "<IHHIII", data
        )
    except (OSError, TypeError, KeyError, struct.error, ELFError) as error:
        raise ValueError("Invalid native ELF") from error
    if magic != _ABI_MAGIC or architecture != _ARCH or stack_size == 0:
        raise ValueError("Invalid Brickphyr native identity")
    if abi is not None:
        expected_major, maximum_minor = _abi_version(abi)
        if major != expected_major or minor > maximum_minor:
            raise ValueError(f"Native ABI bp{major}.{minor} is incompatible with {abi}")
    return NativeIdentity(major, minor, architecture, capabilities, stack_size)


def _path(name: str) -> str:
    if (not isinstance(name, str) or not name or name.startswith("/") or "\0" in name or
            len(name.encode("utf-8")) > _PATH_LIMIT or
            any(part in ("", ".", "..") for part in name.split("/"))):
        raise ValueError(f"Invalid package path: {name!r}")
    return name


def _decode_map(image: bytes) -> dict[str, bytes]:
    stream = BytesIO(image)
    try:
        files = cbor2.CBORDecoder(stream, allow_duplicate_keys=False,
                                  allow_indefinite=False, max_depth=4).decode()
    except (EOFError, cbor2.CBORDecodeError, TypeError) as error:
        raise ValueError("Invalid package CBOR") from error
    if not isinstance(files, dict):
        raise ValueError("Package must be a CBOR map")
    if stream.tell() != len(image) or len(files) - ("" in files) > _FILE_LIMIT:
        raise ValueError("Package has trailing data or too many files")
    for name, content in files.items():
        if not isinstance(name, str) or not isinstance(content, bytes):
            raise ValueError("Package keys must be paths with byte contents")
        if name:
            _path(name)
    names = [name for name in files if name]
    for index, name in enumerate(names):
        if any(other.startswith(name + "/") or name.startswith(other + "/")
               for other in names[:index]):
            raise ValueError(f"Package file/directory collision: {name}")
    return files


def _manifest(data: bytes) -> tuple[int, str, tuple[str, ...]]:
    stream = BytesIO(data)
    try:
        value = cbor2.CBORDecoder(stream).decode()
    except (EOFError, cbor2.CBORDecodeError) as error:
        raise ValueError("Invalid package manifest") from error
    if (stream.tell() != len(data) or not isinstance(value, list) or len(value) != 4 or
            type(value[0]) is not int or value[0] != 1 or
            type(value[1]) is not int or value[1] not in (0, 1, 2) or
            not isinstance(value[2], str) or not isinstance(value[3], list)):
        raise ValueError("Invalid package manifest")
    entry = _path(value[2])
    components = tuple(_path(item) for item in value[3])
    if len(set(components)) != len(components):
        raise ValueError("Repeated native component")
    return value[1], entry, components


def validate(image: bytes, *, abi: str | None = None) -> PackageInfo:
    """Validate a current or legacy FILES image before installation."""
    files = _decode_map(image)
    metadata = files.get("")
    if metadata is not None and metadata.startswith(b"\x84"):
        engine, entry, components = _manifest(metadata)
    elif metadata is not None:
        try:
            entry = _path(metadata.decode("utf-8"))
        except UnicodeDecodeError as error:
            raise ValueError("Invalid legacy entry path") from error
        engine = _FORMAT.get(Path(entry).suffix)
        if engine not in (1, 2):
            raise ValueError("Legacy entry must be Python or MPY")
        components = ()
    else:
        defaults = [name for name in ("main.py", "main.mpy") if name in files]
        if len(defaults) != 1:
            raise ValueError("Package needs exactly one default entry")
        entry = defaults[0]
        engine = _FORMAT[Path(entry).suffix]
        components = ()
    if entry not in files or _FORMAT.get(Path(entry).suffix) != engine:
        raise ValueError("Manifest entry is missing or has the wrong engine")
    for component in components:
        if component == entry or component not in files or not component.endswith(".elf"):
            raise ValueError(f"Invalid native component: {component}")
    for name, content in files.items():
        if name.endswith(".elf"):
            validate_native(content, abi)
        if engine in (1, 2):
            if name.endswith(".py") and b"\0" in content:
                raise ValueError(f"Python file contains a NUL byte: {name}")
            if name.endswith(".mpy") and (len(content) < 4 or content[0] != ord("M") or
                                          content[3] > 31):
                raise ValueError(f"Invalid MPY header: {name}")
    return PackageInfo(entry, engine, components, files)


def _sdk_abi(sdk: Path) -> str:
    try:
        record = json.loads((sdk / "build.json").read_text())
        header = (sdk / "include/brickphyr_abi.h").read_text()
        if (not re.fullmatch(r"[0-9a-f]{64}", record["state"]["config_sha256"]) or
                not re.fullmatch(r"[0-9a-f]{64}", record["artifacts"]["zephyr.elf"]) or
                not isinstance(record["state"]["sources"], dict) or
                not (sdk / "include/brickphyr_api.h").is_file()):
            raise ValueError("SDK build record is incomplete")
        def number(name: str) -> int:
            match = re.search(rf"^#define {name} ([0-9]+)$", header, re.M)
            if not match:
                raise ValueError(f"SDK lacks {name}")
            return int(match[1])
        major, minor = number("BP_ABI_MAJOR"), number("BP_ABI_MINOR")
        if not re.search(r"^#define BP_ABI_ARCH 1U\b", header, re.M):
            raise ValueError("SDK native architecture is unsupported")
    except (OSError, KeyError, json.JSONDecodeError) as error:
        raise ValueError(f"Invalid Brickphyr SDK: {sdk}") from error
    return f"bp{major}.{minor}-armv7em-hf"


def _read(path: Path) -> bytes:
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"Package input must be a regular file: {path}")
    return path.read_bytes()


def build(entry: Path, *, components: Sequence[Path] = (),
          resources: Mapping[str, Path] | None = None, sdk: Path | None = None) -> bytes:
    """Build one explicit package; source freshness belongs to the build tool."""
    entry = Path(entry)
    content: dict[str, bytes] = {entry.name: _read(entry)}
    component_names = []
    for component in components:
        component = Path(component)
        if component.name in content:
            raise ValueError(f"Repeated package path: {component.name}")
        content[component.name] = _read(component)
        component_names.append(component.name)
    for name, path in (resources or {}).items():
        if name in content or name == "":
            raise ValueError(f"Repeated package path: {name}")
        content[_path(name)] = _read(Path(path))
    engine = _FORMAT.get(entry.suffix)
    if engine is None or any(not name.endswith(".elf") for name in component_names):
        raise ValueError("Entry must be .elf/.py/.mpy and components must be .elf")
    if any(name.endswith(".elf") for name in content) and sdk is None:
        raise ValueError("Native package inputs require one Brickphyr SDK")
    abi = _sdk_abi(Path(sdk)) if sdk is not None else None
    content[""] = cbor2.dumps([1, engine, entry.name, component_names], canonical=True)
    image = cbor2.dumps(content, canonical=True)
    validate(image, abi=abi)
    return image


def run(args: argparse.Namespace) -> Path:
    """Write a package from the app pack CLI or exported SDK CMake helper."""
    resources = {}
    for item in getattr(args, "resource", ()) or ():
        name, separator, path = item.partition("=")
        if not separator or name in resources:
            raise ValueError(f"Expected unique resource NAME=PATH: {item}")
        resources[name] = Path(path)
    image = build(Path(args.entry), components=[Path(path) for path in
                  (getattr(args, "component", ()) or ())], resources=resources,
                  sdk=Path(args.sdk) if args.sdk else None)
    output = Path(args.output)
    with NamedTemporaryFile(dir=output.parent, delete=False) as temporary:
        temporary_path = Path(temporary.name)
        temporary.write(image)
    try:
        temporary_path.replace(output)
    finally:
        temporary_path.unlink(missing_ok=True)
    return output


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--entry", required=True, type=Path)
    parser.add_argument("--component", action="append", default=[], type=Path)
    parser.add_argument("--resource", action="append", default=[])
    parser.add_argument("--sdk", type=Path)
    parser.add_argument("output", type=Path)
    print(run(parser.parse_args()))
