# Host connections and interfaces

The experimental F4 image implements selectable USB CDC ACM interfaces: Zephyr
Shell and a minimal Pybricks identification endpoint. The stream handler is
checked with Zephyr's UART emulator and the Pybricks Python clients. The pinned
pybricksdev CDC extension also identifies the compiled firmware protocol handler
through a pseudoterminal and over Linux USB on the Inventor F4. Identity, status,
reconnection, rejection of unsupported commands, and return to Shell are verified.
The Shell/Pybricks round trip is verified over Linux USB on one Robot Inventor F4
(PCB revision unknown), including busy rejection, terminal editing/history,
partial-input isolation and uninterrupted uptime. Experimental SMP management
shares the Shell channel and passes native simulation and Linux USB checks with
standard clients, including fragmented packets and persistent settings. The optional
[Bluetooth profile](bluetooth.md#experimental-f4-controller) provides Pybricks BLE
identification/status, SMP management, and the native interactive terminal with
the same USB/BLE ownership rule. Application loading
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
engine. One Zephyr Shell instance serves USB and the native BLE stream. Command parsing,
line editing, completion, history, buffers, and command execution are shared; only
byte transport changes. A terminal remains useful independently of Python, native,
or WASM execution. Its application commands use the same lifecycle operations as other
clients; they do not bypass ownership or admission checks.

Prefer SMP/MCUmgr for extended management if its existing operations and clients
fit the actual transfer and settings requirements. Reuse the standard mechanisms
before adding a small application-management command group. Firmware image update
support is not a loader for native or WASM applications. Do not create another
framing or file-transfer protocol merely to rename existing functionality.

The planned [native application operations](applications.md#host-and-input-devices)
use SMP with equivalent USB/BLE support and the same lifecycle handlers as Shell.
Their [implementation milestones](builds.md#native-application-milestone) include
temporary RAM runs and persistent installation; neither is available yet.

Shell and SMP can share a serial channel through Zephyr's existing SMP transport.
This is not automatic multiplexing with the Pybricks USB protocol. Over Bluetooth,
GATT characteristic addresses distinguish services without guessing payloads;
that does not relax the single-management-session rule.

### Host CLI

`brickphyrdev` provides the native Bluetooth terminal and USB interface commands.
After [workspace setup](builds.md#setup-and-board-bring-up), run it from the repository:

```sh
uv run brickphyrdev --help
```

To use the installed command from any directory, activate the environment once:

```sh
source .venv/bin/activate
brickphyrdev terminal
```

Runtime dependencies belong to the package; these commands need no `flash` group.
Ordinary `uv run` uses the locked versions. `--locked` is useful for builds and
validation: it rejects an outdated lockfile instead of updating it. See
[uv locking](https://docs.astral.sh/uv/concepts/projects/sync/).

The CLI currently targets Linux. USB identity and Pybricks-to-Shell handover use
the [pybricksdev CDC extension](upstream.md#usb-experiment-prerequisites) pinned
by west and installed by uv from its sibling checkout. Upstream's released Python
package does not yet provide this transport. The installed CLI itself does not
require west or firmware sources at runtime.

### Native Bluetooth terminal

The shared Shell passes native stream/session tests and Linux BLE checks on the
Inventor F4: first connection after boot, editing/history/completion, long output,
reconnection, and subsequent USB, SMP and Pybricks identification. These checks do
not establish radio-fault recovery or cross-platform terminal support.

Close the USB terminal or other controlling client. On Linux, from the repository:

```sh
uv run brickphyrdev terminal
```

The client discovers `Brickphyr` and connects directly after boot, independently
of the USB mode. Pass a BLE address or a different hub name as the final argument
when needed. Ctrl+] closes the link. Editing, completion and history run on the
same hub Shell as USB; the host forwards terminal bytes without interpreting commands.
The client restores local terminal settings on exit or connection failure.

The native interface contains standard SMP management and a separate raw terminal
stream. Its GATT service is `f3641400-00b0-4240-ba50-05ca45bf8abc`; TX notifications
use `f3641401-00b0-4240-ba50-05ca45bf8abc` and acknowledged RX writes use
`f3641402-00b0-4240-ba50-05ca45bf8abc`. These identifiers keep the native stream
separate from Pybricks' application stdin/stdout. There is no additional packet
format. The name/SMP advertisement discovers the hub; service discovery locates
the terminal characteristics.

Subscribe to TX to open the Shell. RX requires the subscription and accepts each
write in full or returns an ATT error. Closing or disconnecting discards unfinished
input and queued commands; an entered handler finishes before ownership is released.
Terminal history is per session. Reopening after unsubscribe requires a new BLE
connection, excluding notifications already queued for the old terminal.
SMP GATT requests cannot execute concurrently with an open terminal stream.
A stalled notification sender closes its link after three seconds without progress;
output is never silently truncated. USB and BLE use the same settings and power hold.
Commands such as `reboot dfu`, `shutdown`, and `bluetooth off` can close the link
before the final write acknowledgment arrives. Do not automatically retry a command
after a transport error; verify the resulting device state first.

### Experimental SMP management

In Brickphyr mode, Zephyr's SMP serial framing carries machine requests on the
same channel as the human Shell. It uses the same session owner and Settings
handlers. Close the terminal before starting a management client:

```sh
uv run brickphyrdev usb select shell
uv run smpmgr --port /dev/ttyACM0 os echo Brickphyr
```

Use the port reported by `python -m serial.tools.list_ports -v`. The unchanged
`smpmgr` command and `smpclient` library are checked against a native Zephyr build
over a pseudoterminal and Linux USB on the Inventor F4. The optional
[Bluetooth profile](bluetooth.md#experimental-f4-controller) also passes standard
SMP BLE discovery, echo, fragmented packets and Settings read/write/save. It starts
advertising at boot; if stopped explicitly, run `bluetooth on` in the terminal.
Close USB before using:

```sh
uv run smpmgr --ble Brickphyr --timeout 10 os echo Brickphyr
```

For multiple nearby hubs, specify the address reported by BLE discovery instead
of the shared default name.

The enabled groups provide OS echo/reset and Settings read/write/commit/whole-store
save. Read and write operate on RAM; save is explicit. Settings values and storage
limitations are described in [settings](settings.md). Load, delete and named saves
are currently rejected before mutation. Shell RPC, firmware/file transfer and
application loading are not enabled.

SMP executes on the Shell thread, so a response cannot interleave with a Shell
command's output. Closing the session waits for an accepted operation to finish;
it discards queued and partial input and suppresses old replies. An accepted save
may therefore finish after the host disconnects. Reopening cannot inherit an old
command. BLE SMP uses the standard Zephyr GATT transport and management workqueue.
Packet admission holds the same management owner and power inhibition until the
accepted packet finishes, even after disconnection. Incomplete and queued old
packets are discarded; they cannot acquire the next session. A disconnect also
rejects subsequent commands bundled in the current packet, while an already
entered handler may finish. These paths require
the pinned [Zephyr changes](upstream.md#smp-prerequisites).

## USB selection experiment

The required outcome is computer-controlled interface selection without buttons
or a hub restart. Buttons may offer the same choice locally, but automated tools
must not depend on them. Bootloader recovery remains a separate operation under
[recovery](recovery.md).

The implementation uses one CDC ACM data channel and device-specific control
requests on endpoint 0. Boot selects Pybricks; the selection survives closing the
port and changes only through an explicit request or reboot. No protocol is
guessed from data. USB IDs remain the experimental `2fe3:0004`.

DTR acquires the data session after discarding buffered UART input from the
previous port session. Closing it retains the power hold until accepted commands
and Shell teardown finish. USB interface selection additionally waits for pending
USB output to drain; an independent BLE session does not depend on that output.
Selection while occupied or closing fails; a control request cannot evict an owner.
The operating system must also enforce exclusive access to the serial port:
firmware cannot distinguish
two processes writing through the same host driver. USB reset releases the
session without selecting another interface.

The selector tool keeps DTR low while draining previous output and uses device
recipient control requests without claiming or detaching the CDC interfaces.
Session buffers, partial protocol frames, and the shell parser are reset at
handover. Shell history currently belongs to one terminal session.

The owner can also request the next interface through its current protocol.
Shell command `host_mode pybricks` requests compatibility mode. Brickphyr's
Pybricks command extension `0x80` (EXIT_COMPATIBILITY, no payload) requests Shell;
it replies with the original correlation tag and the standard success result.
Extra payload receives VALUE_NOT_ALLOWED. Other unsupported commands still fail.
Read the acknowledgment, then close the port: only that session's release applies
the selection. Until then, the existing parser remains active. Opening the next
session uses the new interface without restarting the hub. This extension is
specific to Brickphyr and must not be sent to an unidentified upstream hub.

### Usage

Close the current terminal or client before selection. From the repository:

```sh
uv run brickphyrdev usb status
uv run brickphyrdev usb select shell
uv run python -m serial.tools.list_ports -v
uv run python -m serial.tools.miniterm --raw /dev/ttyACM0 115200
```

Use the listed port instead of `/dev/ttyACM0` when different. Run `info` and
`uptime`, then exit with **Ctrl+]**. Switch and identify through the installed
pybricksdev client:

```sh
uv run brickphyrdev usb select pybricks
uv run brickphyrdev usb identify
uv run brickphyrdev usb select shell
uv run python -m serial.tools.miniterm --raw /dev/ttyACM0 115200
```

`uptime` must continue across the round trip. After reboot, select Shell again.
To exercise the in-band commands instead of selecting through USB control requests:

```sh
uv run brickphyrdev usb handover shell
uv run brickphyrdev usb handover pybricks
```

The tool opens the current interface exclusively, reads its acknowledgment,
closes it, and checks that the requested interface became available.

For several hubs, pass `--serial SERIAL_NUMBER` after `usb` and before its subcommand.
Linux USB permissions must allow device control requests as well as opening the serial port.
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
access policy. Windows/macOS driver access and Pybricks Code discovery over USB
are unverified. Browser discovery over BLE is covered in
[Bluetooth integration](bluetooth.md#pybricks-code).

### Implemented Pybricks subset

Reuse Pybricks' COBS codec, protocol constants, PnP and capability encoders.
The installed pybricksdev client supplies host reads and command framing.

- Read device name, actual Brickphyr firmware version, protocol version, factory
  hub variant and hub kind. Unknown characteristics return an empty value.
- Capabilities advertise no execution engines, program storage or program slots.
- Subscribe emits current USB-connected status with no running program.
- Application commands, including upload, run, STOP and DFU, return `INVALID_COMMAND` with the
  original correlation tag. Use the working Shell `reboot dfu` for update entry.
- Malformed envelopes and oversized frames are discarded through the next frame
  delimiter. They never select another protocol or execute shell text.

This is identification interoperability, not full Pybricks Code or Python
compatibility. The returned firmware version does not impersonate Pybricks.
Its build label uses PEP 440 local-version notation (`0.1.1+bringup`) for Python
clients; the terminal retains Zephyr's `0.1.1-bringup` display format.

### pybricksdev CDC client

The pinned pybricksdev integration provides a CDC transport using the existing
`PybricksHub` command/event implementation. It requires the client changes listed
in [upstream integration](upstream.md#usb-experiment-prerequisites); the released
pybricksdev package does not yet supply this transport. After selecting Pybricks
and closing the terminal, identify the hub without uploading or starting a program:

```sh
uv run --locked python -m pybricksdev usb-info /dev/ttyACM0
```

On the Inventor F4, this transport identifies both Brickphyr and official
Pybricks 4.1.0b6 over Linux USB. The official image also passes STOP/status and
reconnection checks. Program upload and execution through this transport remain
unverified.

The library entry point is `pybricksdev.connections.serial.PybricksHubSerial`.
Use its `connect()`, inherited command methods and status/stdout observables, and
`disconnect()`. The port is explicit; the client does not change Brickphyr's
selected interface. A timeout or canceled exchange closes the session to exclude
ambiguous late replies. A rejected command leaves a healthy connection available.
Disconnection does not stop a user program.

The same transport supports `pybricksdev run usb --port PORT SCRIPT` for hubs that
advertise a supported execution engine. Brickphyr currently advertises none, so
this is not evidence that Python programs can be loaded or run here.

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
