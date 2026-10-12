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
the same USB/BLE ownership rule. The optional native profile adds RAM and stored
native applications; the Python profile adds temporary and installed programs with imports
through those management operations. That profile also has an experimental
[Pybricks execution adapter](#implemented-pybricks-subset); WASM remains unimplemented.

## One management session

Allow one controlling computer session per hub across USB and Bluetooth,
including diagnostics and application transfers. An active USB owner excludes a
second Bluetooth management owner, and conversely. Tools share the current owner
or receive a busy result; another client cannot silently take over the session.
USB power alone does not acquire ownership.

Local transport closure does not acknowledge release by the firmware. While it
finishes accepted operations or receives the BLE disconnection event, a new
admission can still return busy. The USB selector reports ownership and readiness
without acquiring a session.

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

The experimental [native application operations](applications.md#host-and-input-devices)
use SMP with equivalent USB/BLE support and the same lifecycle handlers as Shell.
Their [implementation milestones](builds.md#native-application-milestone) include
temporary RAM runs and persistent native installation, both implemented.

## Native applications

Build the native profile and [its matching SDK](builds.md#experimental-native-sdk)
before using these commands. USB is the default; it selects the Shell/SMP interface
automatically. Close another terminal or client first.

```sh
uv run brickphyrdev app run build/native-examples/hello.elf
uv run brickphyrdev app status
uv run brickphyrdev app output
uv run brickphyrdev app --ble Brickphyr run build/native-examples/counter.elf
uv run brickphyrdev app --ble Brickphyr output
uv run brickphyrdev app stop
```

Closing the command's connection leaves an autonomous application running. Status
and stop may use either transport, regardless of the upload transport. Output
drains one bounded batch; unread overflow is counted in status. No command above
writes application flash.

To upload without starting, use `app upload FILE`; then `app start` or the hub
terminal's `app run`. A complete validated upload survives connection closure.
Use `app discard` to free it. An interrupted upload is discarded automatically and
does not stop an existing application. The hub rejects malformed or incompatible
images; RAM exhaustion never silently switches to persistent storage.

To keep several applications across power cycles:

```sh
uv run brickphyrdev app install build/native-examples/hello.elf
uv run brickphyrdev app --ble Brickphyr install build/native-examples/counter.elf
uv run brickphyrdev app list
uv run brickphyrdev app start hello
uv run brickphyrdev app output
uv run brickphyrdev app --ble Brickphyr start counter
uv run brickphyrdev app stop
uv run brickphyrdev app remove counter
```

`install FILE --name NAME` overrides the filename stem. Installation never starts
an application; `start NAME` selects an installed file, while `start` without a
name uses the waiting RAM upload. In either hub terminal, use `app list`,
`app run NAME`, `app output`, `app stop` and `app remove NAME`. Firmware/SDK ABI
changes may require rebuilding stored applications. See
[storage semantics](applications.md#installed-native-files).

`--serial NUMBER` chooses among USB hubs; `--port PATH` selects an explicit serial
port for an already selected Shell interface or a simulator. `--ble` accepts a
Bluetooth name or address. The CLI reuses `smpclient` for both transports.

The experimental application management group is SMP group 65, API version 1:

| Command ID | Operation | Fields/result |
| --- | --- | --- |
| 0 | Read status | ABI, state/result/fault/cleanup, memory/output counts, upload state, image limit, active `format`, supported `formats` bitmask and `repl` flag |
| 1 | Write upload fragment | `off`, `total`, `data`, optional `format` (default native); returns accepted `off` |
| 2 | Write start | Start the completed RAM image |
| 3 | Write stop | Confirm application termination and release |
| 4 | Read output | At most 128 bytes in `data` |
| 4 | Write Python output attachment | Required boolean `attached`: enable interruptible output backpressure, or detach and resume counted nonblocking logging |
| 5 | Write discard | Discard the waiting RAM upload |
| 6 | Read installed files | `name`: lexical cursor (empty for first); returns next `name`, `size`, `available` bytes; empty name ends iteration |
| 7 | Write publish | `name`, `size`: validate and atomically publish `/apps/.incoming` |
| 8 | Write remove | `name`: remove an installed image |
| 9 | Write stored start | `name`: start an installed image |
| 10 | Write Python input | `data`: at most 128 bytes; accepted atomically or rejected with `EAGAIN` when full |
| 11 | Write Python interrupt | Discard queued input and request `KeyboardInterrupt`; forced termination remains command 3 |
| 12 | Write REPL entry | Start fresh interactive Python or reattach to the existing REPL; reject another running application with `EBUSY` |

Persistent file upload/download/status/close use standard SMP group 8. Only
the application operations missing from that group are added above.

Offset zero starts a new RAM upload. Later fragments must be contiguous or exact
repeats of already accepted bytes, with the same format throughout. Format values
are native ELF (0), Python source (1), MicroPython bytecode/native code (2) and a
[Python file map](applications.md#named-python-programs) (3); the corresponding
bit in `formats` reports admission support. Older servers omit these fields and
support only native ELF. Group errors carry the positive Zephyr errno;
malformed requests use standard SMP errors. This group adds application operations,
not another framing or firmware-update protocol. The versioned definitions are
in [the firmware handler](../src/application_mgmt.c) and
[the client](../brickphyrdev/application.py).

Shell and SMP can share a serial channel through Zephyr's existing SMP transport.
This is not automatic multiplexing with the Pybricks USB protocol. Over Bluetooth,
GATT characteristic addresses distinguish services without guessing payloads;
that does not relax the single-management-session rule.

## Python applications

Use the [Python firmware profile](builds.md#experimental-python-profile). The same
application commands select `.py` source or compatible `.mpy` bytecode/native code
by extension. A directory containing `main.py` or `main.mpy` includes all its
regular files; use a directory dedicated to the application. Symbolic links are
rejected. A `.cbor` file supplies an already encoded file map. Use
`--format native|python|mpy|files` after `run` or `upload` to override inference.

```sh
uv run brickphyrdev app run examples/python/hello.py
uv run brickphyrdev app status
uv run brickphyrdev app output
uv run brickphyrdev app --ble Brickphyr run examples/python/hello.py
uv run brickphyrdev app --ble Brickphyr output
uv run brickphyrdev app --ble Brickphyr stop
```

`run` starts the program and returns its current status; it does not wait for
completion or stream its output. `upload` transfers without starting. After an
upload, `app run`, `app status`, `app output` and `app stop` in the hub Shell use
that same program and supervisor. These operations do not write application flash.
For a temporary program with local imports, pass its directory. Install explicitly
to retain it across reboot; installation does not run the program:

```sh
uv run brickphyrdev app run examples/python/modules
uv run brickphyrdev app output
uv run brickphyrdev app install examples/python/modules --name fibonacci
uv run brickphyrdev app list
uv run brickphyrdev app start fibonacci
uv run brickphyrdev app output
uv run brickphyrdev app --ble Brickphyr start fibonacci
uv run brickphyrdev app --ble Brickphyr output
uv run brickphyrdev app remove fibonacci
```

A single source or MPY file can also be installed directly. The hub terminal uses
the same stored-program operations:

```text
app list
app run fibonacci
app output
app remove fibonacci
```

For a program waiting for input, send a line through the same USB/BLE commands:

```sh
uv run brickphyrdev app run examples/python/input.py
uv run brickphyrdev app input Brickphyr
uv run brickphyrdev app output
```

`input TEXT` appends a newline; `--no-newline` sends only the text. One command is
limited to 128 UTF-8 bytes including that newline. Input already accepted belongs
to the running application and survives the sender closing its connection.
`app interrupt` requests a catchable Python `KeyboardInterrupt`; `app stop`
forcibly terminates the application, including native code that never checks
Python events. The hub Shell provides `app input "some text"` and `app interrupt`
through the same handlers. These commands supply script input. Pybricks bindings
are still pending.

Programs can read their bundled resources with `open("data/file", "rb")`.
Paths stay within that application's retained file map, including after an update
or removal of its installed image. Writable Python files are not implemented.
See [streams and resource limits](applications.md#python-input-and-output).

For the experimental Python REPL, use the same terminal frontend:

```sh
uv run brickphyrdev terminal --usb --repl
uv run brickphyrdev terminal --repl
uv run brickphyrdev terminal --usb --repl --monitor --source application
```

Run one command at a time; the second selects BLE. Ctrl-C interrupts, Ctrl-D at an
empty prompt resets Python, Ctrl-X stops REPL and returns to Shell, and Ctrl-]
detaches without stopping Python. Repeating the command reattaches to its existing
state. Entry while another application runs fails without stopping that program.
The hub must run a Python build with SMP command 12; older builds reject it.
These CLI workflows have ARM QEMU/PTY coverage; physical REPL acceptance remains
pending. See [REPL behavior and limits](applications.md#interactive-python).

From an ordinary terminal opened with `uv run brickphyrdev terminal` (BLE) or
`uv run brickphyrdev terminal --usb`, enter:

```text
python
```

This uses the same VM with a plain input/output stream; Ctrl-X returns to Shell.
For a connection sharing SMP operations such as monitoring, select `--repl`
when opening the terminal instead. The hub rejects plain REPL entry once SMP
is active on that connection. Plain Shell entry and reconnection have ARM QEMU
coverage; their physical USB/BLE acceptance is pending too.

## Host CLI

`brickphyrdev` provides USB/BLE terminals, a text telemetry monitor, application
management and USB interface commands.
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

`uv run brickphyrdev terminal --usb` selects and opens the USB Shell. The existing
`uv run brickphyrdev terminal` command still opens BLE. Add `--monitor` to either
for telemetry within that connection, or run `uv run brickphyrdev monitor` for a
standalone USB monitor. [Monitoring and JSONL recording](telemetry.md#text-monitor-and-recording)
describes source selection, rates, logging and the view-switch shortcut.

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
When transmission slows down, the client pauses reading local input while its
bounded queue is full and resumes as space becomes available. Ctrl+] interrupts
an outstanding send and discards unsent input instead of waiting for the queue
to drain. This behavior is shared by USB and BLE.

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
The combined terminal/monitor instead carries standard serial SMP framing inside
that same native stream; the client separates responses from human Shell output.
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
- Without the Python profile, capabilities advertise no execution engines,
  program storage or slots. Subscription reports the connection; application
  commands return `INVALID_COMMAND` with the original correlation tag.
- Malformed envelopes and oversized frames are discarded through the next frame
  delimiter. They never select another protocol or execute shell text.

The Python profile additionally accepts multi-module format-6 bytecode uploads,
start/stop, REPL and stdin. It reports connection/running status and stdout through
the same command/event handlers over USB and BLE. A new start while an application
runs returns busy; stop uses the common supervisor. In the F4 Python profile,
completing a Pybricks download installs it in the selected persistent slot.
Incomplete transfers are discarded without replacing the installed program.
Builds with `CONFIG_BRICKPHYR_PYBRICKS_SLOTS=0` retain the RAM-only adapter:
complete uploads survive disconnection until replacement or reboot. Native
Brickphyr temporary uploads stay in RAM in both configurations.

The reference client's module records are normalized to the common
[Python file map](applications.md#named-python-programs), retaining one importer
and application lifecycle. The advertised transfer limit is 31,728 bytes, leaving
room for file-map metadata within the 32 KiB application limit. The Cortex-M Python
profile also advertises native `.mpy` ABI 6.3. The
[pybricksdev integration](upstream.md#python-client-prerequisites) selects the
matching compiler and packages imported `.mpy` files. Native/Viper decorators,
an imported Viper module and a dynamic C module pass through the actual serial
client on protected ARM emulation, including GC and repeated execution.
Incompatible module headers fail before the client starts a replacement transfer;
the hub independently validates the received modules.

The actual pybricksdev serial client passes upload, imports, input/output, stop,
repeat and reconnect checks against ARM-emulated firmware. A separate protected
ARM fixture covers the production BLE GATT adapter, ownership and interpreter
with a synthetic controller, including notification backpressure and disconnect.
Neither check establishes physical USB/BLE execution or unchanged Pybricks Code
execution. Pybricks library bindings remain unfinished. Persistent slots additionally
have ARM checks for selection, transfer failure, storage exhaustion and coexistence
with named programs. Physical slot persistence and operation remain unverified.
Subscribed output applies backpressure through the common
[application queue](applications.md#python-input-and-output). Python waits
interruptibly when it fills; USB continues receiving a STOP command behind a
pending output event. BLE retries its retained notification before consuming
another fragment. Detach or selection of native BLE management restores the
nonblocking application log, whose overflow counter the Pybricks client does not
expose. Long-output and blocked-stop cases have ARM checks; physical flow-control
acceptance remains open.
DFU through this protocol is still unsupported; use Shell `reboot dfu`.

The returned firmware version does not impersonate Pybricks.
Its build label uses PEP 440 local-version notation (`0.1.1+bringup`) for Python
clients; the terminal retains Zephyr's `0.1.1-bringup` display format.

### Stored Pybricks programs

The F4 Python profile exposes five client slots, numbered 0–4. They are programs
`@0` through `@4` in the same LittleFS store as named applications; there are no
fixed per-slot flash partitions. All programs share the available space and the
common execution-memory limits. The slot count is a build option, not a limit on
the number of ordinary named programs.

Select a slot with left/right outside the connections menu when USB or BLE is in
Pybricks mode, or from the native hub terminal:

```text
pybricks_slot 2
```

After downloading a program through Pybricks, use the native terminal to inspect,
run or remove its stored image:

```text
app list
app run @2
app remove @2
```

`pybricks_slot` without an argument reports the selection. Selection starts at
zero after boot and cannot change during a download or running application.
The reference status event reports the selected slot; upload metadata has no
private slot or save field. A start command with an explicit ID runs that slot;
without an ID it uses the current selection. Selecting an empty slot is allowed;
starting it fails explicitly.

The final metadata command acknowledges only after the validated image has been
written, closed and atomically renamed. Failed replacement leaves the old program
intact. File operations use a separate 4 KiB management worker; the calling
protocol handler waits for its result. Physical response latency and BLE receive
behavior during that wait remain acceptance items. No application is started by
uploading or by rebooting the hub.

Slots can also be started or removed through `brickphyrdev app` using their `@N`
names. Ordinary `app install` cannot overwrite this reserved namespace; download
through Pybricks to replace a slot. The underlying file map and running-version
rules are shared with [named programs](applications.md#named-python-programs).

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

- [Observations](telemetry.md) describes read-only snapshots through Shell and SMP.
- [Pybricks compatibility](compatibility.md) owns the Python and client contract.
- [Zephyr USB API](https://docs.zephyrproject.org/latest/doxygen/html/group__usbd__api.html)
  supplies control-request handlers.
- [SMP transport](https://docs.zephyrproject.org/latest/services/device_mgmt/smp_transport.html)
  defines Bluetooth and serial framing.
- [MCUmgr handlers](https://docs.zephyrproject.org/latest/services/device_mgmt/mcumgr_handlers.html)
  describes extension groups for operations absent from the standard set.
