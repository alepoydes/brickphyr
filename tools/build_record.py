# SPDX-License-Identifier: Apache-2.0
"""Bind SDK/release metadata to the inputs and output of the firmware link."""

import argparse
import hashlib
import json
import os
import re
import subprocess
from pathlib import Path
from tempfile import NamedTemporaryFile

from west.manifest import Manifest


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_state(path: Path) -> dict:
    def git(*args):
        return subprocess.check_output(["git", "--no-optional-locks", "-C", str(path), *args])

    state = {"revision": git("rev-parse", "HEAD").decode().strip(),
             "dirty": bool(git("status", "--porcelain=v1"))}
    if state["dirty"]:
        # A second edit in an already dirty checkout must invalidate its SDK too.
        changes = hashlib.sha256(git("diff", "--binary", "--no-ext-diff",
                                     "--no-textconv", "HEAD", "--"))
        for name in sorted(git("ls-files", "--others", "--exclude-standard", "-z").split(b"\0")):
            if not name:
                continue
            file = path / os.fsdecode(name)
            link = file.is_symlink()
            data = os.fsencode(file.readlink()) if link else file.read_bytes()
            changes.update(name + b"\0" + bytes([link]) + hashlib.sha256(data).digest())
        state["changes_sha256"] = changes.hexdigest()
    return state


def snapshot(build: Path, version: str) -> dict:
    manifest = Manifest.from_topdir()
    projects = {p.path: Path(p.abspath) for p in manifest.projects
                if p.name != "manifest" and manifest.is_active(p)}
    projects["brickphyr"] = Path(__file__).resolve().parents[1]
    pybricks = manifest.get_projects(["pybricks"])[0]
    projects[f"{pybricks.path}/micropython"] = Path(pybricks.abspath) / "micropython"
    sources = {name: source_state(path) for name, path in sorted(projects.items())}
    return {"version": version, "sources": sources,
            "config_sha256": digest(build / "zephyr/.config"),
            "dts_sha256": digest(build / "zephyr/zephyr.dts")}


def write_json(path: Path, value: dict) -> None:
    data = json.dumps(value, indent=2, sort_keys=True) + "\n"
    if path.exists() and path.read_text() == data:
        return
    with NamedTemporaryFile(mode="w", dir=path.parent, delete=False) as output:
        temporary = Path(output.name)
        output.write(data)
    try:
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def artifacts(build: Path) -> dict:
    zephyr = build / "zephyr"
    names = ["zephyr.elf"]
    if (zephyr / "zephyr.bin").exists():
        names.append("zephyr.bin")
    return {name: digest(zephyr / name) for name in names}


def finish(build: Path) -> None:
    state = json.loads((build / "brickphyr-source.json").read_text())
    if state != snapshot(build, state["version"]):
        raise ValueError("Build source changed during compilation; rebuild")
    if state["version"]:
        header = (build / "zephyr/include/generated/zephyr/app_version.h").read_text()
        match = re.search(r'^#define APP_VERSION_STRING\s+"([^"]+)"$', header, re.M)
        if not match or match[1] != state["version"]:
            raise ValueError("Build version differs from generated application version")
    write_json(build / "zephyr/brickphyr-build.json",
               {"state": state, "artifacts": artifacts(build)})


def verify(build: Path, *, current: bool = True, clean: bool = False) -> dict:
    record = json.loads((build / "zephyr/brickphyr-build.json").read_text())
    state = record["state"]
    if (record["artifacts"] != artifacts(build)
            or state["config_sha256"] != digest(build / "zephyr/.config")
            or state["dts_sha256"] != digest(build / "zephyr/zephyr.dts")):
        raise ValueError("Build artifacts differ from the completed firmware link; rebuild")
    if clean and any(source["dirty"] for source in state["sources"].values()):
        raise ValueError("Build source is dirty; rebuild from clean checkouts")
    if current and state != snapshot(build, state["version"]):
        raise ValueError("Build source differs from the current workspace; rebuild")
    return record


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("snapshot", "finish"))
    parser.add_argument("build", type=Path)
    parser.add_argument("--version", default="")
    args = parser.parse_args()
    if args.operation == "snapshot":
        write_json(args.build / "brickphyr-source.json", snapshot(args.build, args.version))
    else:
        finish(args.build)
