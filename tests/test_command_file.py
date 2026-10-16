# SPDX-License-Identifier: Apache-2.0
"""Command files use the standard Shell result code on one owned client."""

import asyncio

import pytest

from brickphyrdev import command_file


class Client:
    def __init__(self, replies):
        self.replies = iter(replies)
        self.lines = []

    async def request(self, request):
        self.lines.append(request.argv)
        result = next(self.replies)
        if isinstance(result, Exception):
            raise result
        return request._Response.loads(bytes(request._Response(**result)))


def test_quotes_and_successful_truncated_output_continue(tmp_path):
    path = tmp_path / "commands.txt"
    path.write_bytes('  # comment\r\n\nmotor group 100 A="coast\u2028now"\r\nstatus\n'.encode())
    commands = command_file.load_commands(path)
    client = Client([{"o": "part", "ret": 0, "truncated": True},
                     {"o": "ready\n", "ret": 0}])
    display = []
    asyncio.run(command_file.execute(client, commands, display.append))
    assert client.lines == [['motor group 100 A="coast\u2028now"'], ["status"]]
    assert b"".join(display) == (
        b'$ motor group 100 A="coast\xe2\x80\xa8now"\r\npart\r\n[output truncated]\r\n'
        b'$ status\r\nready\n'
    )


def test_command_failure_reports_source_line_and_stops(tmp_path):
    path = tmp_path / "commands.txt"
    path.write_text("# first\nstatus\n\nbad command\nnever run\n", encoding="utf-8")
    client = Client([{"o": "ok", "ret": 0}, {"o": "error", "ret": -22,
                                               "truncated": True}])
    display = []
    with pytest.raises(command_file.CommandError, match=r"commands\.txt:4:.*ret=-22"):
        asyncio.run(command_file.execute(client, command_file.load_commands(path),
                                         display.append))
    assert client.lines == [["status"], ["bad command"]]
    assert b"[output truncated]\r\n" in b"".join(display)


def test_disconnect_does_not_retry_ambiguous_command(tmp_path):
    path = tmp_path / "commands.txt"
    path.write_text("first\nsecond\n", encoding="utf-8")
    client = Client([ConnectionError("link lost")])
    with pytest.raises(command_file.CommandError, match=r"commands\.txt:1: link lost"):
        asyncio.run(command_file.execute(client, command_file.load_commands(path),
                                         lambda _data: None))
    assert client.lines == [["first"]]


@pytest.mark.parametrize("raw,number", [(b"ok\n\xff", 2), (b"# comment\0\n", 1)])
def test_file_validation_precedes_execution(tmp_path, raw, number):
    path = tmp_path / "commands.txt"
    path.write_bytes(raw)
    with pytest.raises(ValueError, match=rf"commands\.txt:{number}:"):
        command_file.load_commands(path)
