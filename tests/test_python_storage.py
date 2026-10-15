# SPDX-License-Identifier: Apache-2.0
"""Protected Python user-data access and lifetime through the common store."""

import asyncio

from test_native import native_console
from test_python import completed, output
from test_python_hub import BUILD, ready

from brickphyrdev.application import Format, Start, Stop, checked, connection, upload


def test_user_data_bounds_copy_and_application_lifetime():
    source = b"""
from pybricks.hubs import PrimeHub
from pybricks.tools import run_task, wait
s = PrimeHub().system
assert s.storage(0, read=512) == bytes(512)
data = bytearray(b'abc')
s.storage(5, write=memoryview(data))
data[0] = 90
assert s.storage(4, read=5) == b'\\x00abc\\x00'
s.storage(511, write=b'Z')
assert s.storage(511, 1) == b'Z'
assert s.storage(512, read=0) == b''
s.storage(512, write=b'')
for args in ({'offset': -1, 'read': 1}, {'offset': 513, 'read': 0},
             {'offset': 0, 'read': -1}, {'offset': 511, 'read': 2},
             {'offset': 511, 'write': b'ZZ'}):
    try:
        s.storage(**args)
    except ValueError:
        pass
    else:
        assert False
for args in ({'offset': 0}, {'offset': 0, 'read': 1, 'write': b'a'},
             {'offset': 0, 'write': 12}):
    try:
        s.storage(**args)
    except TypeError:
        pass
    else:
        assert False
assert s.storage(5, read=3) == b'abc' and s.storage(511, read=1) == b'Z'
s.storage(10, write=b'A')
assert s.storage_save() is None
s.storage(10, write=b'B')
request = s.storage_save(wait=False)
assert request > 0
while not s.storage_save_status(request):
    wait(1)
s.storage(10, write=b'C')
async def save_async():
    assert await s.storage_save() is None
run_task(save_async())
assert s.storage(10, read=1) == b'C'
print('storage passed')
"""

    async def exercise(device):
        async with connection(port=device) as (client, transport):
            try:
                await upload(client, transport, source, Format.PYTHON)
                await checked(client, Start())
                state = await completed(client)
                text = await output(client)
                assert state.result == state.cleanup == 0 and b"storage passed" in text, (
                    state,
                    text,
                )
                await upload(
                    client,
                    transport,
                    b"""
from pybricks.hubs import PrimeHub
s = PrimeHub().system
assert s.storage(5, read=3) == b'abc' and s.storage(511, read=1) == b'Z'
print('ready')
while True:
    s.storage(5, write=b'xyz')
    s.storage(5, write=b'abc')
""",
                    Format.PYTHON,
                )
                await checked(client, Start())
                await ready(client)
                await checked(client, Stop())
                await upload(
                    client,
                    transport,
                    b"""
from pybricks.hubs import PrimeHub
s = PrimeHub().system
assert s.storage(5, read=3) in (b'abc', b'xyz')
s.storage(5, write=b'new')
assert s.storage(5, read=3) == b'new'
print('storage reusable')
""",
                    Format.PYTHON,
                )
                await checked(client, Start())
                state = await completed(client)
                text = await output(client)
                assert state.result == state.cleanup == 0 and b"storage reusable" in text, (
                    state,
                    text,
                )
            finally:
                await checked(client, Stop())

    with native_console(BUILD) as device:
        asyncio.run(exercise(device))
