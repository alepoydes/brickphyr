# SPDX-License-Identifier: Apache-2.0
"""Read-only observations over the existing SMP management connection."""

from smp import error, message

from .smp import Failure, checked

GROUP = 66


class ErrorV1(error.ErrorV1):
    _GROUP_ID = GROUP


class ErrorV2(error.ErrorV2):
    _GROUP_ID = GROUP
    err: Failure


class _Group:
    _GROUP_ID = GROUP
    _ErrorV1 = ErrorV1
    _ErrorV2 = ErrorV2


class InfoResponse(message.ReadResponse):
    _GROUP_ID = GROUP
    _COMMAND_ID = 0
    api: int
    firmware: str
    build: str
    board: str
    sources: list[str]


class Info(_Group, message.ReadRequest):
    _COMMAND_ID = 0
    _Response = InfoResponse


class SnapshotResponse(message.ReadResponse):
    _GROUP_ID = GROUP
    _COMMAND_ID = 1
    api: int
    source: str
    read_ms: int
    error: int
    values: dict[str, int | bytes]


class Snapshot(_Group, message.ReadRequest):
    _COMMAND_ID = 1
    _Response = SnapshotResponse
    source: str


async def info(client):
    result = await checked(client, Info())
    if result.api != 1:
        raise RuntimeError(f"Unsupported telemetry API {result.api}")
    if len(set(result.sources)) != len(result.sources):
        raise RuntimeError("Duplicate telemetry source names")
    return result


async def snapshot(client, source):
    result = await checked(client, Snapshot(source=source))
    if result.api != 1 or result.source != source:
        raise RuntimeError("Mismatched telemetry snapshot")
    return result
