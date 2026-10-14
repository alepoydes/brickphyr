# Applications and runtime contract

This document distinguishes the experimental native and Python implementations
from the remaining application model. The ABI remains experimental and versioned. Keep SDK definitions shared
with the firmware; do not copy an API description into multiple runtimes.

The [Python implementation plan](python.md) describes Python support and remaining
acceptance, including REPL, named programs, native `.mpy` and Pybricks clients.

## Native portability and resource efficiency plan

The next platform milestone reduces firmware memory and execution costs while
maximizing the memory available to user applications. Preserve MPU isolation,
service responsiveness, supervised cleanup and stored data. Measure the native
and full Python profiles separately: linked RAM includes application arenas and
allocator reserves, so it is not the firmware's live working set.

1. Establish comparable flash, RAM, stack, peak loading and execution budgets.
   Include the four-port Control+/Technic Hub's reference limits from the start:
   208 KiB firmware flash, 16 KiB user flash and 64 KiB RAM. Check the feasibility
   of protected native execution and its distinct BLE controller before promising
   a complete port. Preserve its factory loader and checksum-sensitive storage.
2. Enable hardware single-precision arithmetic with the hard-float calling
   convention on capable targets. Rebuild the firmware, libraries, MicroPython
   and SDK together; compatibility with earlier experimental native binaries is
   not required. Check FP context across preemption, system calls and application
   termination, including native/Viper/C `.mpy`, and measure stack and memory costs.
3. Keep upstream CMSIS-DSP optional in firmware and the matching native SDK,
   linking only required routines and tables. Accept the sensor-processing
   example through RAM and stored-program workflows over USB/BLE. CMSIS-NN remains a
   later optional library for a concrete model, not a mandatory firmware payload.
4. Remove unnecessary image contents, dependencies and allocations; size buffers
   and stacks from contracts and measurements. Review loading copies, inactive
   engine reservations and MPU alignment. Return recovered capacity to applications.
   Optimize measured execution bottlenecks, including interrupt work, copies,
   queues and service waits, preserving numerical behavior and responsiveness.
   Report memory, throughput and latency changes together.
5. Decouple the native application ABI from firmware configuration and Zephyr's
   generated syscall numbering. Version the public interface and check required
   capabilities and memory before execution, retaining ELF/LLEXT. Target the same application binary
   on SPIKE/Inventor F4 and the four-port Technic Hub when both builds provide its
   instruction set, calling convention and required services. Keep adaptation
   costs small and measure them; do not introduce a VM for this compatibility.
6. Bring up Technic as a board/profile in the same implementation: first boot,
   recovery, power/UI, watchdog and BLE Shell; then IMU, four ports, motors,
   settings, storage and native applications. Reuse the Pybricks BLE installer
   and selected hardware code. Start with the native profile; additional engines
   depend on measured capacity. Deliver a build-checked port while Technic hardware
   is unavailable. Cross-hub hardware acceptance remains separate and requires
   the same binary running on physical F4 and Technic boards.
