# SPDX-License-Identifier: Apache-2.0
"""Use the installed reference client's public compiler API for the VM fixture."""

import asyncio
import sys
from pathlib import Path

from pybricksdev.compile import compile_file

source, output = map(Path, sys.argv[1:])
output.write_bytes(asyncio.run(compile_file(str(source.parent), source.name, 6)))
