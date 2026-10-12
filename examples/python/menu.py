# SPDX-License-Identifier: Apache-2.0
"""Choose with left/right, confirm and release center; host Stop still works."""

from pybricks.tools import hub_menu

selected = hub_menu("A", "B", "C")
print("Selected:", selected)
