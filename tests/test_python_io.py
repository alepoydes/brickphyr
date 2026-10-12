# SPDX-License-Identifier: Apache-2.0
"""Python streams and resources through the protected VM and production SMP API."""

import asyncio
import subprocess
import sys
from pathlib import Path

import cbor2
import pytest
from test_native import native_console
from test_python import completed, output

from brickphyrdev import console
from brickphyrdev.application import (
    Format,
    Input,
    Interrupt,
    OutputStream,
    Repl,
    Start,
    Status,
    Stop,
    checked,
    connection,
    upload,
)

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/python-smp-tests"


async def until(client, marker):
    result = bytearray()
    deadline = asyncio.get_running_loop().time() + 3
    while marker not in result:
        status = await checked(client, Status())
        result.extend(await output(client))
        if marker in result:
            break
        assert status.state == 1, (status, bytes(result))
        assert asyncio.get_running_loop().time() < deadline, (marker, bytes(result))
        await asyncio.sleep(0.01)
    return bytes(result)


def test_standard_streams_and_input():
    source = b"""
import sys, select, micropython
poll = select.poll()
poll.register(sys.stdin, select.POLLIN)
assert poll.poll(0) == []
print('ready for input')
assert poll.poll(2000)[0][0] is sys.stdin
name = input('Name: ')
assert name == 'Brickphyr'
micropython.kbd_intr(-1)
print('binary ready')
assert sys.stdin.buffer.read(4) == b'\\x00\\r\\x03\\xff'
sys.stdout.write('text stream\\n')
sys.stderr.write('error stream\\n')
sys.stdout.buffer.write(b'binary\\x00\\n')
sys.stdout.flush()
sys.stderr.flush()
for stream, operation in ((sys.stdin.buffer, lambda s: s.write(b'x')),
                           (sys.stdout.buffer, lambda s: s.read(1))):
    try:
        operation(stream)
        assert False
    except OSError:
        pass
poll.unregister(sys.stdin)
poll.register(sys.stdout, select.POLLIN | select.POLLOUT)
assert poll.poll(0)[0][1] == select.POLLOUT
print('streams passed')
"""

    async def exercise(port):
        async with connection(port=port) as (client, transport):
            try:
                await upload(client, transport, source, Format.PYTHON)
                await checked(client, Start())
                await until(client, b"ready for input")
                # Readline's own editing path handles DEL in input(), not a
                # second line editor invented for the SMP transport.
                await checked(client, Input(data=b"Brickphx\x7fyr\n"))
                await until(client, b"binary ready")
                await checked(client, Input(data=b"\x00\r\x03\xff"))
                status = await completed(client)
                remaining = await output(client)
                assert status.result == 0, (status, remaining)
                assert b"text stream\r\n" in remaining
                assert b"error stream\r\n" in remaining
                assert b"binary\x00\n" in remaining and b"streams passed" in remaining
            finally:
                await checked(client, Stop())

    with native_console(BUILD) as port:
        asyncio.run(exercise(port))


def test_repl_to_script_starts_fresh_input():
    async def exercise(port):
        async with connection(port=port) as (client, transport):
            try:
                await checked(client, Repl())
                await until(client, b">>> ")
                await checked(client, Input(data=b"print(42)\r"))
                await until(client, b"42\r\n>>> ")
                await checked(client, Stop())
                await upload(
                    client, transport, b"print('ready')\nprint(repr(input()))\n", Format.PYTHON
                )
                await checked(client, Start())
                await until(client, b"ready\r\n")
                await checked(client, Input(data=b"\n"))
                assert (await completed(client)).result == 0
                assert b"''\r\n" in await output(client)
            finally:
                await checked(client, Stop())

    with native_console(BUILD) as port:
        asyncio.run(exercise(port))


def test_interrupt_backpressure_and_next_application():
    source = b"""
import sys
try:
    print('loop ready')
    while True: pass
except KeyboardInterrupt:
    print('interrupted')
    assert sys.stdin.read(1) == 'z'
print('caught and resumed')
"""

    async def exercise(port):
        async with connection(port=port) as (client, transport):
            try:
                await upload(client, transport, source, Format.PYTHON)
                await checked(client, Start())
                await until(client, b"loop ready")
                await checked(client, Input(data=b"a" * 128))
                await checked(client, Input(data=b"b" * 128))
                with pytest.raises(RuntimeError, match="error 11"):
                    await checked(client, Input(data=b"excess"))
                assert (await checked(client, Status())).state == 1
                # A control byte bypasses full-input backpressure and cancels
                # queued input. Data following it belongs to the next read.
                await checked(client, Input(data=b"\x03z"))
                status = await completed(client)
                result = await output(client)
                assert status.result == 0 and b"caught and resumed" in result, (status, result)
                with pytest.raises(RuntimeError, match="Input rejected"):
                    await checked(client, Input(data=b"after exit"))
                for blocked in (b"input('blocked: ')\n", b"while True: pass\n"):
                    await upload(client, transport, blocked, Format.PYTHON)
                    await checked(client, Start())
                    if blocked.startswith(b"input"):
                        await until(client, b"blocked: ")
                    await checked(client, Interrupt())
                    assert (await completed(client)).result == -8
                    assert b"KeyboardInterrupt" in await output(client)
                await upload(client, transport, b"print('new application')\n", Format.PYTHON)
                await checked(client, Start())
                assert (await completed(client)).result == 0
                assert b"new application" in await output(client)
            finally:
                await checked(client, Stop())

    with native_console(BUILD) as port:
        asyncio.run(exercise(port))


