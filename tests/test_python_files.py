# SPDX-License-Identifier: Apache-2.0
"""Imports and atomic Python installation through the production ARM/SMP path."""

import asyncio
import re
import subprocess
import sys
from pathlib import Path

import cbor2
import pytest
from pybricksdev.compile import compile_file
from smpclient.requests.file_management import FileUpload
from test_native import native_console
from test_python import completed, output

from brickphyrdev import console, recording
from brickphyrdev.application import (
    Format,
    Input,
    Output,
    Publish,
    Remove,
    RunStored,
    Start,
    Status,
    Stop,
    checked,
    connection,
    files,
    install,
    upload,
)

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/python-smp-tests"


def test_python_imports(tmp_path):
    (tmp_path / "bytecode.py").write_text("value = 11\n")

    async def exercise(port):
        bytecode = await compile_file(str(tmp_path), "bytecode.py", 6)
        image = cbor2.dumps(
            {
                "main.py": b"""import gc, sys, package, bytecode, native_module
assert __file__ == 'main.py' and sys.argv == ['main.py']
assert package.value + bytecode.value == 42
gc.collect()
assert native_module.add(30) == 42
try:
    import missing
except ImportError:
    print('missing import caught')
print('imports 42')
""",
                "package/__init__.py": b"from .child import value\n",
                "package/child.py": b"value = 31\n",
                "bytecode.mpy": bytecode,
                "native_module.mpy": (BUILD / "python-fixtures/native_module.mpy").read_bytes(),
            }
        )
        async with connection(port=port) as (client, transport):
            try:
                await upload(client, transport, image, Format.FILES)
                await checked(client, Start())
                status = await completed(client)
                text = await output(client)
                assert status.result == 0, (status, text)
                assert b"imports 42" in text and b"missing import caught" in text, text
                assert status.format == Format.FILES and status.code_used > 0
                await upload(client, transport, b"import package\n", Format.PYTHON)
                await checked(client, Start())
                assert (await completed(client)).result == -8
                assert b"ImportError" in await output(client)
                # The .mpy entry uses the same imports as source, including packages.
                (tmp_path / "entry.py").write_text("import package\nprint(package.value)\n")
                main = await compile_file(str(tmp_path), "entry.py", 6)
                await upload(
                    client,
                    transport,
                    cbor2.dumps(
                        {
                            "main.mpy": main,
                            "package/__init__.py": b"value = 84\n",
                        }
                    ),
                    Format.FILES,
                )
                await checked(client, Start())
                assert (await completed(client)).result == 0
                assert b"84" in await output(client)
                # A loader exception must cancel its code reservation so a
                # different native module can still be imported in this VM.
                native = (BUILD / "python-fixtures/native_module.mpy").read_bytes()
                await upload(
                    client,
                    transport,
                    cbor2.dumps(
                        {
                            "main.py": b"""try:
    import broken
except ValueError:
    print('truncated import caught')
import native_module
assert native_module.add(30) == 42
print('native import recovered')
""",
                            "broken.mpy": native[:-1],
                            "native_module.mpy": native,
                        }
                    ),
                    Format.FILES,
                )
                await checked(client, Start())
                status = await completed(client)
                text = await output(client)
                assert status.result == 0 and b"native import recovered" in text, (status, text)
                assert b"truncated import caught" in text
            finally:
                await checked(client, Stop())

    with native_console(BUILD) as port:
        asyncio.run(exercise(port))


def test_python_file_map_releases_system_heap():
    source = b"""import native_module, gc
assert native_module.add(30) == 42
gc.collect()
assert gc.mem_free() > 40000
with open("payload.bin", "rb") as resource:
    assert resource.read(1) == b"x"
    resource.seek(-1, 2)
    assert resource.read() == b"x"
print('ready')
input()
print('returned 42')
"""
    entries = {
        "main.py": source,
        "native_module.mpy": (BUILD / "python-fixtures/native_module.mpy").read_bytes(),
        "payload.bin": b"x",
    }
    # A 32 KiB synthetic bundle retains a resource alongside real native code.
    entries["payload.bin"] = b"x" * (32768 - len(cbor2.dumps(entries)) - 1)
    image = cbor2.dumps(entries)
    assert len(image) == 32768

    async def exercise(port):
        shell = bytearray()
        async with console.connection(shell.extend, management=True, port=port) as (
            transport,
            client,
        ):
            async def heap_allocated():
                shell.clear()
                await transport.write(b"kernel heap\r")
                async with asyncio.timeout(2):
                    while not shell.endswith(b"brickphyr> "):
                        await asyncio.sleep(0.005)
                match = re.search(rb"\r\nallocated:\s+(\d+)", shell)
                assert match, bytes(shell)
                return int(match[1])

            baseline = await heap_allocated()
            for stored in (False, True):
                if stored:
                    await install(client, image, "memory", Format.FILES)
                    await checked(client, RunStored(name="memory"))
                else:
                    await upload(client, transport, image, Format.FILES)
                    await checked(client, Start())
                text = bytearray()
                async with asyncio.timeout(3):
                    while b"ready\r\n" not in text:
                        text.extend(await output(client))
                        status = await checked(client, Status())
                        assert status.state == 1, (status, bytes(text))
                        await asyncio.sleep(0.005)
                assert await heap_allocated() > baseline
                await checked(client, Input(data=b"\n"))
                status = await completed(client)
                text.extend(await output(client))
                assert status.result == status.cleanup == 0, (status, bytes(text))
                assert b"returned 42" in text, bytes(text)
                await checked(client, Stop())
                assert await heap_allocated() == baseline
                if stored:
                    await checked(client, Remove(name="memory"))

    with native_console(BUILD) as port:
        asyncio.run(exercise(port))


