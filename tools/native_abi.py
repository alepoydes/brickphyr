# SPDX-License-Identifier: Apache-2.0
"""Bind experimental images to the firmware's configuration and syscall ABI."""

import hashlib
import sys
from pathlib import Path


def main():
    output, *inputs = map(Path, sys.argv[1:])
    identity = hashlib.sha256()
    for path in inputs:
        data = path.read_bytes()
        identity.update(len(data).to_bytes(8, "little"))
        identity.update(data)
    content = f'#pragma once\n#define BRICKPHYR_ABI "{identity.hexdigest()}"\n'
    if not output.exists() or output.read_text() != content:
        output.write_text(content)


if __name__ == "__main__":
    main()