@pytest.mark.parametrize("action", ["drain", "interrupt", "stop", "detach", "disconnect"])
def test_output_stream_backpressure(action):
    source = (
        b"try:\n    print('0123456789' * 409 + '!')\n"
        b"except KeyboardInterrupt: pass\nprint('done')\n"
    )

    async def exercise(port):
        async with console.connection(lambda data: None, management=True, port=port) as (
            transport,
            client,
        ):
            try:
                await checked(client, OutputStream(attached=True))
                await upload(client, transport, source, Format.PYTHON)
                await checked(client, Start())
                async with asyncio.timeout(3):
                    while (state := await checked(client, Status())).queued < 1024:
                        assert state.state == 1
                        await asyncio.sleep(0.01)
                # A full queue stalls the producer, while management remains usable.
                await asyncio.sleep(0.03)
                state = await checked(client, Status())
                assert state.state == 1 and state.dropped == 0
                if action == "stop":
                    await checked(client, Stop())
                    assert (await checked(client, Status())).state == 3
                elif action in ("detach", "disconnect"):
                    if action == "detach":
                        await checked(client, OutputStream(attached=False))
                    else:
                        # QEMU UART has no DTR. The fixture forwards this to the
                        # production session-close notification, without stopping Python.
                        await transport.write(b"session_close\r")
                    state = await completed(client)
                    assert state.result == 0 and state.dropped > 0
                else:
                    if action == "interrupt":
                        await checked(client, Interrupt())
                    text = await until(client, b"done\r\n")
                    state = await completed(client)
                    assert state.result == 0 and state.dropped == 0
                    if action == "drain":
                        assert text == b"0123456789" * 409 + b"!\r\ndone\r\n"
                await checked(client, OutputStream(attached=False))
                await upload(client, transport, b"print('after stream')\n", Format.PYTHON)
                await checked(client, Start())
                assert (await completed(client)).result == 0
                assert b"after stream" in await output(client)
            finally:
                await checked(client, Stop())
                await checked(client, OutputStream(attached=False))

    with native_console(BUILD) as port:
        asyncio.run(exercise(port))


def test_program_resource_files():
    source = b"""
import io, errno, gc
with open('data/values.txt') as f:
    assert f.readline() == 'first\\n'
    assert list(f) == ['second\\n', 'third']
try:
    f.read()
    assert False
except OSError as e:
    assert e.errno == errno.EBADF
with io.open('/data/raw.bin', 'rb') as f:
    assert f.read() == b'\\x00\\r\\xff'
    assert f.seek(-2, 2) == 1
    buf = bytearray(2)
    assert f.readinto(buf) == 2 and buf == b'\\r\\xff'
    f.seek(100)
    assert f.read() == b''
    try:
        f.seek(-200, 1)
        assert False
    except OSError:
        pass
    f.seek(0)
    gc.collect()
    assert f.read(1) == b'\\x00'
for name, error in (('../settings', errno.ENOENT), ('missing', errno.ENOENT),
                    ('data', errno.EISDIR)):
    try:
        open(name)
        assert False
    except OSError as e:
        assert e.errno == error
for mode in ('w', 'wb', 'a', 'x', 'r+'):
    try:
        open('data/raw.bin', mode)
        assert False
    except OSError as e:
        assert e.errno == errno.EROFS
for name, mode in (('data/raw.bin\\x00other', 'r'), ('data/raw.bin', 'rbjunk')):
    try:
        open(name, mode)
        assert False
    except ValueError:
        pass
assert open('empty', 'rb').read() == b''
print('resource files passed')
"""

    async def exercise(port):
        async with connection(port=port) as (client, transport):
            image = cbor2.dumps(
                {
                    "main.py": source,
                    "data/values.txt": b"first\nsecond\nthird",
                    "data/raw.bin": b"\x00\r\xff",
                    "empty": b"",
                }
            )
            await upload(client, transport, image, Format.FILES)
            await checked(client, Start())
            status = await completed(client)
            result = await output(client)
            assert status.result == 0 and b"resource files passed" in result, (status, result)

    with native_console(BUILD) as port:
        asyncio.run(exercise(port))


def test_input_cli(tmp_path):
    source = tmp_path / "input.py"
    source.write_text("name = input('name: ')\nprint('Hello', name)\n")
    with native_console(BUILD) as port:
        cli = [Path(sys.executable).with_name("brickphyrdev"), "app", "--port", port]
        for operation, expected in (
            (["run", str(source)], '"state": "running"'),
            (["input", "Brickphyr"], '"format": "python"'),
            (["output"], "Hello Brickphyr"),
        ):
            result = subprocess.run([*cli, *operation], capture_output=True, text=True, timeout=5)
            assert result.returncode == 0 and expected in result.stdout, (
                result.stdout + result.stderr
            )
