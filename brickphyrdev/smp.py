# SPDX-License-Identifier: Apache-2.0
"""Shared management connection and response validation."""

import contextlib
import sys

from pydantic import BaseModel
from smpclient import SMPClient, success
from smpclient.generics import error_v2
from smpclient.transport.ble import SMPBLETransport
from smpclient.transport.serial import SMPSerialTransport
from usb.util import dispose_resources

from . import usb


class Failure(BaseModel):
    group: int
    rc: int


async def checked(client, request):
    reply = await client.request(request)
    if (reply.header.group_id, reply.header.command_id, int(reply.header.op)) != (
        request.header.group_id,
        request.header.command_id,
        int(request.header.op) + 1,
    ):
        raise RuntimeError("Mismatched SMP response header")
    if not success(reply):
        detail = (
            f"group {reply.err.group}, error {reply.err.rc}"
            if error_v2(reply)
            else f"SMP error {reply.rc}"
        )
        raise RuntimeError(f"{type(request).__name__} rejected: {detail}")
    return reply


@contextlib.asynccontextmanager
async def connection(*, ble=None, port=None, serial_number=None):
    hub = None
    client = None
    try:
        if ble:
            transport, address = SMPBLETransport(), ble
        else:
            if port is None:
                hub = usb.device(serial_number)
                usb.select(hub, "shell")
                port = usb.serial_port(hub)
            transport, address = SMPSerialTransport(exclusive=True, write_timeout=3), port
        client = SMPClient(transport, address, timeout_s=5)
        await client.connect()
        yield client, transport
    finally:
        # Preserve the operation's first error if releasing a link also fails.
        original = sys.exception()
        try:
            if client:
                if original:
                    with contextlib.suppress(Exception):
                        await client.disconnect()
                else:
                    await client.disconnect()
        finally:
            if hub is not None:
                if sys.exception() is not None:
                    with contextlib.suppress(Exception):
                        dispose_resources(hub)
                else:
                    dispose_resources(hub)

