# SPDX-License-Identifier: Apache-2.0
"""Exercise typed native delivery through the real SMP and user-mode paths."""

import asyncio
from pathlib import Path

import pytest
from test_native import native_console

from brickphyrdev import host_messages as inbox
from brickphyrdev.application import Start, Stop, checked, connection, upload
from brickphyrdev.smp import Rejected

BUILD = Path(__file__).resolve().parents[1] / "build/native-smp-tests"


async def _completed(client, type_name, request):
    async with asyncio.timeout(2):
        while True:
            state = await inbox.status(client, type_name, request)
            if state.state == 3:
                return state
            await asyncio.sleep(0.01)


async def _registered(client):
    async with asyncio.timeout(2):
        while True:
            try:
                return await inbox.describe(client)
            except Rejected as error:
                if error.code != 61:
                    raise
            await asyncio.sleep(0.01)


def test_native_inbox_lifecycle():
    async def exercise(port):
        async with connection(port=port) as (client, transport):
            await upload(client, transport, (BUILD / "host_inbox_probe.elf").read_bytes())
            await checked(client, Start())
            entries = await _registered(client)
            assert [(entry.type, entry.name, entry.policy) for entry in entries] == [
                (1, "parameters", 1), (2, "action", 2), (3, "release", 2)
            ]
            run = entries[0].run
            first = await inbox.send(client, "parameters", b"\x03")
            replacement = await inbox.send(client, "parameters", b"\x2a")
            assert first.state == replacement.state == 1
            with pytest.raises(Rejected):
                await inbox.send(client, "parameters", b"\x2b", replacement.request)
            assert (await inbox.status(client, "parameters", first.request)).state == 4
            action = await inbox.send(client, "action", b"")
            assert action.state == 1
            with pytest.raises(RuntimeError):
                await inbox.send(client, "action", b"", action.request + 1)
            retry = await inbox.send(client, "action", b"", action.request)
            assert retry.state == 1
            await inbox.send(client, "release", b"")
            assert (await _completed(client, "parameters", replacement.request)).detail == 42
            outcome = await _completed(client, "action", action.request)
            assert (outcome.result, outcome.detail) == (-34, 7)
            await checked(client, Stop())
            await upload(client, transport, (BUILD / "host_inbox_probe.elf").read_bytes())
            await checked(client, Start())
            assert (await _registered(client))[0].run != run
            with pytest.raises(RuntimeError):
                await checked(client, inbox.Status(run=run, type=1, request=1))
            await checked(client, Stop())

    with native_console(BUILD) as port:
        asyncio.run(exercise(port))
