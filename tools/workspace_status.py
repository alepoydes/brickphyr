# SPDX-License-Identifier: Apache-2.0
"""Show manifest pins and local Git state without changing the west workspace."""

import subprocess
import sys
from pathlib import Path

from west.manifest import Manifest


def git_text(project, args, cwd=None):
    result = project.git(args, cwd=cwd, capture_stdout=True, capture_stderr=True)
    return result.stdout.decode(errors="replace").strip()


def display_revision(revision):
    if revision and len(revision) == 40 and all(
        char in "0123456789abcdefABCDEF" for char in revision
    ):
        return revision[:12]
    return revision


def checkout_state(project, cwd=None):
    head = git_text(project, ["rev-parse", "HEAD"], cwd)
    branch = git_text(project, ["branch", "--show-current"], cwd) or "(detached)"
    changes = git_text(
        project,
        ["--no-optional-locks", "status", "--porcelain=v1", "--untracked-files=normal"],
        cwd,
    )
    count = len(changes.splitlines())
    status = f"dirty ({count} {'entry' if count == 1 else 'entries'})" if count else "clean"
    return head, branch, status


def main():
    try:
        manifest = Manifest.from_topdir()
    except Exception as error:
        print(f"Cannot read west manifest: {error}", file=sys.stderr)
        return 2

    failed = False
    for project in manifest.projects:
        if not manifest.is_active(project):
            continue
        label = f"{project.name} ({project.path})"
        path = Path(project.abspath)
        if not project.is_cloned():
            state = "missing" if not path.exists() else "not a Git checkout"
            print(f"{label}: pin={display_revision(project.revision)} | {state}")
            failed = True
            continue
        try:
            head, branch, status = checkout_state(project)
            pin = (
                "HEAD (manifest repository)"
                if project.name == "manifest"
                else display_revision(project.revision)
            )
            print(f"{label}: pin={pin} | HEAD={head[:12]} | branch={branch} | {status}")
            if project.submodules is True:
                tree = git_text(project, ["ls-tree", "-r", "HEAD"])
                submodules = [
                    line.split("\t", 1)[1]
                    for line in tree.splitlines()
                    if line.startswith("160000 commit ")
                ]
            elif isinstance(project.submodules, list):
                submodules = [submodule.path for submodule in project.submodules]
            else:
                submodules = []
            for submodule in submodules:
                gitlink = git_text(project, ["rev-parse", f"HEAD:{submodule}"])
                subpath = path / submodule
                substatus = git_text(project, ["submodule", "status", "--", submodule])
                if substatus.startswith("-"):
                    print(f"  {submodule}: gitlink={gitlink[:12]} | missing (not initialized)")
                    failed = True
                    continue
                subhead, subbranch, subdirty = checkout_state(project, subpath)
                moved = "moved, " if subhead != gitlink else ""
                print(
                    f"  {submodule}: gitlink={gitlink[:12]} | HEAD={subhead[:12]} | "
                    f"branch={subbranch} | {moved}{subdirty}"
                )
        except (OSError, subprocess.CalledProcessError) as error:
            if isinstance(error, subprocess.CalledProcessError) and error.stderr:
                detail = error.stderr.decode(errors="replace").strip()
            else:
                detail = str(error)
            print(f"{label}: pin={display_revision(project.revision)} | ERROR: {detail}")
            failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