def test_python_file_admission():
    valid = cbor2.dumps({"main.py": b"pass\n"})
    # Hand-constructed CBOR with a duplicate key: a host dict cannot represent it.
    duplicate = b"\xa2" + valid[1:] + valid[1:]
    invalid = [valid[:-1], valid + b"\0", duplicate, cbor2.dumps({}), b"\xbf"]
    invalid += [
        cbor2.dumps({"main.py": b"pass", name: b""})
        for name in (
            "../escape.py",
            "/main.py",
            "a/../b.py",
            "a//b.py",
            "a/",
            "",
            "bad\0.py",
            "a" * 129,
        )
    ]
    invalid += [
        cbor2.dumps(value)
        for value in (
            {"main.py": b"pass", "main.mpy": b"M\x06\0\x1f"},
            {"main.py": b"pass", "main.py/child.py": b""},
            {"main.py": "not a byte string"},
            {"main.py": b"pass", "wrong.mpy": b"M\xff\0\x1f"},
            {"main.py": b"pass", **{f"module{i}.py": b"" for i in range(64)}},
            {"": b"missing.py", "main.py": b"pass"},
            {"": b"../main.py", "main.py": b"pass"},
            {"": b"data.txt", "data.txt": b"pass"},
            {"": "main.py", "main.py": b"pass"},
            {"": b"main.py", "main.py": b"pass", **{f"m{i}.py": b"" for i in range(64)}},
        )
    ]
    metadata = cbor2.dumps({"": b"main.py"})[1:]
    invalid.append(b"\xa3" + metadata + metadata + valid[1:])

    async def exercise(port):
        async with connection(port=port) as (client, transport):
            await upload(client, transport, b"while True: pass\n", Format.PYTHON)
            await checked(client, Start())
            try:
                for image in invalid:
                    with pytest.raises(RuntimeError, match="FormattedUpload rejected"):
                        await upload(client, transport, image, Format.FILES)
                    assert (await checked(client, Status())).state == 1
                # Empty source is a valid entry; rejected uploads did not damage the VM.
                await upload(client, transport, cbor2.dumps({"main.py": b""}), Format.FILES)
                await checked(client, Start())
                assert (await completed(client)).result == 0
            finally:
                await checked(client, Stop())

    with native_console(BUILD) as port:
        asyncio.run(exercise(port))


def test_python_explicit_entry():
    # Metadata is not a file. Exercise the final file at the 64-file boundary,
    # and keep an ordinary module called main separate from the entry point.
    image = cbor2.dumps(
        {
            "": b"start.py",
            "start.py": b"""import sys, main, last
assert __file__ == 'start.py' and sys.argv == ['start.py']
assert main.value + last.value == 42
try:
    open('')
except OSError:
    pass
else:
    raise AssertionError('entry metadata exposed as a file')
print('explicit entry 42')
""",
            "main.py": b"value = 40\n",
            **{f"module{i}.py": b"" for i in range(61)},
            "last.py": b"value = 2\n",
        }
    )

    async def exercise(port):
        async with connection(port=port) as (client, transport):
            await upload(client, transport, image, Format.FILES)
            await checked(client, Start())
            state = await completed(client)
            text = await output(client)
            assert state.result == 0 and b"explicit entry 42" in text, (state, text)

    with native_console(BUILD) as port:
        asyncio.run(exercise(port))


