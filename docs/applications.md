# Applications and runtime contract

This document distinguishes the experimental native implementation from the
remaining application model. The ABI is not stable. Keep SDK definitions shared
with the firmware; do not copy an API description into multiple runtimes.

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
bounded output, read-only buttons, optional [hub matrix](hardware.md#hub-matrix),
[IMU snapshots](hardware.md#hub-imu),
[speaker tone and PCM](hardware.md#hub-sound), and
[battery snapshots](hardware.md#hub-battery).
Matrix writes copy into firmware storage and return on acceptance; snapshots
report completion and errors. Text/constants, initialized data and BSS use
three MPU partitions; the stack has its own protection. Native code receives no
device handles or raw flash service. Essential system threads preempt a busy
application independently of time slicing. This is not a claim that the privileged
ELF parser or kernel withstand every malicious image.

Typed result structures and PCM buffers require their natural C alignment.
Syscall validation checks it and the complete memory range before entering
service locks; a bad pointer ends the application without stranding those locks.

The supervisor joins the application before removing partitions and freeing its
loader allocation. Return, stop and MPU faults retain their result for inspection;
cleanup errors are reported separately. Host disconnection leaves the application
running and its power hold active. Shutdown and all supported reboot paths stop
it before proceeding. Matrix cleanup waits for the final blank before releasing
ownership and unloading the application. A failed blank retains the resource and
reports a cleanup error. No motor API is exported yet.

Use `app run`, `app status`, `app output` and `app stop` in either USB or BLE Shell. Output is
a 1024-byte queue with counted overflow; writing never waits for a reader. The
`diagnostics` command separately drains a bounded firmware log queue. Its backend
remains available without a host, including during LLEXT's deferred-log flush.

The initial admission limits are 32 KiB of ELF and 64 KiB of loader allocation,
borrowed from a shared 128 KiB pool. A RAM upload can coexist with the
loader inside this pool without a second permanent engine heap. Alignment,
symbols and scratch space count against the loader budget; small images with
large BSS can still exceed it. The application stack is 2048 bytes; compiler
generated privileged stacks and guards are additional firmware RAM. `app status`
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
letters, digits, dots, underscores or hyphens, with no leading dot. Other paths,
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
