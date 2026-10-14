# SPDX-License-Identifier: Apache-2.0
import gc
import struct
import brickphyr

with open("control_loop.elf", "rb") as image:
    brickphyr.control_start(image.read(), struct.pack("<I", 3))

print("Native observation started; enter A/B port mask 0..3, or q to stop")
while True:
    command = input()
    if command == "q":
        break
    try:
        mask = int(command)
        if not 0 <= mask <= 3:
            raise ValueError
        revision = brickphyr.control_update(struct.pack("<I", mask))
        gc.collect()
        print("Queued revision", revision, brickphyr.control_status())
    except ValueError:
        print("Use 0, 1 (A), 2 (B), 3 (A+B), or q")
