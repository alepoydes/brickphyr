# SPDX-License-Identifier: Apache-2.0

from pathlib import Path

import pytest
from test_native import native_console


@pytest.fixture(scope="module")
def python_console():
    # Library cases need fresh applications, not a fresh board for each script.
    build = Path(__file__).resolve().parents[1] / "build/python-smp-tests"
    with native_console(build) as port:
        yield port
