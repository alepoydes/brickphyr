# SPDX-License-Identifier: Apache-2.0
"""Hub display and background lighting through the Pybricks API."""

from pybricks.hubs import PrimeHub
from pybricks.parameters import Color, Icon
from pybricks.tools import wait

hub = PrimeHub()
hub.light.blink(Color.BLUE, [100, 100, 300, 100])
hub.display.char("A")
wait(500)
pressed = len(hub.buttons.pressed())
hub.display.number(pressed)
print("Buttons:", pressed)
wait(500)
hub.display.icon(Icon.HEART)
hub.light.animate([Color.RED, Color.BLUE], interval=100)
wait(500)
print("Display complete")
