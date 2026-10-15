# SPDX-License-Identifier: Apache-2.0
"""Memory inspection and launch requests at the host SMP boundary."""

import asyncio
import contextlib
import json
from types import SimpleNamespace

import cbor2
import pytest

from brickphyrdev import application


def test_memory_snapshot_and_stack_launch_order(monkeypatch, tmp_path, capsys):
    requests = []
    uploads = []

    class Reply(SimpleNamespace):
        def model_dump(self, *, exclude):
            return {key: value for key, value in vars(self).items() if key not in exclude}

    async def checked(_client, request):
        requests.append(request)
        if isinstance(request, application.Memory):
            return Reply(stack_min=2048, stack_max=16384, heap_total=100, gc_used=-1)
        if isinstance(request, application.Stack):
            return Reply(name="main", requested=4096, user=4096, unused=512,
                         privileged=1024, priv_unused=-95, kind="primary", next=-1,
                         present=True)
        return Reply()

    async def upload(_client, _transport, image, format):
        uploads.append((image, format))

    monkeypatch.setattr(application, "checked", checked)
    monkeypatch.setattr(application, "upload", upload)

    @contextlib.asynccontextmanager
    async def connection(**_kwargs):
        yield None, None

    monkeypatch.setattr(application, "connection", connection)
    script = tmp_path / "main.py"
    script.write_text("print('ok')\n")

    async def exercise():
        memory_args = SimpleNamespace(command="memory", interval=None, ble=None, port=None,
                                      serial=None)
        await application.execute(memory_args)
        printed = json.loads(capsys.readouterr().out)
        assert printed["overview"]["gc_used"] == -1
        assert printed["stacks"][0]["kind"] == "primary"
        requests.clear()

        snapshot = await application.memory_snapshot(None)
        assert snapshot == {
            "overview": {"stack_min": 2048, "stack_max": 16384,
                         "heap_total": 100, "gc_used": -1},
            "stacks": [{"name": "main", "requested": 4096, "user": 4096,
                        "unused": 512, "privileged": 1024, "priv_unused": -95,
                        "kind": "primary"}],
        }
        assert [type(request) for request in requests] == [application.Memory, application.Stack]
        assert cbor2.loads(requests[1].BYTES[8:]) == {"index": 0}
        requests.clear()

        args = SimpleNamespace(command="run", image=str(script), format=None, stack=4096)
        await application.launch(None, None, args)
        assert [type(request) for request in requests] == [
            application.Memory, application.StartWithStack]
        assert cbor2.loads(requests[-1].BYTES[8:]) == {"stack": 4096}
        assert len(uploads) == 1
        requests.clear()
        uploads.clear()

        with pytest.raises(ValueError, match="2048 through 16384"):
            await application.launch(None, None, SimpleNamespace(**(vars(args) | {"stack": 1024})))
        assert [type(request) for request in requests] == [application.Memory]
        assert not uploads
        requests.clear()

        await application.launch(None, None, SimpleNamespace(command="start", name="program"))
        assert [type(request) for request in requests] == [application.RunStored]
        assert cbor2.loads(requests[0].BYTES[8:]) == {"name": "program"}

    asyncio.run(exercise())
