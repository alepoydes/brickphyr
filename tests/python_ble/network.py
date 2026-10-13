# SPDX-License-Identifier: Apache-2.0
from pybricks.messaging import HubNetwork
from pybricks.tools import run_task

A = "01:02:03:04:05:06"
B = "07:08:09:0A:0B:0C"
C = "0D:0E:0F:10:11:12"
LOCAL = "21:22:23:24:25:26"

network = HubNetwork(inbox_size=525)
assert network.address() == LOCAL
print("network ready")

if input() == "pending":
    network.connect(A)
    raise AssertionError("pending connect must be stopped by host")

for address in (A, B, C):
    network.connect(address)
assert all(network.is_connected(address) for address in (A, B, C))

async def send_maximum():
    await network.connect(A)
    await network.send(b"\xff" * 512, address=B)

run_task(send_maximum())
print("network connected")

input()
assert network.inbox(latest=True) == (
    (B, b"B0"),
    (A, b"A1"),
    (C, (42, 1.5, True, None, b"\0", "é")),
)
assert network.inbox() == ()
print("inbox checked")

input()
assert network.inbox() == ((A, b"y" * 400),)
print("overflow checked")

input()
network.send((42, 1.5, True, None, b"\0", "é"), address=C)
print("network sent")

input()
assert network.inbox() == ((A, b"R"),)
print("relay checked")

input()
network.__del__()
network = HubNetwork(inbox_size=525)
assert network.inbox() == ()
assert all(network.is_connected(address) for address in (A, B, C))
print("network reopened")

input()
assert not network.is_connected(C)
assert network.is_connected(A) and network.is_connected(B)
print("network lost")
input()
network.__del__()
print("network released")
input()
