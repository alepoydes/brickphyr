# SPDX-License-Identifier: Apache-2.0
"""Inspect the hub and use a two-button stop combination for this program."""

from pybricks.hubs import PrimeHub
from pybricks.parameters import Button
from pybricks.tools import wait

hub = PrimeHub()
print(hub.system.info())
hub.system.set_stop_button((Button.LEFT, Button.RIGHT))
try:
    wait(2000)
finally:
    hub.system.set_stop_button(Button.CENTER)
print("system complete")