7. Complete the remaining [Python and device acceptance](python.md#starting-point),
   including client transfer recovery, persistent programs and battery watchdog
   behavior. Keep USB-initiated wake and physical charging-source acceptance as
   explicit hardware checks. Define storage-reset scope before implementation.
8. Publish required dependency changes in their forks, pin reachable revisions
   and accept clean builds, SDK examples and release artifacts. Report per-board
   capabilities and outstanding physical checks explicitly.

The native ELF interface now has a versioned table independent of firmware
configuration; Technic has build-only power/UI/watchdog, BLE management and
RAM-native profiles with shared IMU acquisition, battery observations, telemetry
and electrical port identification, plus direct passive-motor output. Persistent
storage, UART negotiation and regulated motors remain pending. Existing Python
acceptance work remains tracked in its owning plan; WASM is outside this milestone.

## Experimental native profile

The opt-in F4 and Technic profiles run one ARM ELF32 relocatable C image through
Zephyr LLEXT. Technic support is build-only; see its
[profile limits](builds.md#experimental-technic-base).
The SDK emits calls through a versioned, read-only firmware API table. Its resident
wrappers enter the existing Zephyr syscall verifiers; applications do not embed
Zephyr syscall numbers. Only `bp_api_v1` may be imported. TLS, other unresolved
imports and incompatible ARM/float formats are rejected before replacement.
The F4 hard-float build uses Thumb, single-precision hardware floating point and
the matching SDK's static C library for operations such as compiler-generated
`memset`, linked into the application. These execute in its protected domain; no privileged C-library
imports are exposed. This does not supply POSIX or host I/O functions.
`app_main`, constructors and normal-return destructors run in user mode.
Explicit stop and faults do not depend on user cleanup executing.

With the optional DSP configuration, the same upstream CMSIS-DSP static archive
is available to firmware and exported as `lib/libcmsis-dsp.a` in the matching
native SDK. The [IMU filter example](../examples/native/imu_filter.c) averages four
*distinct observed* calibrated hub-axis Z gyro samples. It polls snapshots every
10 ms, but acquisition can skip samples, so its window has no fixed time span.
It resets on an IMU epoch change or unavailable sample and logs one line per 20
observed samples after the window fills. A missing or slow output reader can drop
log bytes; application status counts drops. This example does not establish a
fixed-rate filter or physical IMU performance. The full F4 firmware and matching
SDK, plus the separately built example, compile with DSP enabled; a focused M4
QEMU fixture passes through the production native loader. The exported IMU example
also runs through SMP from RAM and LittleFS with a synthetic register peer and
a 4 KiB image limit, 12 KiB loader budget and 2 KiB application stack. These
checks use an MPS2 memory layout, not Technic's full platform RAM budget.
Bounded Robot Inventor F4 checks also cover the same SDK-built ELF from RAM and
LittleFS over USB and BLE, with advancing IMU samples, output and explicit stop.
The first run after a software reboot needs no preliminary IMU reads. Application
fault, cleanup and output-drop counters remain zero; existing stored files are
preserved. The 3936-byte ELF reports 3288 loaded bytes and uses 672 bytes of its
application stack in these runs. Its 10214-byte loader reservation is an admission
budget, not a measured peak heap allocation. Sensor accuracy, fixed-rate timing
and sustained combined load remain unverified.

### Portable ELF ABI

`.brickphyr.abi` carries a 20-byte, little-endian record: the `BPA1` magic,
16-bit major/minor versions, then 32-bit architecture, required-capability mask
and stack requirement. Version 1 uses fixed-width public structures, 32-bit
pointers and ARM EABI5/AAPCS. F4 and Technic use ARMv7E-M Thumb with FPv4-SP-D16 hard float;
the ARMv7-M software-float fixture is a distinct architecture, not interchangeable.

`BRICKPHYR_APPLICATION()` declares the current ABI, no mandatory optional
services, and a 2048-byte stack. Use
`BRICKPHYR_APPLICATION_REQUIRES(BP_CAP_IMU, 2048)` to require the IMU service,
for example. Combine capability bits with `|`; `bp_capabilities()` reports the
available services for conditional use. Capabilities describe compiled services,
not peripheral readiness or ownership. Device operations still report their
ordinary initialization, connection and ownership errors.

Admission requires the same major and architecture, a supported minor, all
required services and sufficient stack and loader memory. Rejection preserves
the running application. The stack requirement is an admission threshold; the
firmware still reserves one configured application stack. A matching architecture alone cannot promise that any
program fits or that a requested device is present. Within a major version,
existing function slots, signatures, structure layouts, units and semantics are
fixed; append calls at the end and increase the minor version. Incompatible
changes require a new major and table symbol. The table is generated from the
public syscall declarations; bindings keep using the same implementation.

The exported native SDK contains public Brickphyr headers and the CPU/ABI flags
selected from the actual build, with optional CMSIS-DSP headers/archive. It does
not expose Zephyr configuration, syscall IDs or board headers. Static C-library
operations execute in the application domain. The API import restriction defines
the supported interface; MPU permissions and syscall verifiers provide isolation.
This ELF contract is separate from native `.mpy`, which still needs the matching
MicroPython compiler and runtime described below.

An independently built F4 SDK `hello.elf` passes unchanged in Cortex-M4
QEMU firmware configurations with different Zephyr syscall IDs and application
stack sizes, including a 16 KiB native arena after a reserved SRAM prefix.
These checks include constructors, float arithmetic, time/output
calls, rejection and protected cleanup. They do not establish cross-hub hardware
compatibility. In the full F4 Python/DSP build, the table/admission change adds
1784 bytes of linked flash and no linked RAM: 783001 bytes flash and 324352 bytes
RAM, including reserved application pools. The table itself occupies 440 bytes
of read-only flash. Calls add a table lookup and resident SVC wrapper, without
allocation; physical call and loading latency remain unmeasured.

Native C SDK links omit ARM unwind tables. Stack unwinding is unsupported;
allocated sections left outside the accepted image format fail admission before
an application replaces the running one.

On the current ARMv7-M targets, native-only builds allocate the loader's backing
buffer from the shared system heap when starting an application and release it
after successful cleanup. The requested size is the validated image's loader
budget, bounded by the configured execution-memory limit. Native LLEXT allocations
are aligned and protected individually. Python builds retain a shared, fixed
execution arena for both engines: Python maps the whole power-of-two, size-aligned
arena as one MPU partition. That arena has no flash initialization image.
Both engines initialize their allocations and must accept arbitrary initial contents.

The native admission budget includes merged ELF region spans, MPU alignment,
symbol/string tables and loader bookkeeping. It excludes metadata that is only
streamed or skipped; gaps inside a merged region still require memory. The budget
is a conservative reservation, not a measurement of the loader's peak usage.

A native-only replacement reserves its new buffer before stopping the old
application. Insufficient or fragmented heap therefore rejects that start without
interrupting the running program. This briefly requires space for both buffers;
explicitly stopping the old program first releases its budget. Cleanup failure
retains the old buffer until devices, MPU partitions and the loader are released.

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

The F4 and Technic board profiles omit direct Zephyr UART, GPIO, SPI, I2C, ADC,
PWM, watchdog and hardware-information syscalls. Privileged services own hardware
access; native and Python operations use the shared, validated Brickphyr API.
This removes unused driver entry points without changing the application ABI or
disabling the drivers. See the [Zephyr prerequisite](upstream.md#supervisor-owned-driver-prerequisites).

Service files that run exclusively in privileged threads, workqueues or driver
callbacks declare `__ZEPHYR_SUPERVISOR__` before their includes. Zephyr can then
call kernel implementations directly without checking the caller's CPU mode at
each call. This annotation is local to those files: the resident native API and
Python retain their user-mode syscall wrappers, and `application.c` retains mode
checks because it also contains the user-thread entry functions. Application
buffer validation, resource ownership and MPU protection are unchanged.

Typed result structures and PCM buffers require their natural C alignment.
Calls to a service omitted from the build retain their ABI slots and can return
`-ENOTSUP` without inspecting application buffers. For enabled services, syscall
validation checks alignment and the complete memory range before entering
service locks; a bad pointer ends the application without stranding those locks.
Memory checks stay inline, while their error reporting shares one cold path
that retains the syscall name, buffer range and rejection reason. This reduces
flash duplication without adding a call or allocation to successful validation.

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

`CONFIG_BRICKPHYR_APPLICATION_IMAGE_SIZE` limits uploaded image bytes;
`CONFIG_BRICKPHYR_APPLICATION_MEMORY_SIZE` limits the loader budget and, in Python
builds, reserves the execution arena. The F4 native profile permits 64 KiB of ELF
and a 210176-byte loader budget in a 229632-byte shared system heap. The Technic native
profile permits 4 KiB images and a 16 KiB loader budget in a 25 KiB shared heap.
An image limit does not reserve upload space or guarantee that the loaded image
fits; another retained upload or firmware allocation can leave insufficient heap.
Alignment, symbols and scratch space count against the loader budget; small
images with large BSS can still exceed it. The native-only
application stack is 2048 bytes; compiler-generated privileged stacks and guards
are additional firmware RAM. `app status`
reports the image, reserved loader budget, LLEXT allocation and stack high-water
mark after termination. LLEXT's allocation count excludes allocator overhead and
is not the peak RAM requirement.

A protected ARM check loads a 64 KiB padded ELF containing a 64 KiB zero-initialized
array, verifies every byte through the application, and reclaims its memory on exit.
The upload and loader buffers coexist during startup. Padding exercises transfer
capacity, not 64 KiB of executable code. This uses the native F4 budgets with
synthetic peripherals. On a Robot Inventor F4, the native-only profile also runs
that maximum-size RAM upload over USB and BLE. A separately compiled stored ELF
runs with 64 KiB BSS plus 16 KiB initialized writable data over both transports.
Both verify their complete data regions, return without application or cleanup
errors, and leave existing stored files unchanged across the profile switch.
Their reported loaded allocations are 66258 and 83037 bytes; loader reservations
are 136864 and 170291 bytes. These synthetic memory workloads establish neither
capacity under maximum peripheral/communication load nor a worst-case stack bound.

Separate USB checks on the same F4 with Zephyr heap statistics enabled measured
system-heap allocation peaks of 203368 bytes for the RAM workload and 171260 bytes
for the stored workload, measured with a 210944-byte heap. These counts include
the reserved loader buffer; they do not measure its internal live allocations.
Both workloads return system-heap allocation to the 960-byte idle baseline.
ELF admission reads bounded offsets directly from RAM or a locked file, without
allocating a linker context. Only actual linking needs LLEXT's section cache;
file metadata lives in a separate stack frame. File errors retain their original
cause, including when closing the file also fails. The F4 Shell retains its
4096-byte stack for storage, Python and other commands.

Earlier board checks verified repeated execution/unload, constructor state,
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
autorun. Installed images retain their ABI requirements; incompatible interface changes can
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

Native/Viper source decorators still compile on the hub. Precompiled native/Viper
and dynamic C modules require Brickphyr's matching SDK compiler: native `.mpy`
uses format 7.0, with `armv7emsp` and the hard-float single-precision ABI on F4.
Native format 6.3 is rejected; ordinary format-6 bytecode remains compatible with
Pybricks clients. See [SDK compilation](builds.md#experimental-python-profile).
A separate 16 KiB code region is allocated
from the system heap on first use, with up to 32 KiB transient allocator space
needed for MPU alignment. This region is read/execute for the application; its
ordinary data remains non-executable. A privileged, preemptible worker with a
1 KiB stack initializes and publishes code below hardware-service priorities;
large memory operations do not occupy the cooperative system workqueue.
Committed code stays allocated until the
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

Brickphyr's main supervisor and dedicated service threads use kernel-only stacks
with their configured byte budgets. They do not reserve a second privileged stack
for a user-mode transition.
The application retains its MPU-aligned user stack and privileged syscall stack.
Linker RAM totals also include alignment gaps and kernel-object metadata; they
are not a measure of free system heap or Python GC capacity.
Firmware services use queues and events; the unused Zephyr asynchronous mailbox
pool is disabled. Native applications access services through the SDK rather than
kernel mailboxes.

F4 UART state and port-mode descriptions borrow from the shared system heap while
devices are connected. Empty and passive ports need no UART allocation; each
active UART link occupies 944 bytes plus allocator overhead, including its
independent 512-byte RX buffer. Compact error diagnostics remain after disconnect.
The native profile provides a 229632-byte heap, including on-demand loader buffers.
The full Python profile provides 114560 bytes and the core Python profile 144384 bytes;
both retain a 16 KiB MPU-aligned stack and a 64 KiB execution arena.
On F4 each advertised mode uses a 68-byte core, plus the actual combination-mask
and uninterpreted INFO bytes (0–224 bytes per mode) and allocation overhead.
Getters reconstruct the fixed 288-byte public structure in caller-owned memory;
DATA handling does not allocate. Metadata for six fully populated sixteen-mode
devices needs about 28 KiB including allocator overhead, in addition to the live
links; ordinary descriptions cost much less.
Unoccupied capacity is available to uploads, executable code and other shared
allocations. These are heap capacities, not increases to Python's GC arena.
Heap contention or fragmentation can reject a new device or an application
allocation. Physical combined-load acceptance of this layout remains pending.

DriveBase records also use the shared heap only while a pair or its retained logs
exist, instead of reserving every possible pair at boot. Each F4 record occupies
1264 bytes plus allocator overhead. There is no allocation in ordinary control
frames. Pair creation can report `ENOMEM` before changing either motor; retirement
and log lifetime follow the [paired control contract](ports.md#paired-drive-control).

F4 Bluetooth cryptography also allocates its temporary state from the shared
system heap, with the former 2 KiB libc arena added to that budget. LittleFS uses
the mount buffers and file caches supplied by Zephyr. Neither path needs a
separate libc allocator arena. Crypto allocation failures propagate to the
Bluetooth operation; pairing under combined memory pressure remains unverified.

Static kernel-object permissions and metadata use one RAM entry per object;
the sparse address lookup table resides in flash. F4 profiles place the complete
userspace stack range before ordinary data, directly after the execution arena
when Python is enabled, avoiding an alignment gap after BSS. Stack registration, MPU protection,
privileged syscall stacks and application limits remain unchanged. Recovered RAM
is assigned to the shared heap. These layouts require the
[Zephyr sizing changes](upstream.md#kernel-object-sizing-prerequisite).
ARM emulation checks cover lookup, permissions, protected stacks and application
lifecycle; physical combined-load acceptance of the enlarged heaps remains pending.

The optional [memory diagnostics profile](builds.md#memory-diagnostics) includes
Zephyr's `kernel heap` Shell command. It reports
current free/allocated system-heap bytes and the allocation high-water mark since
boot. This heap holds uploads, receive-buffer growth and executable native
code; its free bytes are not the Python GC budget. In REPL, `gc.mem_free()` and
`gc.mem_alloc()` report the separate GC heap. Use `app status` after a program
ends to inspect its stack high-water and native-code usage. A live application's
`unused` stack field is not a current stack measurement.

Measure cold upload, start and cleanup separately. Pybricks reception reserves
1,040 extra bytes for normalization into the common file map within the same
buffer. Conversion allocates no second image; it shifts the input once, then
encodes overlapping byte strings with zcbor outside the allocator/interrupt lock.
The completed buffer is shrunk in place so reception headroom is not retained
alongside the running application's native-code allocation.
Unknown-length reception still briefly holds old and new buffers while growing.
Growth above half the transfer limit requests the full limit, avoiding two nearly
full buffers for the last fragment; if that allocation fails, an exact-size
request may still admit the fragment. Heap counters do not report the largest
contiguous allocation or prove absence of fragmentation.
ARM/QEMU checks exercise a synthetic 32 KiB bundle with a native import through
RAM and stored starts and verify system-heap reclamation. Its fixture uses a
64 KiB system heap and 64 KiB execution arena, but does not establish F4 USB/BLE
driver overhead or the compiler requirements of other programs.

On one Robot Inventor F4 (PCB revision unknown), the full Python profile with
Zephyr's optional memory counters also runs a synthetic 32,768-byte file map
from RAM and LittleFS over USB. Most of the image is an unused resource; the
program imports a dynamic C module and creates default HubNetwork inboxes without
connecting peers. With Zephyr 4.4.2, Zephyr SDK 1.0.1 and the earlier 64 KiB
system-heap configuration, the observed budgets are:

| Allocation | Observed bytes |
| --- | ---: |
| System heap with the complete RAM upload | 32,772 |
| System heap with native code and HubNetwork inboxes active | 19,464 |
| Python GC allocated/free after imports and collection | 1,872 / 30,128 |
| Application stack used/reserved after return | 1,968 / 16,384 |
| Native code used/reserved | 352 / 16,384 |
| Peak system-heap allocation through a separate 31,728-byte Pybricks upload | 48,728 |

The system-heap allocation counter returns to zero after cleanup. The Pybricks
upload's normalized file is read back exactly; it is not executed in this workload.
Allocator counters include rounding and differ from payload sizes. These figures
cover this workload and build, not arbitrary source compilation, fragmentation,
simultaneous active peer traffic or the largest possible allocation. Normal
firmware omits the diagnostic commands. Other workloads still need their own
GC, stack and system-heap budget.

Output uses the shared [bounded queue](#python-input-and-output), with flow control
for subscribed Pybricks output and counted overflow for detached logging.
Upload admission checks the format envelope; Python parsing and full
bytecode loading happen in the protected application after start. Interactive
REPL has ARM/CLI and Inventor F4 USB/BLE checks as described below. The initial
[Pybricks bindings](compatibility.md#implemented-library-subset) reuse the common
device services. The remaining bindings and storage reset are in the
[implementation plan](python.md).

ARM QEMU checks syntax/runtime errors, memory and stack exhaustion, GC, truncated
bytecode, maximum-size source, forced stop and repeated Python/native transitions,
including recovery after a native MPU fault. On one Robot Inventor F4, USB and BLE
checks cover source/bytecode, GC, exceptions and another run. A non-yielding Python
loop remains running across USB-to-BLE handover and can be stopped through BLE;
incomplete staging is discarded and the application power hold clears on stop.
These checks do not establish Pybricks API compatibility or hardware timing bounds.
The F4 hard-float image has USB/BLE checks for native ELF register preservation
across API calls, on-hub Native/Viper compilation, SDK format-7 execution and
dynamic C imports with float objects, exceptions and GC. Forced stop of a native
float loop permits another run; stored files remain unchanged. ARM QEMU also
checks all s16–s31 registers across exceptions and preempting threads, and checks write/execute protection, kernel
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
not a file. The Pybricks adapter uses it to select the first module while preserving
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

### Persistent user data

`hub.system.storage(offset, read=count)` and `storage(offset, write=buffer)`
access 512 shared bytes, initially zero. Reads and writes are bounded copies in
RAM through the native SDK's `bp_user_data_read()`/`bp_user_data_write()`. They
survive application exit and Python soft reset; they do not grant filesystem or
flash-controller access. Named programs and Pybricks slots share these bytes.

```python
from pybricks.hubs import PrimeHub

hub = PrimeHub()
hub.system.storage(0, write=b"hello")
print(hub.system.storage(0, read=5))
```

Dirty user data is saved at orderly hub shutdown, after applications and device
outputs stop. To save without switching off, return to the hub Shell and run:

```text
storage save data
```

The common LittleFS store holds the committed 512-byte `.userdata` file. A save
writes and closes a separate staging file before atomic replacement. A failed
save preserves the committed file and leaves RAM changes pending; changes made
during a save require another save. These internal files are excluded from
application listing and host file transfer. A missing data file starts with zero
bytes; an unreadable or wrong-sized file reports an error and is preserved.

An unexpected power loss, watchdog reset, `reboot` or firmware replacement can
lose unsaved RAM changes. `config save` and user-data saving are independent:
neither persists the other's pending changes. Reads outside the 512-byte user
region are rejected; adjacent private Pybricks settings/program memory is not
exposed. General writable Python files remain outside this interface.

On the Inventor F4, native USB/BLE checks cover read/write, explicit save, retained
data after reboot and loss of unsaved changes. Automatic saving during physical
shutdown remains unverified. The F4 native profile reserves a 4 KiB main stack
for shutdown's filesystem call path; the 512-byte snapshot is temporary stack
storage and the shared RAM data occupies 512 bytes independently of the VM.

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
draining the final queued output after termination. Native SMP additionally has
Inventor F4 USB/BLE checks for lossless output larger than the queue, deliberately
delayed reads and stop while output is blocked. Pybricks-client physical
flow-control acceptance remains open.

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

The CLI reads the existing bounded output log together with its remaining byte
count, execution state and errors in one SMP response. It reports any drops and
drains final output before announcing termination. An idle poll waits 50 ms;
busy output drains at the transport's acknowledged pace. These are host
interaction intervals, not device-service timing guarantees. REPL may be
combined with `--monitor`; monitoring rates are best-effort and its log records
missed polls when the shared transport cannot keep up.

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
and discarded partial input across a physical reconnection. USB/BLE CLI checks
also cover combined application monitoring, long output before/after reset,
reattachment, interruption and forced exit with queued input. BLE remains
bandwidth-limited; these checks establish lossless output and usable lifecycle
operations, not a guaranteed monitor rate or interactive latency under arbitrary
load.

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
exports. Its versioned ELF interface is independent of Zephyr configuration;
reject unsupported architectures, interface versions and required capabilities
clearly, before replacing a running application.

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
