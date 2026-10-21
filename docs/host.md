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

New-session Shell input is retained through startup, including commands sent
before the first prompt. Closing a session discards its remaining input; it
cannot become a command in the next USB or BLE session. The shared Shell worker
stays alive between sessions. After accepted commands finish, session reset
clears history, bypass and terminal settings before admitting the next owner.
Worker reuse has ARM checks for Shell/REPL detach and reattachment. A bounded
Inventor F4 check covers 40 USB reconnections with no A/B UART errors or missed
IMU samples, followed by BLE editing, REPL exit, cross-transport session isolation
and Pybricks USB identification. This does not establish sustained combined-load
behavior.

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
engine. One interactive Zephyr Shell instance serves USB and the native BLE stream. Command parsing,
line editing, completion, history, buffers, and command execution are shared; only
byte transport changes. A terminal remains useful independently of Python, native,
or WASM execution. Its application commands use the same lifecycle operations as other
clients; they do not bypass ownership or admission checks.

Boards with a USB CDC/UART management endpoint enable `BRICKPHYR_HOST_UART`.
BLE-only boards omit that stream's Pybricks framing and dispatcher state, while
retaining the shared Shell and BLE Pybricks endpoint. Enabling UART drivers for
wired devices does not enable a UART management endpoint.

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

For a current heap and thread-stack snapshot, use either transport. The optional
interval streams one JSON object per sample until Ctrl+C; disconnecting does not
stop the application:

```sh
uv run brickphyrdev app memory
uv run brickphyrdev app --ble Brickphyr memory --interval 1
```

