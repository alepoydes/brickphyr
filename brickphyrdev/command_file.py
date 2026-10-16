# SPDX-License-Identifier: Apache-2.0
"""Execute a checked command file through an already-owned SMP Shell client."""

import asyncio
from dataclasses import dataclass
from pathlib import Path

from pydantic import Field
from smp import shell_management
from smpclient.requests import shell_management as shell_requests

from .smp import checked


class ExecuteResponse(shell_management.ExecuteResponse):
    # The firmware extends the standard Shell response without changing its group.
    truncated: bool = Field(default=False, strict=True)


class Execute(shell_requests.Execute):
    _Response = ExecuteResponse


@dataclass(frozen=True)
class Command:
    path: Path
    number: int
    line: str


class CommandError(RuntimeError):
    def __init__(self, command, detail):
        super().__init__(f"{command.path}:{command.number}: {detail}")


def load_commands(path):
    """Validate the entire UTF-8 file before any command is sent."""
    path = Path(path)
    raw = path.read_bytes()
    try:
        content = raw.decode("utf-8")
    except UnicodeDecodeError as error:
        number = raw.count(b"\n", 0, error.start) + 1
        raise ValueError(f"{path}:{number}: Invalid UTF-8") from error
    commands = []
    for number, line in enumerate(content.split("\n"), 1):
        line = line.removesuffix("\r")
        if "\0" in line:
            raise ValueError(f"{path}:{number}: Embedded NUL")
        if line.strip() and not line.lstrip().startswith("#"):
            commands.append(Command(path, number, line))
    return tuple(commands)


async def execute(client, commands, output):
    """Send each whole line once; output accepts bytes like a terminal display."""
    for command in commands:
        output(f"$ {command.line}\r\n".encode("utf-8"))
        try:
            reply = await checked(client, Execute(argv=[command.line]))
        except asyncio.CancelledError:
            raise
        except Exception as error:
            raise CommandError(command, str(error)) from error
        if reply.o:
            output(reply.o.encode("utf-8"))
            if not reply.o.endswith(("\r", "\n")):
                output(b"\r\n")
        if reply.truncated:
            output(b"[output truncated]\r\n")
        if reply.ret:
            raise CommandError(command, f"Shell command failed (ret={reply.ret})")
