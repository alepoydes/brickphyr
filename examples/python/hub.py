# SPDX-License-Identifier: Apache-2.0
"""Static hub display and lighting through the Pybricks API."""

from pybricks.hubs import PrimeHub
from pybricks.parameters import Color, Icon
from pybricks.tools import wait

hub = PrimeHub()
hub.light.on(Color.BLUE)
hub.display.char("A")
wait(500)
pressed = len(hub.buttons.pressed())
hub.display.number(pressed)
print("Buttons:", pressed)
wait(500)
hub.display.icon(Icon.HEART)
wait(500)
print("Display complete")