The object has an `overview` and a `stacks` list. It includes system-heap
allocated/free bytes and peak since boot, current source/staging, Python arena,
native code and recorder reservations, plus each thread's requested and actual
user stack and untouched bytes. The GC sample arrives on the VM's next poll;
`-1` means unavailable and `gc_age_ms` shows how old an earlier sample is.
The privileged-stack reservation is reported for user-mode threads, but its
watermark is unsupported on ARM. This snapshot does not guarantee that a new
allocation or application replacement will fit fragmented heap memory. See
[memory semantics](applications.md#experimental-native-profile).

Use `app budget` to inspect a completed RAM upload or an installed name before
starting it. It reads the actual image through firmware validation and does not
upload, start, stop, consume the ready upload or write flash. Build and install
the [native composite example](applications.md#native-control-and-background-computation),
then inspect its bundled controller:

```sh
uv run brickphyrdev app budget control_robot --resource control_loop.elf --parameters 4 \
  --record-size 184 --record-count 32 --record-config 4
```

For the [Python control package](applications.md#concurrent-execution-example-and-evidence)
installed as `control`:

```sh
uv run brickphyrdev app budget control --resource control_loop.elf --parameters 4 \
  --record-size 184 --record-count 32 --record-config 4
```

Omit the name to check a ready RAM package. `--stack BYTES` declares a Python stack request;
native ELF stack size comes from its ABI. `--native-code` accounts for the Python
native-code limit, and `--dynamic BYTES` declares other application allocations.
Without both record size and count, recorder storage remains unknown. JSON lists
the primary/controller, declared or unknown recorder/dynamic bytes, current heap
and stack snapshot, current staging,
and separately named load/replacement accounting phases. Native admission is a
conservative bound; neither an accounted amount nor free heap guarantees a fit
or predicts a peak. See [budget semantics](applications.md#experimental-native-profile).

`app run` normally starts the application and closes the connection. To accompany
its execution, use `--follow`; `--record FILE` also collects application cycle
records and implies `--follow`:

```sh
uv run brickphyrdev app run examples/python/hello.py --follow
uv run brickphyrdev app run build/native-examples/control_record.elf --record run.jsonl
uv run brickphyrdev app --ble Brickphyr run build/native-examples/control_record.elf --record ble-run.jsonl
uv run brickphyrdev app start my-program --follow
```

Upload/start, output and recording share one native console/SMP connection over
USB or BLE. The same application launcher and record collector serve the other
CLI commands. Program output goes unchanged to stdout; lifecycle and dropped-output
notices go to stderr. On normal application exit, the client drains available
output and records before closing. A nonzero result, fault or cleanup error fails
the command. `--record` creates a new host file and refuses to overwrite an existing
one before connecting. The application must initialize its recorder; the client
waits for it while showing output. Record gaps and incomplete collection remain
explicit in the existing [recording format](applications.md#application-recording).

**Ctrl+C only detaches; it does not stop the application.** Connection failure
also leaves autonomous execution alone. A cancelled active recording receives a
cancelled footer when the file remains writable. To stop after detaching, use
`brickphyrdev app stop` over the chosen transport. Finite hub buffers cannot
guarantee lossless output or records if the reader or connection is too slow.

Closing the command's connection leaves an autonomous application running. Status
and stop may use either transport, regardless of the upload transport. Output
drains one bounded batch; unread overflow is counted in status. No command above
writes application flash.

To upload without starting, use `app upload FILE`; then `app start` or the hub
terminal's `app run`. A complete validated upload survives connection closure.
Use `app discard` to free it. An interrupted upload is discarded automatically and
does not stop an existing application. The hub rejects malformed or incompatible
images; RAM exhaustion never silently switches to persistent storage.

To assemble one native/Python composite package from explicit inputs, use:

```sh
uv run brickphyrdev app pack --entry build/native-examples/control_background.elf \
  --component build/native-examples/control_loop.elf \
  --sdk build/native-sdk/brickphyr-sdk build/native-examples/control_robot.cbor
```

Repeat `--component PATH` for required native ELF components and
`--resource NAME=PATH` for other bundled files. Native inputs must use the SDK
that built them. This explicit command validates the given artifacts; rebuild
with the SDK CMake package target to track source and component changes.

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

`install FILE --name NAME` overrides the filename stem. Installation stops and
joins the current application, completes cleanup, removes the old named image,
streams the new image through hidden `/apps/.incoming`, validates it, and renames
it into place. After removal, a failed replacement leaves the name absent, with no rollback.
Local validation and stop/cleanup failures precede removal. If the final reply is
lost, the client compares the installed bytes; without readback it reports an
unknown outcome rather than assuming that publication failed.
Installation does not start an application; `start NAME` selects an installed file, while `start` without a
name uses the waiting RAM upload. In either hub terminal, use `app list`,
`app run NAME`, `app output`, `app stop` and `app remove NAME`. Firmware/SDK ABI
changes may require rebuilding stored applications. See
[storage semantics](applications.md#installed-native-files).

Prefix a program name with `_` to keep it out of the hub's button selector while
leaving it available from the CLI and terminal. For example,
`uv run brickphyrdev app install build/native-examples/hello.elf --name _helper`
installs a hidden program. Number visible programs such as `01-start`, `02-next`
to set their browsing order.

For Python, `--stack BYTES` on `app run` or `app start` requests a user stack
before upload/start. The client checks the firmware's advertised minimum and
maximum first; omitting the option uses its default. On the full F4 Python
profile the request is 4096–16384 bytes, default 12288 bytes, and the MPU rounds
that default allocation to 16384 bytes. Native ELF stack size remains its ABI
declaration; a nonzero override is rejected. The hub Shell accepts
`app run NAME STACK` for a stored Python program and `app run --stack BYTES`
for a waiting RAM upload. For example:

```sh
uv run brickphyrdev app run examples/python/hello.py --stack 8192
uv run brickphyrdev app start fibonacci --stack 8192
```

`--serial NUMBER` chooses among USB hubs; `--port PATH` selects an explicit serial
port for an already selected Shell interface or a simulator. `--ble` accepts a
Bluetooth name or address. The CLI reuses `smpclient` for both transports.

### Native application messages

A running native primary may register versioned binary inbox types. The host can
discover them and send a complete block without writing flash or stopping the
application. These commands work over the same USB or BLE SMP connection:

```sh
uv run brickphyrdev app message-types
uv run brickphyrdev app message-send parameters --file controller.bin
uv run brickphyrdev app message-status parameters
uv run brickphyrdev app --ble Brickphyr message-send ping --hex ''
```

`message-send` reports receipt (`pending`), not application acceptance. Use
`message-status TYPE [REQUEST]` to read the outcome. Each registration declares
an ID, name, format version, maximum payload and either `value` or `command`
policy. A newer pending value replaces the older one; commands reject another
request until the primary completes the first. The request number supports
idempotent retries, and a new application run gets a new run ID so old messages
cannot reach its replacement. The hub Shell uses the same handlers with
`app message`, `app message send TYPE HEX`, and `app message status TYPE [ID]`.
States are 0 empty, 1 pending, 2 processing, 3 completed, 4 superseded;
`result` is the application's zero or negative outcome, and `detail` is defined
by that message type.
See the [SDK contract](applications.md#host-to-native-messages).

The experimental application management group is SMP group 65, API version 1:

| Command ID | Operation | Fields/result |
| --- | --- | --- |
| 0 | Read status | ABI, state/result/fault/cleanup, memory/output counts, upload state, image limit, active `format`, supported `formats` bitmask and `repl` flag; `install=1` advertises stop-first installation; `control_stack`/`control_unused` describe an independent controller's stack |
| 1 | Write upload fragment | `off`, `total`, `data`, optional `format` (default native); returns accepted `off` |
| 2 | Write start | Start the completed RAM image; optional `stack` bytes for Python (0 or absent selects firmware default) |
| 3 | Write stop | Confirm application termination and release |
| 4 | Read output | At most 128 bytes in `data` |
| 4 | Write Python output attachment | Required boolean `attached`: enable interruptible output backpressure, or detach and resume counted nonblocking logging |
| 5 | Write discard | Discard the waiting RAM upload |
| 6 | Read installed files | `name`: lexical cursor (empty for first); returns next `name`, `size`, `available` bytes; empty name ends iteration |
| 7 | Write publish | `name`, `size`: close, validate and atomically rename `/apps/.incoming`; reject replacement of a retained running source |
| 8 | Write remove | `name`: remove an installed image |
| 9 | Write stored start | `name`: start an installed image; optional `stack` bytes for Python |
| 10 | Write Python input | `data`: at most 128 bytes; accepted atomically or rejected with `EAGAIN` when full |
| 11 | Write Python interrupt | Discard queued input and request `KeyboardInterrupt`; forced termination remains command 3 |
| 12 | Write REPL entry | Start fresh interactive Python or reattach to the existing REPL; reject another running application with `EBUSY` |
| 13 | Read output and state | At most 256 bytes in `data`, plus `state`, `result`, `fault`, `cleanup`, `dropped`, `queued`, and `repl`; `queued` is the output remaining after this read |
| 14 | Read recording metadata | `generation`, record `size`, retained `capacity`, `first`, `next`, `active`, producer-defined `schema`, `config_size`, nullable `trigger`, `after`, end `reason` (0 open, 1 application end, 2 completed trigger, 3 full, 4 duration, 5 cancelled, 6 capture error), `control`, `parameter_size`, `paused`, `segments`, `compact`, `compact_ack`, and `inline_parameters` |
| 14 | Write clear recording | No arguments; rejects a live application or built-in capture ownership with `EBUSY`; use `capture clear` for the latter |
| 15 | Read recording fragment | Required `generation`, record `index` and byte `off`; returns at most 256 bytes in `data` (at most 224 with control metadata, possibly less with inline parameters), plus current `generation`, `first`, `next`, `active`, `trigger`, `after`, `reason`, binary `step`, and optional applied `parameters` |
| 16 | Read recording configuration | Required `generation`, byte `off`; returns at most 256 bytes in `data`, current `generation` and configuration `total` |
| 16 | Write recording trigger | Required `generation`, `after`; freeze after that many further records without stopping the application |
| 17 | Read applied control parameters | Required `generation`, record `index`; returns current `generation`, `first`, `next`, applied `revision`, and its exact `data` (at most 256 bytes) |
| 18 | Read memory snapshot | `heap_total`, `heap_used`, `heap_free`, `heap_peak`, `source_ram`, `source_flash`, `staging`, `python_arena`, `primary_native`, `control_native`, `code`, `code_used`, `records`, `gc_used`, `gc_free`, `gc_age_ms`, `stack_min`, `stack_max`, `stack_default`, `active` |
| 19 | Read thread stack | Required zero-based `index`; `present=false` ends the list, otherwise returns `name`, `kind`, `requested`, `user`, `unused`, `privileged`, `priv_unused`, `next` |
| 20 | Read image budget | Optional `name` (empty: completed RAM upload), `resource` (controller ELF within a package), `control` (default false), `stack` and `parameters` (default zero); returns integer `format`, `image`, `source_ram`, `source_flash`, `transfer`, `stack`, `user_stack`, `priv_stack`, `arena`, `code_limit`, `native`, `alignment`, `supervisor` |
| 21 | Read recorder budget | `size`, `capacity`, `config`, `control`, `parameters`; returns `bytes` for the firmware recorder allocation excluding allocator metadata |
| 22 | Read recording block | Required `generation`, starting `index` and slot byte `off`; returns consecutive slot bytes in `data` and the current position/capture fields, without a separate `step` field |
| 23 | Write recording identity | Required `generation` and nonzero 16-byte `tag`; bind an untagged capture or retain its existing tag; returns `generation` and the retained `tag` |
| 24 | Acknowledge paused recording | Required `generation` and exclusive `next`; available only for the optional acknowledged paused mode, not segmented telemetry |
| 25 | Read compact control block | Required `generation` and starting `index`; optional `format=4` requests the smaller first-row encoding when possible; returns `rows`, independently decodable binary `data`, current position/capture fields, and optional applied `parameters`; optional `ack` belongs only to acknowledged paused mode |
| 26 | Read native message type | Zero-based `index`; `present=false` ends the list, otherwise `run`, `type`, `name`, `version`, `max_size`, `policy`, `last` |
| 27 | Write native message | Required `run`, `type`, nonzero `request`, binary `data`; returns `state`, `result`, `detail`, `request` |
| 28 | Read native message status | Required `run`, `type`, `request` (zero selects latest); returns the same result fields |
| 29 | Write install prepare | `name`: stop/join/cleanup the current application, remove old named image and stale staging before SMP file upload |

Recording metadata (command 14) advertises the maximum block payload as `block`.
Its absence means only command 15 is available. A block slot consists of the
existing 32-byte little-endian control step when control recording is enabled,
followed by the application's fixed-size payload. Blocks may cross slot and
allocation-block boundaries or end inside a step/payload. For segmented control
records with parameters, command 22 is unavailable: read one record at a time
using command 15. If `inline_parameters` is false, read its parameters with
command 17 before requesting the record; this query does not release it. Compact
blocks stop at a parameter-revision boundary and include applied parameters when
needed. Generation and range checks apply to all record reads; missing data is never
silently replaced. Block copying is bounded and performs no allocation or I/O
while holding the recorder lock.

Compact control blocks retain the eight-byte header: four-byte magic, then
little-endian 16-bit row count and uncompressed row size. Without `format`, or
with `format=0`, the response uses `BCB3`. With `format=4`, the hub may use `BCB4`
when its first row is smaller, otherwise it returns `BCB3`. BCB4 places a
`ceil(field_count/2)`-byte width table after the header, with the first field in
the low nibble. Each non-aliased field contributes that many low-order bytes;
omitted high-order bytes are zero. Aliased fields contribute no bytes and copy
their source field. BCB4 is used only when each non-aliased field fits in 15
bytes and the complete first row is shorter. Subsequent rows use the unchanged
BCB3 change bitmap and exact field/delta encoding. The decoder must accept
either magic and validate field widths and the complete block length. Every
block remains independently decodable, with exact timestamps and values.
Unsupported nonzero format values
are rejected.

Finite captures may advertise `variable=true` (absent means false). Their `size`
is the maximum payload length, and each command-22 record is a uint16
little-endian length followed by exactly that many payload bytes. Length is
1–`size`; there is no padding or control step. `index` still selects a logical
record, while `off` includes its length prefix. Blocks can split that prefix.
Collectors require block support for this mode and retain the exact payload in
the host file. Capacity bounds the record count rather than `size * capacity`
bytes; a finite schedule may finish its full window with unused event slots.
The mode is immutable within a recording and is part of its resume identity.

Command 14 also returns the capture `tag` (16 zero bytes before binding).
Tag and metadata are sampled under the same lock. The tag is immutable until
recorder reset/clear; it does not change the native SDK's recording ABI.
A resuming client must compare its saved tag before requesting records and must
never bind a new tag to make a mismatched capture look resumable.

[Application recording](applications.md#application-recording) uses non-consuming
reads for ordinary rings and finite captures. In segmented mode, encoding a
response retires its complete records, without receipt acknowledgement. Partial
fragments remain retained until the complete record is encoded. A request
starting after `first` within the retained range returns `EINVAL`, so it cannot
discard an unencoded prefix. Empty responses never retire records. Record, block
and compact reads reject a stale generation with `ESTALE`; an index outside
`[first, next)` returns empty data with current bounds. The client reports
replacement or loss instead of merging fragments. An offset outside an available
record returns `EINVAL`. Configuration reads return their current generation with
empty data on a mismatch. Triggers reject a stale
generation; the trigger boundary is the exclusive `next` index at acceptance.

In control mode, each available data fragment carries the same 32-byte `step`:
little-endian signed 64-bit scheduled/start microseconds followed by unsigned
32-bit sequence, skipped releases, parameter revision and parameter size.
Other responses use an empty step. Parameter reads bind values to a retained
record; an overwritten or replaced record returns empty data with current bounds.
An in-range revision zero with size zero is a valid standalone empty parameter
block. A retained ordinary record has no control parameters and returns `ENOTSUP`.
The collector must validate generation and bounds before combining fragments,
step metadata and parameter bytes.

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
in [the firmware handler](../src/application_mgmt.c),
[the application client](../brickphyrdev/application.py) and
[the budget client](../brickphyrdev/budget.py).

Shell and SMP can share a serial channel through Zephyr's existing SMP transport.
SMP output wakes the shared stream when a complete packet has been encoded or
its existing transmit buffer fills. Serial lines and Base64 fragments do not
each start a transfer; short replies still flush without waiting for another
request. The UART adapter offers the available contiguous bytes to its driver,
which determines how much fits in its FIFO; USB packet boundaries do not limit
that copy. The standard SMP codec and wire format are unchanged.
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
through the same handlers. These commands supply script input. See the current
[Pybricks library coverage](compatibility.md#implemented-library-subset).

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
The hub must run a Python build with SMP commands 12 and 13; older builds reject
this CLI workflow. Command 4 remains available for 128-byte output reads.
These CLI workflows have ARM QEMU/PTY coverage. The shared SMP REPL operations
also pass editing, history/completion, interruption, soft reset, display cleanup
and raw-protocol checks over USB/BLE on the Inventor F4. See
[REPL behavior and limits](applications.md#interactive-python).

From an ordinary terminal opened with `uv run brickphyrdev terminal` (BLE) or
`uv run brickphyrdev terminal --usb`, enter:

```text
python
```

This uses the same VM with a plain input/output stream; Ctrl-X returns to Shell.
For a connection sharing SMP operations such as monitoring, select `--repl`
when opening the terminal instead. The hub rejects plain REPL entry once SMP
is active on that connection. Plain Shell entry, editing, soft reset, raw REPL,
return and reconnection also pass on the Inventor F4 over USB/BLE. Reconnection
preserves variables and cancels an unfinished line.

## Host CLI

`brickphyrdev` provides USB/BLE terminals, a text telemetry monitor, application
management, USB interface commands and [F4 firmware update over USB](builds.md#firmware-update-over-usb).
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
package does not yet provide this transport. Most CLI commands need neither west
nor firmware sources at runtime; `flash` uses the Pybricks source checkout.

`uv run brickphyrdev terminal --usb` selects and opens the USB Shell. The existing
`uv run brickphyrdev terminal` command still opens BLE. Add `--monitor` to either
for telemetry within that connection, or run `uv run brickphyrdev monitor` for a
standalone USB monitor. [Monitoring and JSONL recording](telemetry.md#text-monitor-and-recording)
describes source selection, rates, logging and the view-switch shortcut.

### Wake over USB

When shutdown leaves USB attached for charging, restart the hub explicitly:

```sh
uv run brickphyrdev usb wake
uv run brickphyrdev terminal --usb
```

Add `--serial SERIAL` after `usb` when several hubs are attached. Wake waits up
to ten seconds for the same hub to report that it is running. Transient USB
descriptor and transfer errors during re-enumeration use that same deadline;
access errors are reported without retrying. It does not restart
an already running hub or interrupt its application. Discovery, mode selection
and opening a terminal do not wake the hub implicitly. The command requires
firmware supporting the power request below; it cannot power a disconnected or
fully unpowered USB device. See [shutdown and charging](settings.md#inactivity-shutdown).

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
to drain. A control-key input flush also leaves telemetry and connection handling
responsive. This behavior is shared by USB and BLE.

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
The F4 Bluetooth profile requests ATT MTU 247, leaving up to 244 bytes in a
write or notification. Native output batches up to that capacity, independently
of the Pybricks event limit. Notifications and acknowledged
client writes respect the negotiated capacity; peers at MTU 23 still use 20-byte
chunks. The CLI reads Bleak's public single-packet write limit, retaining write
responses to detect rejected input. Larger MTUs do not change the stream format.
The combined client uses binary SMP on the standard SMP GATT characteristic
alongside the native terminal characteristics, on one BLE connection. Requests
are serialized; terminal input and output remain independent of SMP framing.
This requires firmware supporting both channels together. USB retains standard
serial SMP framing in the Shell stream. BLE stream teardown joins accepted
Shell handlers and SMP packets before releasing the connection's resources.
On one USB-powered Inventor F4, ten rounds of Shell output and SMP status
completed on one BLE connection. A simultaneous passive 500-row capture at a
requested 15 ms period and three full Shell replies decoded without gaps. Motors
were off; these checks do not establish battery-powered robot timing.
The F4 profile requests a 7.5 ms connection interval during SMP exchange through
Zephyr's existing connection-parameter control, restoring its preferred idle
parameters after five seconds without SMP traffic. The central may choose other
parameters. `bluetooth` prints the actual interval, peripheral latency and ATT
MTU for each connected BLE link; these values do not by themselves establish
payload throughput.
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

The base profile provides OS echo/reset and Settings read/write/commit/whole-store
save. Read and write operate on RAM; save is explicit. Settings values and storage
limitations are described in [settings](settings.md). Load, delete and named saves
are currently rejected before mutation. Application profiles add the file and
program operations described above. F4 also enables the standard SMP Shell group
for [command files](telemetry.md#command-files), reusing the Shell parser and
handlers in an execution context without another worker or history. Its bounded
response reports the command return code and output truncation. SMP firmware
updates are not enabled.

Serial SMP executes on the Shell thread, so a response cannot interleave with a Shell
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

Connection admission, teardown and framed USB Pybricks parsing share the MCUmgr
management queue instead of reserving another thread and stack. A parsing batch
is bounded by the receive-buffer capacity; pending input wakes take precedence
over periodic status polling. Shell retains its own worker, so teardown can join
an entered command without blocking the controller that delivers its output.
Management work must not wait for another operation on the same queue. The F4
native/Python and Technic native profiles return 2 KiB of recovered RAM to the
shared heap; application stacks and loader limits are unchanged.

Unknown CBOR fields may contain at most four nested maps/lists when skipped.
Deeper values return a decode error instead of consuming unbounded C stack.
`CONFIG_ZCBOR_MAX_RECURSION_DEPTH` controls this limit independently of decoder
state backups. The [CBOR prerequisite](upstream.md#cbor-stack-bound) describes
the local dependency changes.

## USB selection experiment

The required outcome is computer-controlled interface selection without buttons
or a hub restart. Buttons may offer the same choice locally, but automated tools
must not depend on them. Bootloader recovery remains a separate operation under
[recovery](recovery.md).

The implementation uses one CDC ACM data channel and device-specific control
requests on endpoint 0. Boot selects Brickphyr Shell/SMP (yellow central-button
lighting). The selection survives closing the port and changes only through an
explicit request or reboot; reboot restores Brickphyr mode. No protocol is guessed
from data. USB IDs remain the experimental `2fe3:0004`.

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

`uptime` must continue across the round trip. Reboot restores Shell as the default.
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
the same command/event handlers over USB and BLE. Subscribed status repeats every
500 ms, as in Pybricks, so clients can recover a notification missed during
connection setup. A completed program's queued output precedes its stopped status.
A new start while an application runs returns busy; stop uses the common
supervisor. In the F4 Python profile,
completing a Pybricks download installs it in the selected persistent slot.
Incomplete transfers are discarded without replacing the installed program.
Builds with `CONFIG_BRICKPHYR_PYBRICKS_SLOTS=0` retain the RAM-only adapter:
complete uploads survive disconnection until replacement or reboot. Native
Brickphyr temporary uploads stay in RAM in both configurations.

#### Application data

`pybricks.messaging.AppData` (also `pybricks.tools.AppData`) exchanges binary data
with a subscribed Pybricks client through these same USB/BLE handlers. It is
separate from standard input/output and does not open another connection.

`AppData([(mode, size), ...])` sorts unique byte-sized mode numbers and assigns
contiguous receive regions, initially zeroed. The combined receive capacity is
1,024 bytes. `get_bytes(mode)` returns a snapshot; `get_bytes(mode, index)` reads
one byte. Client command 7 contains a little-endian 16-bit offset followed by
data; valid writes atomically overwrite that part of the receive buffer.

Construction announces `[0, sorted mode numbers...]`. `write_bytes(data)` sends
opaque data; `configure(mode, parameter, value)` sends
`[1, mode, parameter, value...]`. All use event 2. One outgoing payload fits one
event: at most 63 bytes over USB, and `min(63, ATT_MTU - 4)` over BLE (19 bytes
at the initial MTU of 23). The setup/configuration headers count toward that
limit. Oversized messages fail before replacing a pending send.

Ordinary calls wait; inside `run_task` they return awaitables. A newer send
replaces a pending one. Completion means acceptance by the existing transport
queue, not acknowledgment from the remote application. Status and stdout retain
priority. Disconnect or selecting native management aborts a pending send with
an error; the Python program continues if it handles that error. Reconnection
allows another send. Empty writes complete without emitting an event.

Only one AppData object may be open. `close()`, collection, forced application
stop and REPL reset release its mailbox. Failed construction also releases it.
The supervisor copies all exchanged bytes; transport callbacks never retain
pointers into Python memory. ARM checks cover both transports, mode layout,
size limits, replacement/cancellation, disconnect and lifetime cleanup using a
synthetic GATT peer and UART. Inventor F4 checks with pybricksdev cover binary
USB/BLE exchange, offsets and rejected out-of-bounds writes, configuration events,
awaitable sends, USB-to-BLE reconnection and mailbox cleanup after forced stop.

For a connected pybricksdev hub whose program has opened AppData, the application
defines the receive-buffer layout. Subscribe before starting the program when
its initial mode announcement is needed. A subsequent exchange can use:

```python
with hub.app_data_observable.subscribe(lambda data: print(data.hex())):
    await hub.write_app_data(b"\x01\x02", offset=0)
```

`write_app_data` sends one atomic protocol write. It rejects oversized packets
instead of splitting a write into separately observable updates.

#### Program representation

The reference client's module records are normalized to the common
[Python file map](applications.md#named-python-programs), retaining one importer
and application lifecycle. The first record is the entry point, including when
Pybricks Code names it after the opened file instead of `__main__`; its original
module name remains available to imports. The advertised transfer limit is
64,496 bytes in the full F4 Python profile, leaving room for file-map metadata within the 64 KiB application
limit. The receive buffer grows with accepted data instead of reserving that
maximum for every program. Growth temporarily retains the old buffer; package
normalization also needs space for the common file map. Allocation failure
preserves acknowledged fragments, and starting a new transfer releases the
incomplete one. Protected ARM checks cover small-program execution under heap
pressure, rejected growth and subsequent recovery.
The Python profile advertises ordinary format-6 bytecode. Native `.mpy` requires
the [matching Brickphyr SDK](builds.md#experimental-python-profile), whose enlarged
FPU exception frame is incompatible with native format 6.3. Upload SDK-native
modules through Brickphyr SMP; the Pybricks client path remains bytecode-only.
The actual serial client has protected ARM checks for bytecode imports and
rejection of SDK-native modules before replacement starts; the previous stored
program remains runnable. The hub independently validates received modules.

The actual pybricksdev serial client passes upload, imports, input/output, stop,
repeat and reconnect checks against ARM-emulated firmware. A separate protected
ARM fixture covers the production BLE GATT adapter, ownership and interpreter
with a synthetic controller, including notification backpressure and disconnect.
On the Inventor F4, both actual pybricksdev clients additionally upload, execute
and repeat the [hub example](../examples/python/hub.py) through a persistent slot.
The named applications remain unchanged and the test slot is removable through
native management. Unchanged [Pybricks Code over BLE](bluetooth.md#pybricks-code)
also passes upload, imports, input/output, stop, repeat and reconnect checks on
that hub. This does not establish complete library compatibility. Persistent slots
additionally have ARM checks for selection, transfer failure, storage exhaustion and coexistence
with named programs. On Inventor F4, disconnecting USB midway through a download
and disconnecting BLE after all payload bytes but before final metadata both
preserve the installed program. Reconnecting through the other transport rejects
completion of the abandoned upload, runs the installed program and accepts a new
download. These checks use deliberate client disconnection, not power removal or
loss during flash publication. Physical slot retention across power loss remains unverified.
Slot contents and execution after a software reboot pass on the Inventor F4;
this does not establish retention across complete power removal.
Subscribed output applies backpressure through the common
[application queue](applications.md#python-input-and-output). Python waits
interruptibly when it fills; USB continues receiving a STOP command behind a
pending output event. BLE retries its retained notification before consuming
another fragment. Detach or selection of native BLE management restores the
nonblocking application log, whose overflow counter the Pybricks client does not
expose. Long-output and blocked-stop cases have ARM checks. Both physical clients
also deliver a finite 4 KiB output exactly, stop continuous printing, interrupt a
non-yielding loop and run again after reconnecting. F4 USB also preserves complete
output records across an unread, full host TTY buffer and accepts STOP; its reply
and stopped status are received after reading resumes. This check does not measure
when STOP executes during the unread interval. An F4 diagnostic image additionally
delivers a finite 7.6 kB BLE stream exactly while counters confirm repeated
notification-allocation retries. Recovery from sustained physical BLE send failure
remains unverified. Notifications have no application-level acknowledgment, so
pausing a Python reader does not establish pressure on the hub while the host
Bluetooth stack keeps receiving. F4 USB checks also cover byte-for-byte readback of
multi-module program creation/replacement, all five slots filled with 30 kB
programs, and replacement of a slot while the other four remain stored.
Application storage fills its existing read cache per flash transaction to reduce metadata
read overhead without additional cache memory. A final USB upload acknowledgment
exceeded the client's two-second timeout in one check; its cause remains
unresolved. Successful timed transfers do not establish a worst-case
storage-response bound.
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

Select a slot with left/right while the hub is idle in Pybricks USB mode.
The matrix shows the selected digit; a short center
press runs its stored program. The Bluetooth button opens the connections menu
only while no program or management session is active. During a program it
remains an application button.

The native hub terminal can also select a slot:

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
Use the [client correction](upstream.md#python-client-prerequisites) when starting
without an explicit slot immediately after connection; a cached status selection
may not yet match the hub.

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
reconnection checks. The Brickphyr Python profile additionally passes upload,
execution and the input/output checks described above. Program execution through
this transport on the official image remains unverified.

The library entry point is `pybricksdev.connections.serial.PybricksHubSerial`.
Use its `connect()`, inherited command methods and status/stdout observables, and
`disconnect()`. The port is explicit; the client does not change Brickphyr's
selected interface. A timeout or canceled exchange closes the session to exclude
ambiguous late replies. A rejected command leaves a healthy connection available.
Disconnection does not stop a user program.

The same transport supports `pybricksdev run usb --port PORT SCRIPT` for hubs that
advertise a supported execution engine. Brickphyr's Python profile advertises
the implemented formats above; the base image without Python advertises none.

### Experimental control requests

The interface selector uses device recipient, vendor request `0x42`, and `wIndex = 0`.
GET (`bmRequestType = 0xc0`, `wValue = 0`, length 8) returns format version 1,
mode (0 Pybricks, 1 Shell), occupied flag, selectable flag, then a signed
little-endian 32-bit transport error. SET (`0x40`, length 0) takes the mode in
`wValue`. Unsupported values and occupied transitions stall the request; GET
reports the current state without taking ownership. Errors retain the first
transport failure and require a reboot before retrying.

Power request `0x43` uses the same device recipient with `wValue = wIndex = 0`.
GET (`0xc0`, length 2) returns version 1 and phase: 0 running, 1 stopping,
2 off, 3 waking. SET (`0x40`, length 0) requests wake from off; running and waking
are idempotent successes, while stopping or invalid fields stall. Admission stays
closed until reboot. Reset can interrupt the SET status acknowledgment: clients
must query the same USB serial until it reports running, rather than retry the
mutation or treat a disappearing device as proof of success. Neither request
acquires a terminal session or detaches its CDC driver.

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
