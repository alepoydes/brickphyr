# Host connections and interfaces

The experimental F4 image implements selectable USB CDC ACM interfaces: Zephyr
Shell and a minimal Pybricks identification endpoint. The stream handler is
checked with Zephyr's UART emulator and the upstream Pybricks Python client.
The Shell/Pybricks round trip is verified over Linux USB on one Robot Inventor F4
(PCB revision unknown), including busy rejection, terminal editing/history,
partial-input isolation and uninterrupted uptime. Bluetooth, SMP, application loading,
and Python/WASM execution remain outside this implementation.

## One management session

Allow one controlling computer session per hub across USB and Bluetooth,
including diagnostics and application transfers. An active USB owner excludes a
second Bluetooth management owner, and conversely. Tools share the current owner
or receive a busy result; another client cannot silently take over the session.
USB power alone does not acquire ownership.

The selected interface determines how requests are interpreted for that session.
Malformed input is an error, not a reason to guess another protocol. Switching
interfaces must preserve application state and the shared settings model. Normal
host disconnection does not stop an autonomous application. Session ownership
also governs [inactivity shutdown](settings.md#inactivity-shutdown).

Input-device connections are governed separately by [Bluetooth](bluetooth.md) and
[applications](applications.md#host-and-input-devices); they are not additional
computer management clients.

## Interface roles

| Interface | Purpose |
| --- | --- |
| Pybricks-compatible | Existing clients discover the hub and use supported Python upload, start/stop, status, and input/output operations |
| Brickphyr management | Zephyr Shell for interactive settings and diagnostics; standard management mechanisms for automation and additional application formats |

Both interfaces use the same firmware services and application supervisor.
Choosing an interface does not replace the operating system or select an execution
engine. A terminal must remain useful independently of Python, native, or WASM
execution. Its application commands use the same lifecycle operations as other
clients; they do not bypass ownership or admission checks.

Prefer SMP/MCUmgr for extended management if its existing operations and clients
fit the actual transfer and settings requirements. Reuse the standard mechanisms
before adding a small application-management command group. Firmware image update
support is not a loader for native or WASM applications. Do not create another
framing or file-transfer protocol merely to rename existing functionality.

Shell and SMP can share a serial channel through Zephyr's existing SMP transport.
This is not automatic multiplexing with the Pybricks USB protocol. Over Bluetooth,
GATT characteristic addresses distinguish services without guessing payloads;
that does not relax the single-management-session rule.

## USB selection experiment

The required outcome is computer-controlled interface selection without buttons
or a hub restart. Buttons may offer the same choice locally, but automated tools
must not depend on them. Bootloader recovery remains a separate operation under
[recovery](recovery.md).

The implementation uses one CDC ACM data channel and device-specific control
requests on endpoint 0. Boot selects Pybricks; the selection survives closing the
port and changes only through an explicit request or reboot. No protocol is
guessed from data. USB IDs remain the experimental `2fe3:0004`.

DTR acquires the data session. Closing it releases ownership after the old shell
has finished and pending USB output has drained. Selection while occupied or
closing fails; a control request cannot evict an owner. The operating system must
also enforce exclusive access to the serial port: firmware cannot distinguish
two processes writing through the same host driver. USB reset releases the
session without selecting another interface.

The selector tool keeps DTR low while draining previous output and uses device
recipient control requests without claiming or detaching the CDC interfaces.
Session buffers, partial protocol frames, and the shell parser are reset at
handover. Shell history currently belongs to one terminal session.

### Usage

Close the current terminal or client before selection. From the repository:

```sh
uv run --locked --group flash python -m tools.host status
uv run --locked --group flash python -m tools.host select shell
uv run --locked python -m serial.tools.list_ports -v
uv run --locked python -m serial.tools.miniterm --raw /dev/ttyACM0 115200
```

Use the listed port instead of `/dev/ttyACM0` when different. Run `info` and
`uptime`, then exit with **Ctrl+]**. Switch and identify through the unmodified
client in the selected Pybricks checkout:

```sh
uv run --locked --group flash python -m tools.host select pybricks
uv run --locked --group flash python -m tools.host identify
uv run --locked --group flash python -m tools.host select shell
uv run --locked python -m serial.tools.miniterm --raw /dev/ttyACM0 115200
```

`uptime` must continue across the round trip. After reboot, select Shell again.
For several hubs, pass `--serial SERIAL_NUMBER` before the subcommand. Linux USB
permissions must allow device control requests as well as opening the serial port.
On a Linux desktop using systemd-logind, install the scoped udev rule once:

```sh
sudo install -m 644 tools/70-brickphyr.rules /etc/udev/rules.d/70-brickphyr.rules
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=usb --attr-match=manufacturer=Brickphyr
```

The rule grants USB access to the active local user and matches the Brickphyr
manufacturer string as well as the development IDs. It does not replace the CDC
driver or change access for other Zephyr devices. The serial port still needs its
normal access permissions. Headless installations need an administrator's device
access policy. Windows/macOS driver access and unchanged Pybricks Code discovery
are unverified.

### Implemented Pybricks subset

Reuse Pybricks' COBS codec, protocol constants, PnP and capability encoders.
The upstream `tools/flash/serdev/pybricks.py` client supplies host reads.

- Read device name, actual Brickphyr firmware version, protocol version, factory
  hub variant and hub kind. Unknown characteristics return an empty value.
- Capabilities advertise no execution engines, program storage or program slots.
- Subscribe emits current USB-connected status with no running program.
- Commands, including upload, run, STOP and DFU, return `INVALID_COMMAND` with the
  original correlation tag. Use the working Shell `reboot dfu` for update entry.
- Malformed envelopes and oversized frames are discarded through the next frame
  delimiter. They never select another protocol or execute shell text.

This is identification interoperability, not full Pybricks Code or Python
compatibility. The returned firmware version does not impersonate Pybricks.

### Experimental control requests

Both requests use device recipient, vendor request `0x42`, and `wIndex = 0`.
GET (`bmRequestType = 0xc0`, `wValue = 0`, length 8) returns format version 1,
mode (0 Pybricks, 1 Shell), occupied flag, selectable flag, then a signed
little-endian 32-bit transport error. SET (`0x40`, length 0) takes the mode in
`wValue`. Unsupported values and occupied transitions stall the request; GET
reports the current state without taking ownership. Errors retain the first
transport failure and require a reboot before retrying.

The mechanism remains experimental beyond the verified Linux/Inventor F4 setup.
It requires the dependency changes
listed in [upstream integration](upstream.md#usb-experiment-prerequisites).

## References

- [Pybricks compatibility](compatibility.md) owns the Python and client contract.
- [Zephyr USB API](https://docs.zephyrproject.org/latest/doxygen/html/group__usbd__api.html)
  supplies control-request handlers.
- [SMP transport](https://docs.zephyrproject.org/latest/services/device_mgmt/smp_transport.html)
  defines Bluetooth and serial framing.
- [MCUmgr handlers](https://docs.zephyrproject.org/latest/services/device_mgmt/mcumgr_handlers.html)
  describes extension groups for operations absent from the standard set.
