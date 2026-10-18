# SPDX-License-Identifier: Apache-2.0
"""Application-defined host inboxes through the application SMP group."""

from pydantic import PrivateAttr
from smp import message

from . import application
from .smp import checked

_MAX_REQUEST = (1 << 32) - 1


class Descriptor(message.ReadResponse):
    _GROUP_ID = application.GROUP
    _COMMAND_ID = 26
    present: bool
    run: int = 0
    type: int = 0
    name: str = ""
    version: int = 0
    max_size: int = 0
    policy: int = 0
    last: int = 0
    _message_version: int = PrivateAttr(default=0)

    def model_post_init(self, context):
        if self.present and not {
            "run", "type", "name", "version", "max_size", "policy", "last"
        } <= self.model_fields_set:
            raise ValueError("Incomplete application message descriptor")
        # smpclient also calls its protocol version field "version" and replaces
        # it with the header value after decoding the CBOR response.
        object.__setattr__(self, "_message_version", self.version)
        if self.header is not None:
            object.__setattr__(self, "version", self.header.version)
        super().model_post_init(context)

    @property
    def message_version(self):
        return self._message_version


class Describe(application._Group, message.ReadRequest):
    _COMMAND_ID = 26
    _Response = Descriptor
    index: int


class SendResponse(message.WriteResponse):
    _GROUP_ID = application.GROUP
    _COMMAND_ID = 27
    state: int
    result: int
    detail: int
    request: int


class Send(application._Group, message.WriteRequest):
    _COMMAND_ID = 27
    _Response = SendResponse
    run: int
    type: int
    request: int
    data: bytes


class StatusResponse(message.ReadResponse):
    _GROUP_ID = application.GROUP
    _COMMAND_ID = 28
    state: int
    result: int
    detail: int
    request: int


class Status(application._Group, message.ReadRequest):
    _COMMAND_ID = 28
    _Response = StatusResponse
    run: int
    type: int
    request: int


async def describe(client):
    """Return descriptors for the current application run, in firmware order."""
    entries = []
    index = 0
    while True:
        entry = await checked(client, Describe(index=index))
        if not entry.present:
            return entries
        entries.append(entry)
        index += 1


async def _resolve(client, type_or_name):
    entries = await describe(client)
    if isinstance(type_or_name, str) and not type_or_name.isdecimal():
        matches = [entry for entry in entries if entry.name == type_or_name]
    else:
        type_id = int(type_or_name)
        matches = [entry for entry in entries if entry.type == type_id]
    if not matches:
        raise ValueError(f"No application message type {type_or_name!r}")
    if len(matches) != 1:
        raise RuntimeError(f"Ambiguous application message type {type_or_name!r}")
    return matches[0]


async def send(client, type_or_name, data: bytes, request=None):
    """Submit one payload to a discovered inbox and return its initial status."""
    if not isinstance(data, bytes):
        raise TypeError("Message payload must be bytes")
    entry = await _resolve(client, type_or_name)
    if len(data) > entry.max_size:
        raise ValueError(f"Message payload exceeds {entry.max_size} bytes for {entry.name}")
    if request is None:
        request = entry.last + 1
    if not isinstance(request, int) or not 1 <= request <= _MAX_REQUEST:
        raise ValueError("Message request must be between 1 and 4294967295")
    reply = await checked(client, Send(run=entry.run, type=entry.type, request=request, data=data))
    if reply.request != request:
        raise RuntimeError("Mismatched application message request")
    return reply


async def status(client, type_or_name, request=0):
    """Read a request's status; zero selects the latest request."""
    if not isinstance(request, int) or not 0 <= request <= _MAX_REQUEST:
        raise ValueError("Message request must be between 0 and 4294967295")
    entry = await _resolve(client, type_or_name)
    reply = await checked(client, Status(run=entry.run, type=entry.type, request=request))
    if request and reply.request != request:
        raise RuntimeError("Mismatched application message request")
    return reply
