# SPDX-License-Identifier: Apache-2.0
"""Release preflight against real Git checkouts, including a moved submodule."""

import subprocess

import yaml

from tools.workspace_status import main


def test_release_check_detects_unpinned_code_and_stale_resolution(tmp_path, monkeypatch):
    def git(path, *args):
        return subprocess.check_output([
            "git", "-c", "user.name=Codex", "-c", "user.email=codex@localhost",
            "-c", "commit.gpgsign=false", "-C", str(path), *args,
        ], stderr=subprocess.PIPE, text=True).strip()

    def commit(path):
        git(path, "add", ".")
        git(path, "commit", "-qm", "fixture")
        return git(path, "rev-parse", "HEAD")

    workspace = tmp_path / "workspace"
    root, dependency, source = (workspace / "app", workspace / "dependency", tmp_path / "module")
    for path in (root, dependency, source):
        path.mkdir(parents=True)
        git(path, "init", "-q")
    (source / "code").write_text("original\n")
    commit(source)
    git(dependency, "-c", "protocol.file.allow=always", "submodule", "add", "-q", str(source), "module")
    pin = commit(dependency)
    git(dependency, "update-ref", "refs/heads/manifest-rev", pin)
    (workspace / ".west").mkdir()
    (workspace / ".west/config").write_text("[manifest]\npath = app\n")

    def update_pin():
        manifest = {"manifest": {"projects": [{"name": "dependency", "url": str(dependency),
                    "revision": git(dependency, "rev-parse", "HEAD"),
                    "submodules": [{"path": "module"}]}], "self": {"path": "app"}}}
        (root / "west.yml").write_text(yaml.safe_dump(manifest))
        commit(root)

    update_pin()
    monkeypatch.chdir(root)
    assert main(["--check"]) == 0

    (dependency / "code").write_text("new dependency code\n")
    commit(dependency)
    assert main([]) == 0  # Ordinary status remains useful during development.
    assert main(["--check"]) == 1
    update_pin()
    assert main(["--check"]) == 1  # Matching HEAD still leaves west resolution stale.
    git(dependency, "update-ref", "refs/heads/manifest-rev", "HEAD")
    assert main(["--check"]) == 0

    module = dependency / "module"
    (module / "code").write_text("local runtime fix\n")
    commit(module)
    assert main(["--check"]) == 1
    commit(dependency)
    update_pin()
    git(dependency, "update-ref", "refs/heads/manifest-rev", "HEAD")
    assert main(["--check"]) == 0
    (module / "code").write_text("uncommitted runtime fix\n")
    assert main(["--check"]) == 1