def test_python_store_keeps_running_version():
    original = cbor2.dumps(
        {
            "main.py": b"""import sys
count = 0
while True:
    import version
    assert version.value == 1
    with open('version.txt') as data:
        assert data.read() == '1'
    del sys.modules['version']
    count += 1
    if count % 100 == 0: print(count)
""",
            "version.py": b"value = 1\n",
            "version.txt": b"1",
        }
    )
    replacement = cbor2.dumps(
        {
            "main.py": b"import version\nassert open('version.txt').read() == '2'\n"
            b"print('new version', version.value)\n",
            "version.py": b"value = 2\n",
            "version.txt": b"2",
        }
    )

    async def heartbeat(client, after=0):
        pending = b""
        async with asyncio.timeout(3):
            while True:
                state = await checked(client, Status())
                assert state.state == 1, state
                pending += (await checked(client, Output())).data
                while b"\n" in pending:
                    line, pending = pending.split(b"\n", 1)
                    count = int(line.strip())
                    if count > after:
                        return count
                await asyncio.sleep(0.01)

    async def exercise(port):
        async with connection(port=port) as (client, transport):
            try:
                await install(client, original, "program", Format.FILES)
                await install(client, b"print('second')\n", "second", Format.PYTHON)
                assert (await checked(client, Status())).state == 0
                assert [x.name async for x in files(client)] == ["program", "second"]
                await checked(client, RunStored(name="program"))
                before = await heartbeat(client)
                await checked(
                    client,
                    FileUpload(
                        name="/apps/.incoming", off=0, len=len(replacement), data=replacement[:4]
                    ),
                )
                with pytest.raises(RuntimeError, match="Publish rejected"):
                    await checked(client, Publish(name="program", size=len(replacement)))
                assert await client.download_file("/apps/program") == original
                with pytest.raises(RuntimeError, match="Publish rejected"):
                    await install(
                        client, cbor2.dumps({"../escape.py": b""}), "program", Format.FILES
                    )
                assert await client.download_file("/apps/program") == original
                await install(client, replacement, "program", Format.FILES)
                before = await heartbeat(client, before)
                await checked(client, Remove(name="program"))
                await heartbeat(client, before)
                await install(client, replacement, "program", Format.FILES)
                await checked(client, RunStored(name="program"))
                assert (await completed(client)).result == 0
                assert b"new version 2" in await output(client)
                await checked(client, RunStored(name="second"))
                assert (await completed(client)).result == 0
                assert b"second" in await output(client)
                await checked(client, Remove(name="program"))
                await checked(client, Remove(name="second"))
                assert not [x async for x in files(client)]
            finally:
                await checked(client, Stop())

    with native_console(BUILD) as port:
        asyncio.run(exercise(port))


def test_python_directory_cli(tmp_path):
    (tmp_path / "main.py").write_text("import helper\nprint(helper.value)\n")
    (tmp_path / "helper.py").write_text("value = 'directory works'\n")
    with native_console(BUILD) as port:
        cli = [Path(sys.executable).with_name("brickphyrdev"), "app", "--port", port]
        for operation, expected in (
            (["run", str(tmp_path)], '"format": "files"'),
            (["output"], "directory works"),
            (["install", str(tmp_path), "--name", "example"], '"state": "returned"'),
            (["list"], "example\t"),
            (["start", "example"], '"format": "files"'),
            (["output"], "directory works"),
            (["remove", "example"], '"state": "returned"'),
        ):
            result = subprocess.run([*cli, *operation], capture_output=True, text=True, timeout=8)
            assert result.returncode == 0 and expected in result.stdout, (
                result.stdout + result.stderr
            )


def test_controller_file_sources():
    # Both RAM and stored Python packages reference their ELF resource directly;
    # the native primary resolves the same installed controller by name.
    controller = (BUILD / "control_probe.elf").read_bytes()
    package = cbor2.dumps({
        "main.py": b"""import brickphyr, gc, struct
brickphyr.control_start('controller.elf', struct.pack('<4I', 0, 7, 0xfffffff8, 0))
for _ in range(10000):
    state = brickphyr.control_status()
    if state['sequence'] >= 3:
        break
    gc.collect()
else:
    raise AssertionError('controller did not progress')
print('resource controller passed')
""",
        "controller.elf": controller,
    })

    async def exercise(port):
        async with connection(port=port) as (client, transport):
            try:
                for stored in (False, True):
                    if stored:
                        await install(client, package, "package", Format.FILES)
                        await checked(client, RunStored(name="package"))
                    else:
                        await upload(client, transport, package, Format.FILES)
                        await checked(client, Start())
                    state = await completed(client)
                    text = await output(client)
                    assert state.result == state.cleanup == 0, (state, text)
                    assert state.control_stack and b'resource controller passed' in text
                    await checked(client, Stop())
                    await checked(client, recording.Clear())
                await install(client, controller, "controller", Format.NATIVE)
                await upload(client, transport, (BUILD / "control_file_probe.elf").read_bytes())
                await checked(client, Start())
                state = await completed(client)
                text = await output(client)
                assert state.result == state.cleanup == 0, (state, text)
                assert b'stored controller passed' in text and state.control_stack
            finally:
                await checked(client, Stop())
                await checked(client, recording.Clear())

    with native_console(BUILD) as port:
        asyncio.run(exercise(port))
