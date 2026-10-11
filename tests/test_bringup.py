# SPDX-License-Identifier: Apache-2.0
"""Exercise built firmware and its image boundary, without accessing a hub."""

import os
import re
import selectors
import subprocess
import time
from pathlib import Path
from struct import pack_into

import pytest
import serial

from tools.check_image import check_image

BUILD = Path(__file__).resolve().parents[1] / "build"


def test_console():
    executable = BUILD / "native/zephyr/zephyr.exe"
    with subprocess.Popen(
        [executable],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
    ) as process:
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)

            def console_port():
                output = bytearray()
                deadline = time.monotonic() + 3
                while not (match := re.search(rb"pseudotty: (\S+)\n", output)):
                    remaining = deadline - time.monotonic()
                    assert remaining > 0 and selector.select(remaining), output.decode()
                    chunk = os.read(process.stdout.fileno(), 4096)
                    assert chunk, output.decode()
                    output.extend(chunk)
                return match[1].decode()

            def response(console, command, keys=None):
                console.write((command.encode() if keys is None else keys) + b"\n")
                echo = command.encode() + b"\r\n" if keys is None else b"\r\n"
                output = console.read_until(echo)
                assert output.endswith(echo), output.decode()
                output = console.read_until(b"brickphyr> ")
                assert output.endswith(b"brickphyr> "), output.decode()
                return output.decode()

            def uptime(console):
                return int(re.search(r"Uptime: (\d+) ms", response(console, "uptime"))[1])

            try:
                port = console_port()
                with serial.Serial(port, timeout=3, write_timeout=3, exclusive=True) as console:
                    assert "uptime" in response(console, "help")
                    info = response(console, "info")
                    assert "Brickphyr " in info and "Zephyr " in info
                    assert "Target native_sim/native/64" in info
                    # Execute the edited line, not just check echoed characters.
                    for keys in (
                        b"infX\x08o",
                        b"infX\x7fo",
                        b"ino\x1b[Df",
                        b"inXfo\x1b[H\x1b[C\x1b[C\x1b[3~\x1b[F",
                        b"inf\t",
                        b"discard\x15info",
                    ):
                        assert "Brickphyr " in response(console, "info", keys), keys
                    first = uptime(console)
                    # Up/Up/Down selects the last uptime after visiting info.
                    assert "Uptime:" in response(console, "uptime", b"\x1b[A\x1b[A\x1b[B")
                    assert "Uptime:" not in response(console, "uptime extra")
                    assert "not found" in response(console, "unknown_command")
                    assert "Usage:" in response(console, "reboot invalid")
                    assert "not supported" in response(console, "reboot dfu")
                time.sleep(0.2)
                with serial.Serial(port, timeout=3, write_timeout=3, exclusive=True) as console:
                    before_reboot = uptime(console)
                    assert before_reboot >= first + 200
                    console.write(b"reboot\n")
                    port = console_port()
                with serial.Serial(port, timeout=3, write_timeout=3, exclusive=True) as console:
                    assert uptime(console) < before_reboot
                    assert "Brickphyr " in response(console, "info")
            finally:
                process.terminate()
                process.wait(timeout=3)


def test_image():
    elf = BUILD / "prime/zephyr/zephyr.elf"
    image = (BUILD / "prime/zephyr/zephyr.bin").read_bytes()
    check_image(elf, image)

    corrupt = bytearray(image)
    pack_into("<I", corrupt, 4, 0x08008001)  # A reset into the preserved loader.
    with pytest.raises(ValueError, match="Reset vector"):
        check_image(elf, corrupt)
    with pytest.raises(ValueError, match="extent|differs"):
        check_image(elf, image[:-1])
    corrupt = bytearray(image)
    corrupt[0x264] = 0xFF
    with pytest.raises(ValueError, match="boot info"):
        check_image(elf, corrupt)


@pytest.mark.parametrize(
    "suite",
    ["hub-tests", "poweroff-tests", "radio-tests", "imu-tests", "imu-failure-tests", "sound-tests"],
)
def test_hub_drivers(suite, tmp_path):
    result = subprocess.run(
        [BUILD / suite / "zephyr/zephyr.exe"],
        cwd=tmp_path,
        capture_output=True,
        text=True,
        timeout=5,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert "PROJECT EXECUTION SUCCESSFUL" in result.stdout, result.stdout


def test_host_transport():
    result = subprocess.run(
        [BUILD / "host-tests/zephyr/zephyr.exe"],
        capture_output=True,
        text=True,
        timeout=5,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert "PROJECT EXECUTION SUCCESSFUL" in result.stdout, result.stdout
