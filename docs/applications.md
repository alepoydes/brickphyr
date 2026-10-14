# Applications and runtime contract

This document distinguishes the experimental native and Python implementations
from the remaining application model. The ABI remains experimental and versioned. Keep SDK definitions shared
with the firmware; do not copy an API description into multiple runtimes.

The [Python implementation plan](python.md) describes Python support and remaining
acceptance, including REPL, named programs, native `.mpy` and Pybricks clients.

## Native portability and resource efficiency plan

The [platform roadmap](roadmap.md) owns delivery order and the retained resource
optimization work. Complete SPIKE/Inventor F4 support and prepare the runtime for
Babybot while maximizing useful application memory. Preserve MPU isolation,
service responsiveness, supervised cleanup and stored data. Native Technic is a
secondary port; Python on Technic and WASM are outside this milestone.

Hard-float execution, optional CMSIS-DSP and the versioned ELF API independent of
Zephyr syscall numbering are existing foundations, described below. Continue
measuring loading peaks, stacks, buffers, reachable code and execution costs;
linked RAM includes allocatable reserves and is not the firmware's live working set.

The supervisor runs standalone native or Python applications, or one Python
application with an independent native controller. Both contexts belong to one
application lifetime. Their memory is separate; the controller does not depend on
VM objects or GC. See [concurrent control](#native-control-alongside-python).

## Native control alongside Python

The `brickphyr` Python module uses the existing native ELF loader. Arguments are
positional:

- `control_start(image, parameters=b"")` copies/links an ELF from a readable
  buffer and starts its `app_main()` in a separate protected thread. An ELF can
  be a resource in the ordinary named or RAM Python package. One controller is
  allowed; another start returns `EBUSY`. A rejected start leaves Python running.
- `control_update(parameters)` copies a complete replacement and returns its
  positive revision. The block's size is fixed by `control_start`, at most
  256 bytes. Updates replace the pending revision; intermediate revisions need
  not be observed. A controller receives the latest whole block at its next step.
- `control_status()` returns the last step's sequence, skipped releases,
  scheduled/start times, applied revision, pending revision, period and native
  stack/loader/loaded byte counts. The pending revision may be newer than the
  one applied. Before the first step its timing fields are zero.

Native ABI 1.4 adds `bp_control_wait(period_us, &step, parameters, capacity)`.
The first call fixes an absolute periodic schedule and releases immediately.
Periods must be positive and exactly representable by the system tick (100 us on
F4). Later calls sleep until the next release; late execution skips whole missed
periods and reports them in `step.skipped`. It never runs a catch-up burst.
`step.scheduled_us` and `started_us` use the input frame's monotonic timebase;
their difference includes dispatch delay. `sequence` counts executed steps.
The same call works in standalone ELF programs with an empty parameter block.
There is no allocation, formatting, storage or host I/O in this release path.

On F4 Python runs at priority 15 and the controller at 14; Shell/logging remain
at 13, below acquisition and management services. The controller preempts Python
and GC, while essential services can still preempt or stop either application
context. A controller that never yields can starve Python. This priority scheme
is not a deadline guarantee: application work, interrupts and driver delays must
be measured for the selected period. Motor command expiry remains enforced by
the independent port service; a missed step alone does not abort an application.
Sample-triggered release and bounded background jobs remain planned work.

The controller has its own dynamically allocated stack, MPU domain and loader
memory. Neither context can access the other's mutable memory; parameter bytes
are copied by the supervisor. Control ports use a separate owner from Python:
conflicting writes, motor setup and release return `EBUSY`, while read-only
snapshots remain available. Singleton hub services such as the matrix and speaker
still belong to the whole application; coordinate their use in application code.

Return or fault of either context ends the whole application. Explicit stop,
replacement and Python REPL soft reset also stop both contexts. The supervisor
joins both before releasing device claims, partitions, code, stacks and thread
objects. Normal native destructors execute in that context on return; forced stop
and faults do not rely on them. The first completion supplies the result/fault;
cleanup errors are reported separately and retain resources for retry. Ordinary
host disconnection leaves both contexts running. Keep Python alive while its
controller is needed; independent component restart is not provided.

[The read-only example](../examples/native/control_loop.c) records a 20 ms native
observation step beside [Python orchestration](../examples/python/control/main.py).
It records IMU, battery and optionally A/B encoders/output publications, without
energizing motors. Copy a matching-SDK `control_loop.elf` beside `main.py` in an
ignored build directory; then use the ordinary application commands:

```sh
mkdir -p build/control-demo
cp examples/python/control/main.py build/control-demo/main.py
cp build/native-examples/control_loop.elf build/control-demo/control_loop.elf
uv run brickphyrdev app run build/control-demo
uv run brickphyrdev app input 1
uv run brickphyrdev app output
uv run brickphyrdev app stop
uv run brickphyrdev app record reports/control-step.jsonl
```

Use the [SDK build procedure](builds.md#experimental-native-sdk) to produce the
ELF for the installed firmware. `app install build/control-demo --name control`
retains the same package in flash. Add `--ble Brickphyr` after `app` for Bluetooth.
The `brickphyr.control-loop/1` example schema contains the existing input record
and selected port mask. It opts into [control recording](#application-recording),
which attaches release times, sequence and the applied parameter values. The CLI
decodes these into JSONL and preserves raw bytes. The example is bounded read-only
observation; numerical replay and actuation correlation remain application work.

Protected M4/QEMU checks cover progress during Python GC/floating-point work,
whole parameter revisions, skipped periods, return, cross-domain faults, forced
stop and reclaimed heap. They do not measure physical scheduling latency.
On one Robot Inventor F4, a 20 ms read-only controller also runs beside Python
GC/floating-point work through USB and BLE; parameter updates, exit and subsequent
standalone execution pass. A short retained USB sample has no skipped releases,
with dispatch delay up to 5.84 ms and snapshot duration up to 3.53 ms. These are
observations of that workload, not worst-case bounds or a balancing budget.
Additional USB/BLE checks retain records during Python GC and monitoring, with
no skipped 20 ms releases in those short runs. BLE completes fewer telemetry
requests, so its timing cannot be treated as an equal-load USB comparison.
A synthetic Babybot predictor compiled unchanged for the SDK and desktop also
produces matching recorded float32 results in standalone and concurrent execution,
including parameter revisions. With control recording, the retained 64 of 96
steps also reproduce from their captured model and three applied revisions after
ring overwrite; USB/BLE retrieve identical records. This establishes bounded
numerical portability, not physical feedback control or complete robot replay.
Controller stack high-water and sustained combined-load characterization remain
open; the normal image has no kernel stack Shell.

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
the running application. The supervisor allocates the requested stack on demand,
with a 2048-byte runtime minimum and MPU rounding. The configured stack size is
the admission ceiling. A matching architecture alone cannot promise that any
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

On the current ARMv7-M targets, native execution allocates the loader's backing
buffer from the shared system heap when starting an application and release it
after successful cleanup. The requested size is the validated image's loader
budget, bounded by the configured execution-memory limit. Native LLEXT allocations
are aligned and protected individually. Python retains its own fixed, power-of-two,
size-aligned arena as one MPU partition. Native execution never uses that arena,
including in Python-enabled builds. The arena has no flash initialization image;
both engines initialize their allocations and accept arbitrary initial contents.

The native admission budget includes merged ELF region spans, MPU alignment,
symbol/string tables and loader bookkeeping. It excludes metadata that is only
streamed or skipped; gaps inside a merged region still require memory. The budget
is a conservative reservation, not a measurement of the loader's peak usage.

A native replacement reserves its new buffer before stopping the old
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
and a 214144-byte loader budget in a 235264-byte shared system heap. The Technic native
profile permits 4 KiB images and a 16 KiB loader budget in a 25 KiB shared heap.
An image limit does not reserve upload space or guarantee that the loaded image
fits; another retained upload or firmware allocation can leave insufficient heap.
Alignment, symbols and scratch space count against the loader budget; small
images with large BSS can still exceed it. The native-only profile permits a
2048-byte user stack. Its privileged syscall stack, guard and allocation metadata
also come from the shared heap. The supervisor reserves a replacement's stack
before stopping the current program; insufficient heap or alignment space leaves
that program running. It joins the thread before freeing the stack on return,
fault or explicit stop. `app status`
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

### Control input frame

`bp_control_frame_get(port_mask, &frame)` reads the current calibrated IMU,
battery and selected encoder/output publications in one native call. Bits 0–5
select A–F; zero reads only the hub sources. Caller-owned storage holds the
result. The operation does not allocate, acquire a device, change modes, perform
I/O or start a motor observer. This is the first input boundary for the
[independent control runtime](roadmap.md), not its scheduler or cycle recorder.

Each source retains its own validity and time. IMU data-ready/read times are in
microseconds; encoder parser completion, battery conversion completion and motor
driver completion remain in milliseconds. Capture start/end times describe the
API read, not the physical acquisition. `capture_resolution_us` reports their
quantization: one microsecond on F4, or the system tick on targets without a
64-bit cycle counter. The sources are not simultaneously
sampled. The encoder and output of each selected port are copied together under
that port service's existing lock. Current connection, accepted request and last
completed output have explicit generation identifiers. After reconnect, an old
output confirmation reports `ESTALE` and `valid=0` until a new output completes;
reading the frame never applies a new output merely to refresh its timestamp.

Check `imu.error`, `battery.error` and each selected port's `error` before using
measurements. Output has separate `output_error`, `cleanup_error`, `valid` and
`pending` fields: a passive motor can have valid output without encoder feedback.
An accepted pending request does not replace the last driver-confirmed duty.
No field measures winding voltage or current. Compare IMU epoch and encoder
generation/sequence, and choose an age limit appropriate to the controller;
the service's connection timeout is not a control deadline.

A successful call returns zero even if some sources are unavailable. Disabled
services report `ENOTSUP`, absent ports report `ENODEV`, and an invalid mask
returns `EINVAL` before touching output storage. Unselected port entries are
zero and must not be interpreted as valid observations. Errors retain source
timestamps where the underlying service retains its last sample.

The [control-frame example](../examples/native/control_frame.c) reads A/B twice
without moving them, and reports source times, errors, generations and applied
outputs. It uses the same upload/run path as other SDK examples. Its pause and
text output are illustrative, not a periodic controller. The API is appended in
native ABI 1.1; older firmware rejects that SDK's images before replacement.

The exported SDK example has bounded USB/BLE checks on one Robot Inventor F4
(PCB revision unknown), using the full Python image and two idle ID-75 encoder
motors. Both paths return changing IMU/encoder sequences, battery data and the
expected stale output-generation indication without acquiring the motors.
Stored programs remain unchanged. The read is preemptible: capture duration is
observable, but no control-loop deadline or simultaneous-sampling guarantee is
provided. Sensor accuracy and sustained combined-load timing remain unverified.
A bounded read-only probe also runs while the host requests cached IMU, encoder
and battery snapshots over USB or BLE. USB traffic can extend a capture beyond
5 ms even with batched SMP transmission. That experiment is not an admission
budget for a controller; see [independent control](#native-control-alongside-python)
for the execution mechanism and its remaining timing acceptance.

## Application recording

Native ABI 1.3 adds `bp_record_begin(&spec)` and `bp_record_write(data, length)`.
An application configures one fixed-size binary record format before periodic
work. `spec` names its schema and version, record size (1–1024 bytes) and retained
record count. The total buffer may not exceed 32768 bytes. The supervisor
allocates it from the shared system heap while the caller waits; a failed
allocation reports `ENOMEM`. A second successful configuration in the same run
is rejected. Applications that do not record allocate no recording buffer.

Native ABI 1.5 adds `bp_record_begin_config(&spec, config, length)`. It copies up
to 1024 bytes of immutable experiment configuration together with the buffer;
configuration and records share the 32768-byte allocation limit. The record
schema defines the configuration's binary format too. Use it for model values,
initial state, units and configuration identity needed for replay. Ring overwrite
does not erase it. This configuration is not a mutable parameter store.

Native ABI 1.6 adds optional `bp_control_record_begin(&spec, config, length)`.
Use it instead of the ordinary begin operation before the first control wait.
Each `bp_record_write` then automatically retains the latest successful
`bp_control_wait` step: release/start times, sequence, missed releases and the
parameter block delivered at that boundary. Until the first successful wait after
begin, writes return `ENODATA`. Several records may describe one step. The payload and
configuration formats still belong to the application; step metadata is separate.

Parameter values are copied on revision changes and shared by records with the
same revision. They survive until the last referring record is overwritten, even
if the revision's first record has already gone. Pending Python updates are not
substituted for applied values. Application changes to its private parameter copy
or other internal state must be recorded in the payload when needed for replay.
Standalone native execution has revision zero and an empty delivered parameter
block; put its application-defined constants in the configuration.

The control mode preallocates bounded metadata, parameter slots and staging space
within the same 32768-byte limit. An over-budget request returns `EINVAL` before
allocation. Eviction/reuse takes constant work without scanning the ring, and no
allocator, transport or storage operation runs in a control step. Ordinary
recordings reserve none of these optional buffers. Control step sequence exhaustion
returns `EOVERFLOW`; it does not silently wrap to a previous sequence.

Each write copies one complete record and returns without allocation, transport
work or waiting for a reader. Wrong lengths are rejected before modifying the
buffer. When full, the buffer replaces its oldest record. Monotonic 64-bit record
numbers expose the missing interval; sequence exhaustion returns `EOVERFLOW`.
The application owns the binary schema and must include the measurements,
commands, parameter revisions and times needed to interpret its calculation.
The firmware does not infer those values from a later sensor read.

Return, fault and forced stop freeze the retained tail for retrieval. Normal
host disconnect does not stop recording. Starting another application, Python
soft reset or explicit clearing discards it and changes the recording generation.
A rejected image does not clear a running application's records. The retained
buffer consumes heap until replacement or clearing, even after application exit;
clear it before admitting a large replacement when memory is constrained.
Records are volatile and do not survive firmware reset or loss of power.

`bp_record_trigger(after)` marks the boundary after the last recorded frame and
freezes recording after exactly `after` further writes. The buffer must already
contain a frame, and `after` must be less than its capacity, retaining at least
one frame of prehistory. Zero freezes immediately; a second trigger returns
`EALREADY`. After completion, writes return `ENOSPC` without changing the capture.
The application continues running and may stop calling the recorder. An early
application exit retains the available frames and identifies the incomplete
capture. A trigger represents an application-defined event, not a firmware
reset or power-loss detector. Rearming currently requires a new application run.

The same SMP operations retrieve records over USB and BLE. Reads do not consume
them and can be repeated. Large records use bounded fragments; every response
reports the current generation and retained index range. A client must discard
a partial record if it was overwritten, and must not combine generations.
Generations identify runs within one boot, not persistent experiment identities.

```sh
uv run brickphyrdev app run build/native-examples/control_record.elf
uv run brickphyrdev app record control.jsonl
uv run brickphyrdev app --ble Brickphyr record control-again.jsonl
uv run brickphyrdev app record-clear
```

Use the matching SDK to build that example. `app record` creates a new JSONL
file, refusing to overwrite an existing one before connecting. It collects the
retained tail and follows a live application until it ends or the user interrupts
collection. It does not stop that application. Session metadata identifies the
firmware, board, ABI and application status; each record retains its exact binary
bytes as hex. Explicit gap events mark missing record ranges. A final event
distinguishes completed collection from cancellation or transport failure, and
completed collection includes current or terminal application result, fault and
cleanup status.
Clearing a live application's recording returns `EBUSY`.

To trigger a capture from the computer, use `app record FILE --after N`.
It binds the trigger to the current run and collects the retained prehistory
and the next N frames through the same USB/BLE connection. For example:

```sh
uv run brickphyrdev app run build/native-examples/control_loop.elf
uv run brickphyrdev app record reports/control-event.jsonl --after 8
uv run brickphyrdev app status
uv run brickphyrdev app stop
```

Here the read-only controller continues after capture. Its configuration is one
little-endian `uint32` containing the requested period in microseconds.
JSONL session metadata retains configuration bytes as hex. A `recording-end`
event identifies a completed trigger capture and the current application status;
it does not mean the application stopped. If the application ends first,
`application-end` includes the trigger boundary, requested count and incomplete
capture status. Collection never silently combines configurations from different runs.

For control recordings, each JSONL `record` also has a `control` object with the
step fields above. A `parameters` event precedes the first collected record using
each revision and holds its exact bytes as hex. The collector fetches each observed
revision once; it discards an incomplete record if its parameter snapshot was
overwritten before retrieval and reports the gap. A replay must use these values
and the application schema, not current hub settings or a later sensor read.

The read-only [example](../examples/native/control_record.c) records 40 observations
of the IMU, battery and ports A/B, retaining the last 32 in 5632 bytes. Its
`brickphyr.control-input/1` schema is a 176-byte little-endian layout; the CLI
also decodes its vectors, source errors/times, connection generations and last
driver-confirmed motor outputs into JSON values. It neither claims nor drives
motors. Its requested 20 ms observation interval is illustrative, not a control
deadline. Collection begun after completion reports the first eight observations
as a gap. Arbitrary disconnection cannot preserve an unbounded history in RAM.

The example has bounded checks on one Robot Inventor F4 (PCB revision unknown):
live USB collection retains all 40 observations, and USB/BLE retrieve identical
retained tails after completion. No motors are energized. ARM/SMP checks cover
fragmented records, overwrite, forced stop, reconnect and explicit clearing.
Protected ARM checks also cover copied configuration, native triggering,
preserved prehistory, post-trigger writes, continued execution and early stop.
Control recording checks cover applied parameter values across ring overwrite,
shared revision retention, slot recycling and memory release after stop/fault.
On the same F4, `control_loop` retains its period configuration and identical
USB/BLE event captures while continuing execution. A BLE immediate capture also
passes with the native controller running alongside Python; later controller
progress leaves the frozen records unchanged.
These checks do not establish lossless streaming at arbitrary rates or a control
deadline under combined load.

### Recorded group command

The [group example](../examples/native/control_group.c) records its original A/B
input, one paired command and subsequent driver observations. `control_group.elf`
drives A/B at opposite 20% duty with a 100 ms absolute expiry; clear both
mechanisms before running it. `control_group_coast.elf` builds the same code with
passive outputs only and can check the recording path without energizing motors:

```sh
uv run brickphyrdev app run build/native-examples/control_group_coast.elf
uv run brickphyrdev app record group.jsonl
uv run brickphyrdev app --ble Brickphyr record group-again.jsonl
uv run brickphyrdev app record-clear
```

Its `brickphyr.control-group/1` records are 384 bytes each, with four retained
slots (1536 bytes). The first 176 bytes use the input schema above and remain
unchanged throughout the command. Later fields contain the requested duties,
connection generations, deadline, admission result, group state and per-port
driver outputs. The CLI decodes these into `input`, command fields, `group` and
`ports` while preserving the binary bytes. `phase` identifies the command attempt
(0), observed active output (1), completion (2) or failed wait (3). Invalid source
input is recorded before returning without sending a motor command.

Group and output reads are separate snapshots. Match the command and group
request IDs, then each port's `completed`/`generation` against its
`group_request`/`group_generation`; a later coast has a different completion ID.
Check admission, read, output and cleanup errors before interpreting successful
output. The sample's 1 ms polling may miss an intermediate phase under load; an
absent applied observation is not evidence that no output occurred. Driver
completion timestamps do not measure physical voltage or shaft response.
This is one command experiment, not a scheduled controller or a lossless trace
of every driver transition.

Protected ARM/SMP checks exercise opposite duties, expiry/coast, the passive
variant, input rejection and host decoding. One Robot Inventor F4 (PCB revision
unknown) has checks of the passive variant and identical USB/BLE record retrieval;
physical group drive and expiry remain unverified. The example adds no resident
firmware worker or memory reservation. Automatic control-step correlation,
including attribution of actuation completion to the original input, autonomous
flash recording and numerical replay remain [roadmap work](roadmap.md).

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
and forced stop join all application contexts before releasing their resources.
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
allocation to 16 KiB. This allocation exists only while Python is loaded; a
native application in the same image uses its own ABI requirement. Stack checking
uses the actual lower bound with a 512-byte MicroPython margin. These are
admission limits, not guarantees that every image
within them has enough compiler memory or stack. Status reports an execution error
or protection fault when applicable.

Brickphyr's main supervisor and dedicated service threads use kernel-only stacks
with their configured byte budgets. They do not reserve a second privileged stack
for a user-mode transition.
The active application allocates its MPU-aligned user stack and privileged
syscall stack from the shared heap; idle firmware reserves neither.
Linker RAM totals also include alignment gaps and kernel-object metadata; they
are not a measure of free system heap or Python GC capacity.
Firmware services use queues and events; the unused Zephyr asynchronous mailbox
pool is disabled. Native applications access services through the SDK rather than
kernel mailboxes.

F4 UART state and port-mode descriptions borrow from the shared system heap while
devices are connected. Empty and passive ports need no UART allocation; each
active UART link occupies 944 bytes plus allocator overhead, including its
independent 512-byte RX buffer. Compact error diagnostics remain after disconnect.
The native profile provides a 235264-byte heap, including on-demand loader buffers
and stacks. The full Python profile provides 134272 bytes and the core Python
profile 164352 bytes; both retain a 64 KiB Python arena. Active stacks and native
loader buffers, including a concurrent controller, come from that heap.
These F4 profiles assign available linker RAM to
the shared heap, below the 319 KiB firmware RAM boundary; the loader-selector
SRAM remains reserved.
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

Motor controllers borrow from this heap only when observation or regulated
control is requested. Each F4 controller occupies 436 bytes plus allocation
overhead; empty ports, sensors and direct-duty-only motors need no such record.
The worker retires released records after completing its current computation.
Published observations/settings and retained logs keep their existing lifetimes.
This recovers idle capacity for applications; when all six controllers are live,
their allocations include overhead that the former static records did not have.

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
the sparse address lookup table resides in flash. F4 profiles place any static
userspace stacks before ordinary data to avoid an alignment gap after BSS.
Application stacks instead use protected heap allocations. Recovered static RAM
is assigned to the shared heap. These layouts require the
[Zephyr sizing changes](upstream.md#kernel-object-sizing-prerequisite) and
[dynamic-stack correction](upstream.md#dynamic-application-stacks).
ARM emulation checks cover lookup, permissions, protected stacks and application
lifecycle; physical combined-load acceptance of the enlarged heaps remains pending.

On one Robot Inventor F4, the full Python image runs native ELF with a 2 KiB
stack and Python with a 16 KiB stack over USB/BLE. USB checks also cover a
32 KiB source and a freshly installed native program. Existing files retain
their contents. These standalone checks do not establish sustained memory-pressure
behavior; the separate concurrent checks are described above.

The optional [memory diagnostics profile](builds.md#memory-diagnostics) includes
Zephyr's `kernel heap` Shell command. It reports
current free/allocated system-heap bytes and the allocation high-water mark since
boot. This heap holds uploads, receive-buffer growth, executable native code and
application stacks; its free bytes are not the Python GC budget. In REPL, `gc.mem_free()` and
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
80 KiB system heap, including the on-demand stack, and a 64 KiB execution arena;
it does not establish F4 USB/BLE driver overhead or other programs' compiler needs.

On one Robot Inventor F4 (PCB revision unknown), the full Python profile with
Zephyr's optional memory counters also runs a synthetic 32,768-byte file map
from RAM and LittleFS over USB. Most of the image is an unused resource; the
program imports a dynamic C module and creates default HubNetwork inboxes without
connecting peers. With Zephyr 4.4.2, Zephyr SDK 1.0.1 and the earlier 64 KiB
system-heap configuration with a separate static application stack, the observed
budgets were:

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
the complete file set is copied into the Python execution arena alongside the GC
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

One application executes at a time. The current implementation selects one
enabled engine from its format; the planned native-control/Python extension
adds two execution contexts within that same application lifetime. Standalone
native applications do not initialize MicroPython or WAMR. Runtime bindings use
the shared device services; WASM remains unimplemented.

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
