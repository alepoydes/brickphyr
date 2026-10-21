# SPDX-License-Identifier: Apache-2.0
"""An interrupted reply must not turn a completed install into another deletion."""

import asyncio
from types import SimpleNamespace

import cbor2
import pytest

from brickphyrdev import application as app


@pytest.mark.parametrize("failure,readback", [("preparation", None),
                                            ("publication", True), ("publication", False)])
def test_install_lost_reply(monkeypatch, failure, readback):
    image = cbor2.dumps({"main.py": b"pass\n"})
    calls = []
    interrupted = TimeoutError("reply lost")

    async def checked(client, request):
        calls.append(type(request).__name__)
        if isinstance(request, app.Status):
            return SimpleNamespace(api=1, install=1, abi="bp2.8-armv7em-hf", formats=15)
        if isinstance(request, app.ListFiles):
            return SimpleNamespace(available=10000)
        if isinstance(request, app.PrepareInstall) and failure == "preparation":
            raise interrupted
        if isinstance(request, app.Publish):
            raise interrupted

    class Client:
        async def upload_file(self, data, path):
            calls.append("upload")
            assert data == image and path == "/apps/.incoming"
            yield len(data)

        async def download_file(self, path):
            calls.append("readback")
            assert path == "/apps/robot"
            return image if readback else b"different"

    monkeypatch.setattr(app, "checked", checked)
    operation = app.install(Client(), image, "robot", app.Format.FILES)
    if readback is True:
        asyncio.run(operation)
        assert calls[-1] == "readback"
    else:
        message = "preparation did not complete" if failure == "preparation" else "outcome unknown"
        with pytest.raises(RuntimeError, match=message) as caught:
            asyncio.run(operation)
        assert caught.value.__cause__ is interrupted
    assert calls.count("PrepareInstall") == 1
    if failure == "preparation":
        assert "upload" not in calls
