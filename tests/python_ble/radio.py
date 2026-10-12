# SPDX-License-Identifier: Apache-2.0
from pybricks.messaging import BLERadio
from pybricks.hubs import PrimeHub

radio = BLERadio(observe_channels=(7, 8))
assert radio.observe(7) is None
assert radio.signal_strength(7) == -128
print("radio ready")
while True:
    action = input()
    if action in ("fresh", "refresh"):
        assert radio.observe(7) == (True, False, -300, 1.5, "go", b"\0")
        assert radio.signal_strength(7) == (-60 if action == "fresh" else -40)
        assert radio.observe(8) == (() if action == "fresh" else None)
    elif action == "bad":
        try:
            radio.observe(7)
            assert False
        except RuntimeError:
            pass
    elif action == "expired":
        assert radio.observe(7) is None
        assert radio.signal_strength(7) == -128
    elif action == "reopen":
        old = radio
        old.close()
        hub = PrimeHub(observe_channels=(7,))
        radio = hub.ble
        old.close()
        assert radio.observe(7) is None
    print(action + " done")
