# SPDX-License-Identifier: Apache-2.0
"""Composite applications through real LittleFS, SMP and protected ARM execution."""

import asyncio
from pathlib import Path

import cbor2
import pytest
from smpclient.exceptions import SMPUploadError
from smpclient.requests.file_management import FileClose, FileUpload
from test_native import incompatible_native, native_console

from brickphyrdev.application import (
    Format,
    ListFiles,
    Memory,
    Output,
    PrepareInstall,
    Publish,
    Remove,
    RunStored,
    Status,
    Stop,
    checked,
    connection,
    files,
    install,
)
from brickphyrdev.storage_errors import upload_storage_failure

BUILD = Path(__file__).resolve().parents[1] / "build/native-smp-tests"


def test_composite_install_launch_and_recovery():
    primary = (BUILD / "control_resource_probe.elf").read_bytes()
    control = (BUILD / "control_probe.elf").read_bytes()
    contents = {
        "": cbor2.dumps([1, 0, "primary.elf", ["controller.elf"]]),
        "primary.elf": primary,
        "controller.elf": control,
        "padding.bin": bytes(40000 - len(primary) - len(control)),
    }
    image = cbor2.dumps(contents, canonical=True)

    async def exercise(port):
        async with connection(port=port) as (client, _):
            status = await checked(client, Status())
            assert status.formats == 9 and len(image) > status.limit  # No Python; exceeds RAM-upload cap.
            await install(client, image, "robot", Format.FILES)
            available = (await checked(client, ListFiles(name=""))).available
            assert available < len(image)  # Two complete versions would not fit.
            await checked(client, RunStored(name="robot"))
            text = bytearray()
            async with asyncio.timeout(2):
                while b"package controller passed" not in text:
                    text.extend((await checked(client, Output())).data)
                    status = await checked(client, Status())
                    assert status.state == 1 and status.cleanup == 0, (status, text)
                    await asyncio.sleep(0.005)
            memory = await checked(client, Memory())
            assert memory.source_ram == 0 and memory.source_flash == len(image)
            with pytest.raises(RuntimeError, match="error 16"):
                await checked(client, Remove(name="robot"))
            # Replacement joins both contexts, needs one version's space, and never starts it.
            await install(client, image, "robot", Format.FILES)
            assert (await checked(client, Status())).state == 3
            assert [item.name async for item in files(client)] == ["robot"]
            assert (await checked(client, ListFiles(name=""))).available == available
            assert (await checked(client, Memory())).source_flash == 0
            # Exercise the firmware admission directly, independently of host validation.
            missing = {key: value for key, value in contents.items() if key != "padding.bin"}
            del missing["controller.elf"]
            broken = missing | {"controller.elf": incompatible_native(control)}
            truncated = cbor2.dumps(missing | {"controller.elf": control})[:-1]
            for invalid in (cbor2.dumps(missing), cbor2.dumps(broken), truncated):
                await checked(client, PrepareInstall(name="robot"))
                async for _ in client.upload_file(invalid, "/apps/.incoming"):
                    pass
                with pytest.raises(RuntimeError, match="Publish rejected"):
                    await checked(client, Publish(name="robot", size=len(invalid)))
                assert not [item async for item in files(client)]
            # An unfinished transfer stays hidden even after its connection is lost.
            await checked(client, PrepareInstall(name="robot"))
            await checked(client, FileUpload(name="/apps/.incoming", off=0, len=len(image), data=image[:64]))
        async with connection(port=port) as (client, _):
            assert not [item async for item in files(client)]
            await install(client, image, "robot", Format.FILES)
            # Real LittleFS ENOSPC reaches the standard client's parsed error.
            free = (await checked(client, ListFiles(name=""))).available
            with pytest.raises(SMPUploadError) as caught:
                async for _ in client.upload_file(bytes(free + 4096), "/apps/.incoming"):
                    pass
            failure = upload_storage_failure(caught.value)
            assert failure is not None and failure.cause == "ENOSPC", caught.value
            await checked(client, FileClose())
            await checked(client, Stop())
            await checked(client, Remove(name="robot"))

    with native_console(BUILD) as port:
        asyncio.run(exercise(port))
