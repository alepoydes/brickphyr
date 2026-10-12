# Applications and runtime contract

This document distinguishes the experimental native and Python implementations
from the remaining application model. The ABI is not stable. Keep SDK definitions shared
with the firmware; do not copy an API description into multiple runtimes.

The [Python implementation plan](python.md) describes the next runtime milestone,
including REPL, named programs, native `.mpy` and Pybricks client integration.

## Experimental native profile

The opt-in F4 profile runs one ARM ELF32 relocatable C image through Zephyr LLEXT.
The matching SDK emits inline system-call stubs and an ABI identity checked before
replacement. Unresolved imports, TLS and incompatible ARM/float formats are rejected.
The current toolchain uses Thumb, soft-float compiler support and the SDK's static
C library for operations such as compiler-generated `memset`, linked into the
application. These execute in its protected domain; no privileged C-library
imports are exposed. This does not supply POSIX or host I/O functions.
`app_main`, constructors and normal-return destructors run in user mode.
Explicit stop and faults do not depend on user cleanup executing.

The [SDK header](../sdk/include/brickphyr.h) defines monotonic time, cancellable sleep,
bounded output, read-only buttons, optional
[button lighting](hardware.md#application-button-lighting),
[hub matrix](hardware.md#hub-matrix),
[IMU snapshots](hardware.md#hub-imu),
[speaker tone and PCM](hardware.md#hub-sound),
[battery snapshots](hardware.md#hub-battery), and
[external port identification, sensor measurements and direct motor drive](ports.md).
Matrix writes copy into firmware storage and return on acceptance; snapshots
report completion and errors. Text/constants, initialized data and BSS use
three MPU partitions; the stack has its own protection. Native code receives no
device handles or raw flash service. Essential system threads preempt a busy
application independently of time slicing. This is not a claim that the privileged
ELF parser or kernel withstand every malicious image.

Typed result structures and PCM buffers require their natural C alignment.
Syscall validation checks it and the complete memory range before entering
service locks; a bad pointer ends the application without stranding those locks.

`bp_system_get()` reads the calling application's identity, hub name, cached boot
cause and current USB/BLE management link. `bp_set_stop_buttons()` selects a
fresh-press combination; native applications default to no program stop button,
while Python defaults to the center button and receives `SystemExit`.
`bp_shutdown()` requests the same supervisor cleanup and power-off as the hub UI.
Long-center-press power-off remains independent of program button selection.

The supervisor joins the application before removing partitions and freeing its
loader allocation. Return, stop and MPU faults retain their result for inspection;
cleanup errors are reported separately. Host disconnection leaves the application
running and its power hold active. Shutdown and all supported reboot paths stop
it before proceeding. Matrix cleanup waits for the final blank before releasing
ownership and unloading the application. A failed blank retains the resource and
reports a cleanup error. Button-light cleanup restores system indication for each
claimed light; a failed restoration retains its claim for retry. Sensor
mode claims return to automatic acquisition on application exit; normal host
disconnection preserves them. Motors share those port claims. Cleanup waits for
coast before freeing ownership and unloading;
failed cleanup retains the claim and image so explicit stop can retry.

Use `app run`, `app status`, `app output` and `app stop` in either USB or BLE Shell. Output is
a 1024-byte queue with counted overflow; writing never waits for a reader. The
`diagnostics` command separately drains a bounded firmware log queue. Its backend
remains available without a host, including during LLEXT's deferred-log flush.

The initial admission limits are 32 KiB of ELF and 64 KiB of loader allocation.
A linker-aligned 64 KiB execution arena serves the one active engine; a separate
64 KiB system heap supplies uploads and firmware allocations. Python reuses that
same arena when enabled. Alignment, symbols and scratch space count against the
loader budget; small images with large BSS can still exceed it. The native-only
application stack is 2048 bytes; compiler-generated privileged stacks and guards
are additional firmware RAM. `app status`
reports the image, reserved loader budget, LLEXT allocation and stack high-water
mark after termination. LLEXT's allocation count excludes allocator overhead and
is not the peak RAM requirement.

The board checks verify repeated execution/unload, constructor state,
soft-float arithmetic, forced termination, recovery after a prohibited RAM write,
cross-transport reconnection and power holds on one Robot Inventor F4 (PCB revision
unknown). ARM QEMU additionally checks allocator capacity restoration, exhaustion,
ABI/truncation rejection and command execution at the terminal priority. SMP checks
use separately compiled EDK images through the actual protocol and CLI, both on
ARM QEMU and the F4. Both RAM and installed files use this lifecycle; see
[build and SDK usage](builds.md#experimental-native-sdk).

RAM upload and start are distinct operations. An upload never stops the running
application. Its final fragment triggers complete-image validation; an incomplete
upload is discarded after the closing session's accepted operations finish. A
complete image stays ready across sessions until started, discarded, replaced or
rebooted. Starting it uses the same supervisor and releases the receive buffer
after LLEXT has copied retained sections. `app discard` frees a waiting image
without stopping the active one. See [host commands](host.md#native-applications).

## Installed native files

The F4 profile mounts a separate 224 KiB LittleFS partition at `/apps`. It shares
neither files nor allocation with settings. The [recovery map](recovery.md) defines
its bounds and initialization policy. A successful mount exposes existing files;
only an entirely erased partition is initialized automatically.

Standard SMP file management transfers `/apps/.incoming`. This reserved staging
file is excluded from the application list. Installed names contain 1–32 ASCII
letters, digits, dots, underscores or hyphens, with no leading dot. Reserved
`@N` names identify [Pybricks slots](host.md#stored-pybricks-programs); they can be
listed, run, read and removed through the same store, but ordinary installation
cannot overwrite them. Other paths,
including settings and traversal aliases, are denied. Installed files are readable
through SMP but cannot be overwritten through the file-upload command.

Publication closes the transfer, checks the declared size and the complete ELF
using the same admission checks as RAM, then atomically renames the staging file.
It does not execute code or stop the current instance. An interrupted or rejected
update leaves the previous installed file intact. Partial staging data may survive
a disconnect or reboot; the next installation closes and truncates it. LittleFS
metadata and staging consume space, so reported free bytes are not a guarantee
that a file of exactly that size can be installed. Capacity errors preserve
previously installed applications.

`app list`, `app run NAME`, and `app remove NAME` are available in the shared Shell.
The host [CLI](host.md#native-applications) provides the same lifecycle over USB
and BLE. Removal or replacement affects the stored image, not a running instance:
LLEXT has already copied its execution sections into protected RAM. There is no
autorun. Installed images retain their ABI identity; firmware/SDK changes can
require rebuilding and reinstalling them.

File runs use Zephyr's LLEXT file loader without reserving a full ELF receive
buffer. File admission and loading hold the store lock so replacement cannot
change the checked file. Both sources then enter the same protected supervisor.
LittleFS installation, rejected partial updates, reboot/power-cycle persistence and USB/BLE
Shell/CLI launches have been checked on the Inventor F4. ARM QEMU also exercises
filesystem exhaustion and compares the loaded allocations of both sources.

## Experimental Python profile

The additional [Python profile](builds.md#experimental-python-profile) runs source
and compatible format-6 bytecode from RAM through the same supervisor and SMP
operations as native applications. Compilation and execution run in Zephyr user
mode. Each start initializes fresh interpreter state; normal return, exceptions
and forced stop release ownership before another engine can use the arena.
Closing the host connection leaves the program running and its power hold active.

Native/Viper decorators and native format-6.3 `.mpy` use MicroPython's compiler
and loader in that same domain. The tested external compiler target is `armv7m`,
software floating point, single-precision MicroPython objects; incompatible native
architectures/sub-versions are rejected. A separate 16 KiB code region is allocated
from the system heap on first use, with up to 32 KiB transient allocator space
needed for MPU alignment. This region is read/execute for the application; its
ordinary data remains non-executable. Committed code stays allocated until the
application ends, including functions no longer referenced by Python. Exhaustion
raises `MemoryError`. Status reports its reservation as `code` and usage as
`code_used`, separately from the execution arena.

The 32 KiB upload limit also applies to Python. The retained source/bytecode shares
the 64 KiB arena with the GC heap, so larger programs leave less runtime memory.
The Python profile requests a 12 KiB application stack; the F4 MPU rounds its
allocation to 16 KiB. Stack checking uses the actual lower bound with a 512-byte
MicroPython margin. These are admission limits, not guarantees that every image
within them has enough compiler memory or stack. Status reports an execution error
or protection fault when applicable.

Output uses the shared [bounded queue](#python-input-and-output), with flow control
for subscribed Pybricks output and counted overflow for detached logging.
Upload admission checks the format envelope; Python parsing and full
bytecode loading happen in the protected application after start. Interactive
REPL has ARM/CLI and Inventor F4 USB/BLE checks as described below. The initial
[Pybricks bindings](compatibility.md#implemented-library-subset) reuse the common
device services. Writable Python storage and the remaining bindings are in the
[implementation plan](python.md).

ARM QEMU checks syntax/runtime errors, memory and stack exhaustion, GC, truncated
bytecode, maximum-size source, forced stop and repeated Python/native transitions,
including recovery after a native MPU fault. On one Robot Inventor F4, USB and BLE
checks cover source/bytecode, GC, exceptions and another run. A non-yielding Python
loop remains running across USB-to-BLE handover and can be stopped through BLE;
incomplete staging is discarded and the application power hold clears on stop.
These checks do not establish Pybricks API compatibility or hardware timing bounds.
Bounded native/Viper calculations and a dynamic C `.mpy` with GC also pass through
USB/BLE on that F4. ARM QEMU additionally checks write/execute protection, kernel
data protection, forced native-loop stop, code exhaustion and subsequent execution.
Source, bytecode and dynamic C imports share MicroPython's import machinery.
Package-relative imports, fresh module state and recovery from a truncated native
import have ARM coverage. Temporary and installed programs with imports execute
over USB/BLE on that F4 and from its Shell. Reboot preserves named Python programs
and discards the staged RAM image; full power-cycle acceptance of this Python
profile remains pending.

### Standard Python modules

The Python profile uses the pinned MicroPython implementations of `io`, `math`,
`micropython`, `struct`, `sys`, `errno`, `json`, `random`, `select` and `gc`.
Their inclusion follows the selected Pybricks F4 reference. `memoryview`, builtin
help and module discovery, memory diagnostics, GC finalizer support and the
reference's extended/reverse class operators are enabled too. Device finalization
does not replace the supervisor's unconditional application cleanup.

| Area | Executable ARM coverage |
| --- | --- |
| `io`, `sys`, `select`, `errno` | Text/binary streams, buffer direction, polling, input, exception formatting, resource reads and file errors |
| `struct`, `memoryview` | Endianness, in-place packing, buffer boundaries, shared views and read-only buffers |
| `json`, `random`, `math` | Stream decoding, seeded generation, ranges and rejected inputs, numeric results with single-precision tolerances |
| `gc`, `micropython` | Collection, thresholds, allocation locks, diagnostics, optimization selection and native/Viper execution |
| Interactive/language support | Module help, reverse operators, editing, interruption and fresh state after reset |

The short module suite executes selected unchanged cases from the pinned
MicroPython checkout against its expected results or CPython where portable.
This is integration coverage of the listed behavior, not full CPython
compatibility. The newly enabled features have an F4 build but await physical
acceptance; earlier stream/resource and native-code checks are listed above.

The optional [Pybricks library](compatibility.md#implemented-library-subset) shares
this runtime, importer and I/O. Parameters, geometry and non-device tools have
software coverage; hardware bindings remain unfinished.

### Named Python programs

A Python program with accompanying modules is one CBOR map from relative file
paths to byte strings. By default it has exactly one root entry, `main.py` or
`main.mpy`. An optional empty-string key selects another entry: its byte-string
value must name an existing `.py` or `.mpy` file in the same map. This metadata is
not a file. The Pybricks adapter uses it to select `__main__.mpy` while preserving
an ordinary imported module named `main`.
The CLI builds this image from a dedicated application directory, or wraps a
single `.py`/`.mpy` file for installation. Source and compatible bytecode/native
modules can coexist, including packages with `__init__.py` or `__init__.mpy`.
MicroPython owns module resolution and execution; this map supplies the file bytes.
It introduces no second writable filesystem or package manager.

The entire encoded image is limited to 32 KiB, at most 64 files plus optional entry
metadata, and 128 bytes per relative path. Absolute paths, empty/`.`/`..` components, embedded NUL,
duplicate keys and file/directory collisions are rejected. CBOR keys and values
must be definite-length text and byte strings. Admission validates the complete
map and each Python/MPY format envelope; syntax and bytecode-body errors remain
execution errors. Unsupported native architectures are checked again on import.

Named images use the same LittleFS staging, validation, atomic rename, listing and
removal as native ELF. Upload/install never starts or stops a program. On launch,
the complete file set is copied into the shared execution arena alongside the GC
heap. Later replacement or removal cannot change a running instance's imports,
even when a module is imported again after removal from `sys.modules`. Validation
and file loading temporarily require a complete image buffer in the system heap;
memory exhaustion fails without replacing the installed version.

Temporary directories use the same image and interpreter without any application
storage write. `open()` and `io.open()` read bundled resources in text or binary
mode, with seeking, iteration and context-manager close. Write modes fail with
`EROFS`; a missing or escaped path fails with `ENOENT`. An absolute path is rooted
in this same program namespace, never in the firmware filesystem. `__file__` and
`sys.argv[0]` identify the root entry.
See [CLI and terminal commands](host.md#python-applications).

### Python input and output

Scripts use `input()`, `sys.stdin`, `sys.stdout`, `sys.stderr` and their binary
`.buffer` streams. Upstream MicroPython supplies readline editing and the stream
methods. `io`, `json`, `random`, `select` and `errno` join the existing core
modules. `random.seed()` uses uptime as its default seed; it is not a source of
cryptographic randomness. `select.poll()` waits on the same application I/O
event instead of spinning in a host-specific reader.

The supervisor has a 256-byte input queue and accepts fragments up to 128 bytes.
A full queue returns `EAGAIN` without accepting any of that fragment. Accepted
script input remains with the application across host disconnection. Each new application
starts with an empty queue. Ctrl-C requests `KeyboardInterrupt`, clears prior
queued input and retains bytes following it in the accepted fragment.
`micropython.kbd_intr()` changes or disables that control character; an explicit
management interrupt remains available independently. Interrupts are delivered
at VM event points, including blocked input and output. A native loop may require forced
stop through the supervisor.

Text output converts LF to CRLF; `.buffer` preserves bytes. Both streams feed one
1024-byte application queue. While a continuous consumer is attached, Python
waits interruptibly for queue space instead of discarding bytes. The Pybricks
subscription, plain Shell REPL and `brickphyrdev terminal --repl` use this same
mechanism. An SMP client can explicitly attach before starting a program using
[application command 4](host.md#native-applications).
Stop remains available with unread output; writable polling reports queue space.
Detachment or disconnection releases that wait without stopping the application.
Changing from Pybricks to native BLE management releases the old subscription;
subsequent native requests preserve an explicitly attached native consumer.

Without that continuous consumer, writes use the nonblocking application log;
status counts discarded bytes when the queue overflows. One-shot `app output`
reads keep this behavior, so autonomous execution remains independent of a host
polling its log. Native `bp_write` retains its nonblocking behavior. `flush()` has
no extra Python buffer to drain and does not promise host receipt. Reading
stdout/stderr or writing stdin fails explicitly.

ARM user-mode checks cover these streams, interruption/backpressure and resource
access after stored-program replacement. Inventor F4 checks cover input, output,
resource reads and a catchable loop interrupt over USB/BLE, Shell input and a
pending read retained across USB-to-BLE handover.
Continuous output has ARM serial-client, CLI/PTY and synthetic BLE-adapter checks
for complete long output, stop/interruption during a blocked write and release
on detach/profile change. REPL checks cover output after a Python reset and
draining the final queued output after termination. Physical acceptance of this
flow control remains open.

### Interactive Python

`brickphyrdev terminal --repl` starts or reattaches to the same protected VM through
the application operations. Add `--usb` for USB. An already running ordinary
application causes a busy error; entering REPL never silently replaces it.
MicroPython's maintained friendly/raw REPL and readline supply parsing, multiline
input, editing, history and completion. The existing host terminal supplies TTY
handling and transport flow control. Input, output and optional monitoring share
one serialized SMP connection.

Ctrl-C interrupts Python or cancels the line being edited. Ctrl-D at an empty
prompt requests a soft reset: the supervisor joins the old thread, releases its
devices and executable allocations, then starts fresh Python state. Raw REPL
remains raw across that reset. This does not reboot firmware or restart its device
services. Ctrl-X stops REPL and returns to Shell; it remains available when a
program is not reading and keyboard input is backpressured. Ctrl-] closes the
host terminal without stopping Python. A physical session boundary discards
queued REPL input and cancels an unfinished REPL command line, preserving globals
and any command already executing. Reattachment does not start another VM.

The CLI reads the existing bounded output log and reports its drop count. An idle
poll waits 50 ms; busy output drains at the transport's acknowledged pace, with
status sampled once per second. These are host interaction intervals, not
device-service timing guarantees. REPL may be combined with `--monitor`.

An ordinary hub Shell also accepts `python` to enter that same VM, using the
existing connection as a plain byte stream. It has the same editing, reset,
reattachment and explicit exit behavior. Ctrl-X in this raw stream follows bytes
already queued for input; it cannot overtake an unread large paste. The SMP CLI
path above has a separate stop operation and can interrupt input backpressure.
Disconnecting either frontend leaves the application running.

A connection that already uses SMP must use `terminal --repl`; plain `python`
entry rejects that connection before starting the VM. Raw REPL bytes must not
be guessed to be management frames. This keeps binary output intact and avoids
two consumers of one stream.

Protected ARM checks cover editing, input, interruption, state retention,
admission, native-code cleanup on soft reset and the raw protocol. Real CLI/PTY
checks cover detach, monitoring, TTY restoration and forced exit with a full input
queue. Session-notification checks use synthetic detach on QEMU's plain UART.
Plain Shell entry, return and reconnection also have ARM checks through the shared
host dispatcher. On the Inventor F4, both USB/BLE paths pass friendly/raw REPL,
editing, history/completion and soft reset. SMP additionally checks interruption
and display cleanup on reset; plain Shell checks entry/return, retained globals
and discarded partial input across a physical reconnection. Combined output
backpressure and CLI/monitor acceptance on hardware remain open.

## Application formats

One application executes at a time, under one of the engines enabled in the
installed firmware. Native applications do not initialize MicroPython or WAMR.
Python and WASM use the same device services through their respective bindings.

For interpreted engines, document the privilege of the VM, its bindings, and any
native extensions. Language-level restrictions alone are not evidence of MPU
isolation. Shared service ownership and lifetime rules apply to every engine.

Use Zephyr LLEXT as the initial native loader. Evaluate the actual ELF relocation,
floating-point ABI, memory alignment, and toolchain requirements on the target
before publishing SDK flags. C, C++, Rust, and Zig can be investigated as producers
of a compatible native artifact; no working Brickphyr integration for them is yet
claimed beyond the minimal C profile above. Language runtimes, constructors, unwinding, and standard libraries can
add requirements beyond a shared CPU target.

Use WAMR for WASM. Start with a supported execution mode that fits the hub;
interpreter versus AOT remains a measured implementation choice. A new JIT is not
part of bring-up. Portable WASM still needs compatible hardware imports and memory
limits. Native MicroPython `.mpy` modules are a separate compatibility question,
not the Brickphyr native application format.

## Storage and launch operations

Support both development runs without flash writes and several applications
retained across power cycles. Only one application executes at a time. The initial
implementation uses a received RAM buffer for temporary images and LittleFS in
external flash for installed files. A RAM filesystem is not required by these
use cases and is outside the initial milestone.

| Operation | Required result |
| --- | --- |
| Run a transferred image | Receive into bounded RAM, validate, load and start without persisting the image |
| Install an application | Store and validate a complete image, then publish it in the installed application list; installation does not start it |
| Start an installed application | Read the selected file and use the same admission, loader and execution lifecycle as a temporary image |

Both sources use the same application format, ABI/capability checks, allowed
imports, memory isolation, stop and fault handling. Reuse LLEXT's buffer and file
loaders. Receiving bytes alone does not execute code or stop a running program;
replacement follows the confirmed-stop order below. An interrupted temporary
upload releases its staging resources without stopping an autonomous application.
RAM exhaustion reports an error rather than silently persisting the image.

Installation uses a staging file. A complete, validated image replaces the
installed entry atomically; an interrupted or rejected update preserves the
previous installed version. Listing, inspecting and removing installed files
share the same storage operations across clients. Storage access must preserve
the settings validation/save contract and the regions in [recovery](recovery.md).
Determine the application partition and usable capacity from the actual flash
map before writing; do not inherit a fixed number of program slots.

Account for the entire peak RAM requirement: the received image, loaded sections,
relocations, loader scratch space, application data and stack, and MPU alignment.
Measure buffer lifetime and copying through the real loader before setting image
limits or promising in-place execution. Firmware and SDK must describe the same
experimental ABI. Directory layout, command syntax and wire fields are defined
with the first working implementation.

## Admission and lifetime

The supervisor must establish the following order, whatever internal state names
the implementation uses:

1. Identify the application format and required engine, ABI, capabilities, and
   resources. Reject incompatible or malformed input before application execution.
2. Stop the preceding application and confirm device operations/callbacks are
   quiescent before replacing its resources. Uploading bytes is not a STOP.
3. Load into owned memory, resolve allowed imports, and establish the application
   memory domain and permissions. Do not expose arbitrary kernel symbols.
4. Run application initialization and entry in the application domain, including
   constructors supplied by the image.
5. On explicit stop, normal return, or fault, cancel outstanding work, put owned
   actuators in their specified stopped condition, and release resources before
   unloading code. Do not free buffers while a driver or callback can still use them.

The privileged supervisor must be able to terminate an application that never
yields. User cleanup is optional assistance, not a prerequisite for stopping.
This applies to native code and both interpreted engines. Pybricks-compatible
multitasking must not reproduce system hangs; see [compatibility](compatibility.md).
Recovery from a fault is only reported after the corresponding platform action
succeeds; an unconfirmed actuator stop cannot be presented as a clean idle state.
The board implementation must define its actual stop/inhibit mechanism.

Zephyr's loader does not automatically add an extension to a user memory domain.
Initialization and teardown can execute image-provided code, so their calling
context matters. See [LLEXT loading](https://docs.zephyrproject.org/latest/services/llext/load.html).

## Device interface

Expose the operations applications need, not unrestricted register access. Keep
the native path short, with caller-owned buffers and bounded calls wherever
possible. A syscall boundary does not require allocating a Python object, copying
an entire history, or dispatching through a generic service framework.

For every public operation specify units, numeric range, ownership, lifetime,
blocking/cancellation behavior, errors, and whether it allocates. For sensing,
include acquisition time and validity. For commands, distinguish queue acceptance
from application at the device. Multi-motor timing semantics must be stated, not
inferred from two consecutive function calls.

Motor direct drive and servo commands share resource ownership. Flash writes,
controller pairing, or a diagnostic shell cannot bypass the application manager
to fight an active owner. Read-only monitoring should use existing observations
when it can, with a documented bounded cost.

The experimental SDK is generated from the same definitions as the firmware
exports. Initially require a compatible SDK/firmware ABI identity rather than
adding layers to preserve every experimental build. Reject mismatches clearly.

## Host and input devices

An autonomous application can run with no computer. Normal host disconnection,
recorder failure, or a full log queue must not implicitly STOP it. The application's
input policy determines what controller loss means; the firmware reports the event
and stops treating stale commands as fresh input.

Use one controlling desktop connection owner per hub, including uploads and
diagnostics. Other local tools must use that owner or receive a useful busy result.
The limit applies across USB and Bluetooth; see [host connections](host.md).
This host ownership does not prohibit a simultaneous Xbox connection through the
firmware's Bluetooth stack.

Native application management uses the existing SMP transports over USB and BLE.
Reuse standard file transfer for persistent storage and add only the missing
application operations, including temporary-image reception, in an application
command group. Both transports must provide the same operations, validation,
results and lifecycle; only transport framing, packet sizes and connection setup
differ. Shell and `brickphyrdev` use the same firmware operation handlers, with
the existing Shell instance shared by USB and BLE.

Application execution must join the [power policy](settings.md#inactivity-shutdown)
before the first autonomous run: a running application inhibits idle shutdown,
and normal host disconnection leaves it running. Explicit shutdown/reboot stops
and quiesces the application before the final power/reset operation.

Provide upload, start, explicit stop, capabilities, and logs first. Add equipment
monitoring when needed. Reuse public desktop transport/flashing APIs where they
match; keep protocol extensions distinct and advertise them. Do not assume the
unchanged Pybricks editor can operate every new Brickphyr application format.

## Diagnostics

Normal output consists of state transitions, actionable faults, and bounded
statistics. Detailed capture is an explicit debugging option. Active control may
drop debug messages rather than block indefinitely behind a slow host; report the
drop count. Keep mandatory device events separate from optional diagnostics.

Retain operation context and the original fault through secondary logging/cleanup
failures. Do not add redundant checksums to disposable debug data without an
identified corruption problem. Firmware images and persistent data have separate
integrity and compatibility requirements.

The [native application milestone](builds.md#native-application-milestone) defines
the implementation sequence and acceptance checks for these selected contracts.
