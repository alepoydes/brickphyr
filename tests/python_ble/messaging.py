# SPDX-License-Identifier: Apache-2.0
# Synthetic peer exchanges use the public binding and Pybricks wire format.
from pybricks.messaging import AppData
from pybricks.tools import AppData as Alias, run_task

assert AppData is Alias
app = AppData([(9, 2), (1, 4)])
assert app.get_bytes(1) == bytes(4)
assert app.get_bytes(9) == bytes(2)
try:
    AppData([])
    assert False
except OSError:
    pass
print("ready")
while True:
    action = input()
    if action == "read":
        assert app.get_bytes(1) == b"abcd"
        assert app.get_bytes(9) == b"ef"
        assert app.get_bytes(9, 1) == ord("f")
        app.configure(9, 3, b"ok")
        print("read done")
    elif action == "large":
        try:
            app.write_bytes(bytes(range(60)))
        except OSError:
            print("too large")
        else:
            print("large done")
    elif action == "replace":
        async def replace():
            old = app.write_bytes(b"obsolete")
            current = app.write_bytes(b"replacement")
            old.close()
            await current
            cancelled = app.write_bytes(b"cancelled")
            cancelled.close()
            await app.write_bytes(b"final")
        run_task(replace())
        print("replace done")
    elif action == "close":
        old = app
        old.close()
        app = AppData([(2, 1)])
        old.close()
        assert app.get_bytes(2) == b"\0"
        print("close done")
    elif action == "blocked":
        try:
            app.write_bytes(b"blocked")
            assert False
        except OSError:
            pass
        app.close()
        try:
            AppData([])  # Failed announcement must release its lease.
            assert False
        except OSError:
            pass
        print("disconnected")
    elif action == "reopen":
        app = AppData([(7, 0)])
        assert app.get_bytes(7) == b""
        app.write_bytes(b"")
        print("reopened")
