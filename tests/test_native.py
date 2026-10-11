# SPDX-License-Identifier: Apache-2.0
"""Run the production loader and user threads on an ARM MPU model."""

import os
import selectors
import signal
import subprocess
import time
from pathlib import Path


def test_native_lifecycle():
    build = Path(__file__).resolve().parents[1] / "build/native-app-tests"
    cache = subprocess.check_output(["cmake", "-LA", "-N", str(build)], text=True)
    qemu = next(
        line.split("=", 1)[1] for line in cache.splitlines() if line.startswith("QEMU:FILEPATH=")
    )
    with subprocess.Popen(
        [
            qemu,
            "-machine",
            "mps2-an385",
            "-cpu",
            "cortex-m3",
            "-nographic",
            "-monitor",
            "none",
            "-kernel",
            str(build / "zephyr/zephyr.elf"),
        ],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    ) as process:
        try:
            output = bytearray()
            deadline = time.monotonic() + 10
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ)
                while b"NATIVE PASS:" not in output:
                    remaining = deadline - time.monotonic()
                    assert remaining > 0 and selector.select(remaining), output.decode()
                    chunk = os.read(process.stdout.fileno(), 4096)
                    assert chunk, output.decode()
                    output.extend(chunk)
                    assert b"NATIVE FAIL" not in output, output.decode()
        finally:
            os.killpg(process.pid, signal.SIGTERM)
            process.wait(timeout=3)
