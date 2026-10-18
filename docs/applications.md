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

The supervisor runs standalone native or Python applications, or a native/Python
primary context with an independent native controller. Both contexts belong to one
application lifetime. Their memory is separate; the controller does not depend on
VM objects or GC. See [concurrent control](#native-control-and-background-computation).

## Native control and background computation

The `brickphyr` Python module uses the existing native ELF loader. Arguments are
positional:

- `control_start(image, parameters=b"")` links an ELF from a readable buffer or
  a string naming a bundled resource and starts its `app_main()` in a separate
  protected thread. Prefer `control_start("controller.elf", parameters)` for
  resources: it reads the package directly, without an ELF-sized Python object. One controller is
  allowed; another start returns `EBUSY`. A rejected start leaves Python running.
- `control_update(parameters)` copies a complete replacement and returns its
  positive revision. The block's size is fixed by `control_start`, at most
  256 bytes. Updates replace the pending revision; intermediate revisions need
  not be observed. A controller receives the latest whole block at its next step.
- `control_status()` returns the last step's sequence, skipped releases,
  scheduled/start times, applied revision, pending revision, period and native
  stack/loader/loaded byte counts. The pending revision may be newer than the
  one applied. Before the first step its timing fields are zero. A positive
  sequence with `period_us == 0` identifies sample-triggered execution.

Native ABI 1.4 adds `bp_control_wait(period_us, &step, parameters, capacity)`.
The first call fixes an absolute periodic schedule and releases immediately.
Periods must be positive and exactly representable by the system tick (100 us on
F4). Later calls sleep until the next release; late execution skips whole missed
periods and reports them in `step.skipped`. It never runs a catch-up burst.
`step.scheduled_us` and `started_us` use the input frame's monotonic timebase;
their difference includes dispatch delay. `sequence` counts executed steps.
The same call works in standalone ELF programs with an empty parameter block.
Step metadata and delivered parameter bytes must be disjoint; overlap returns
`EINVAL` before changing outputs or advancing the control clock.
There is no allocation, formatting, storage or host I/O in this release path.

Native ABI 1.9 adds `bp_time_us()` on the same monotonic timebase. Use it after
a control wait and after application computation to measure the complete path:
`started_us` precedes parameter copying, recorder staging and syscall return.
The clock uses hardware cycles where a 64-bit counter exists, otherwise system
ticks; the input frame's `capture_resolution_us` describes its quantization.
Reading it has syscall/clock overhead and does not establish a deadline bound.

Native ABI 1.8 adds `bp_control_imu_wait(port_mask, timeout_ms, &step, &frame,
parameters, capacity)` as an alternative. The first successful wait selects the
release mode for that native context; switching between periodic and IMU waits
returns `EINVAL`. The sample wait returns the latest fresh, distinct IMU
publication together with cached battery and selected port observations. The
first call may use an already available fresh sample. Later calls sleep until
its raw sequence changes; they do not poll or replay an accumulated sample queue.
The returned frame retains the exact IMU observation which released the step,
even if acquisition advances while the other sources are copied.
Frame, step metadata and delivered parameter bytes must be disjoint. An overlap
returns `EINVAL` before writing outputs, waiting for a sample or advancing the step.

`scheduled_us` is that observation's data-ready edge, so the difference to
`started_us` includes acquisition, processing, input copying and dispatch delay.
`skipped` counts intervening raw ready edges since the preceding successful step
(zero for the first), including samples lost by acquisition or skipped by a slow
controller. Check the IMU epoch before integrating across discontinuities. The
frame's capture interval includes time spent waiting; its individual source
timestamps keep their existing meanings. Parameters and recording metadata use
the same release boundary as periodic execution.

The timeout is finite, from 1 to `INT32_MAX` milliseconds. A stale successful
publication waits for new data until that deadline. Initialization without a
sample also waits; published acquisition/processing failures return their error.
Timeout returns `ETIMEDOUT`. A failed call does not advance the step or apply new
parameters, and the caller chooses whether to retry or stop. Stop/fault handling
can abort the sleeping context without leaving application memory in the IMU
worker. Independent motor expiry remains necessary while waiting or recovering.

On F4 the primary runs at priority 16, the shared storage worker at 15 and the
controller at 14; Shell/logging remain at 13, below acquisition and management
services. Storage can progress even when the primary never yields; equal-priority
time slicing alone cannot ensure this under frequent higher-priority wakeups.
The controller preempts both storage and the primary
(including Python GC), while essential services can still preempt or stop either application
context. A controller that never yields can starve the primary. This priority scheme
is not a deadline guarantee: application work, interrupts and driver delays must
be measured for the selected period. Motor command expiry remains enforced by
the independent port service; a missed step alone does not abort an application.
Python can run bounded C numerical jobs through a matching-SDK native `.mpy`.
That work remains in Python's lower-priority protected context, so the controller
can preempt it without a Python yield or GC cooperation. It does not create an
additional worker. Application code owns job size, candidate validation and
rollback; `control_update` publishes a complete accepted parameter block. General
combined-load timing acceptance remains open.

The controller has its own dynamically allocated stack, MPU domain and loader
memory. Neither context can access the other's mutable memory; parameter bytes
are copied by the supervisor. Control ports use a separate owner from the primary:
conflicting writes, motor setup and release return `EBUSY`, while read-only
snapshots remain available. Singleton hub services such as the matrix and speaker
still belong to the whole application; coordinate their use in application code.

Return or fault of either context ends the whole application. Explicit stop,
replacement and Python REPL soft reset also stop both contexts. The supervisor
joins both before releasing device claims, partitions, code, stacks and thread
objects. Normal native destructors execute in that context on return; forced stop
and faults do not rely on them. The first completion supplies the result/fault;
cleanup errors are reported separately and retain resources for retry. Ordinary
host disconnection leaves both contexts running. Keep the primary alive while its
controller is needed; independent component restart is not provided.

### Native primary context

Native ABI 1.10 exposes `bp_control_start`, `bp_control_update` and
`bp_control_get` to the primary ELF. The Python bindings call these same services;
no Python engine is required for native background work. Each ELF has its own
individually allocated loader regions, stack and MPU domain. A failed second-image
load leaves the primary intact. The shared supervisor still joins both before unloading either.

Start the controller before the primary uses control waits or initializes a
recording; otherwise start returns `EBUSY`. With a controller running, only that
context may advance the control clock or write/trigger the recorder. Competing
primary calls return `EPERM`. The primary may read observations and publish
parameters with its lower priority, without blocking the controller on its work.

`bp_record_get` returns recording metadata. `bp_record_readinto` takes a
`bp_record_request` containing generation/index and caller-owned data/parameter
buffers. It copies one complete record, its applied parameters and step metadata;
readers have independent cursors. Its buffer, gap and generation rules are the
same as the Python calls below. It returns the payload length, or zero when the
next observation has not been written. Payload, parameter and metadata output
regions must be disjoint; overlap returns `EINVAL` before any output is written.
Neither reading nor updating allocates.

The [native background example](../examples/native/control_background.c) starts
the existing `control_loop` controller, reads 24 observations and changes the
observed port mask at a step boundary. It sends no actuator commands. Build
`control_background` with the [ordinary SDK procedure](builds.md#experimental-native-sdk):

```sh
uv run cmake --build build/native-examples --target control_background control_loop -j4
uv run brickphyrdev app install build/native-examples/control_loop.elf --name control_loop
uv run brickphyrdev app run build/native-examples/control_background.elf
uv run brickphyrdev app output
uv run brickphyrdev app record reports/native-control.jsonl
```

Add `--ble Brickphyr` after `app` for Bluetooth. The primary calls
`bp_control_start_file("control_loop", 12, parameters, size)` to load the installed
controller through the same validator and LLEXT linker as a buffered start. The
store remains locked through validation/linking; no complete controller ELF is
embedded in the primary or retained after linking. Missing files return `ENOENT`,
invalid images fail admission, and a rejected start leaves the primary alive.
Without application storage the call returns `ENOTSUP`. Initial parameters,
independent protection and the common stop/fault lifetime are unchanged.
Real numerical algorithms, candidate validation and rollback remain application code.

Protected ARM checks cover native primary progress, copied revisions,
cross-domain faults, forced stop and complete heap reclamation, including a
firmware configuration without Python. On one Robot Inventor F4, the SDK example
also passes through USB and BLE with real IMU/passive port observations and both
parameter revisions retained. It makes no motor commands. This checks the shared
native lifecycle and transport path, not a numerical workload's deadline budget.

### Reading observations from Python

The controller can use the existing [recording channel](#application-recording)
to supply bounded observations to Python. No separate queue is allocated, and
Python and USB/BLE readers have independent cursors; reading does not consume data.

- `brickphyr.record_info()` returns `generation`, `first`, `next`, `size`,
  `capacity`, `schema`, `parameter_size`, `control` and `active`. It raises
  `OSError(ENODATA)` until the application initializes recording.
- `brickphyr.record_readinto(generation, index, data, parameters)` copies one
  complete retained record into writable caller-owned buffers. For control
  recordings it copies the parameters actually applied to that record, together
  with its step metadata, under one recorder lock. Buffers must not overlap and
  must hold at least `size` and `parameter_size` bytes. Larger buffer tails are
  untouched. Use an empty `bytearray()` when there are no parameters.
- A successful read returns `control` and `size`. For a control record it also
  returns `sequence`, `skipped`, `scheduled_us`, `started_us`, `applied_revision`
  and `parameter_size`. With valid buffers, reading `index == next` returns
  `None` immediately; it does not wait for a future observation.

Generation and index are unsigned 32-bit and 64-bit integers; values outside
those ranges raise `OverflowError`. A changed generation returns `ESTALE`, an
overwritten index `EOVERFLOW`, an index beyond `next` `EINVAL`, and a short buffer
`EMSGSIZE`. These codes, including `ENODATA`, are available in Python's `errno`
module. These errors leave both payload buffers unchanged. The application
must handle missing observations explicitly; the reader never silently skips
ahead. `record_info()` is a snapshot: the ring can advance before the next read.

For example, after the controller has initialized a control recording and written
at least one observation:

```python
info = brickphyr.record_info()
data = bytearray(info["size"])
parameters = bytearray(info["parameter_size"])
step = brickphyr.record_readinto(info["generation"], info["next"] - 1,
                                data, parameters)
# Decode data using info["schema"]; parameters belongs to this exact step.
```

Reuse these buffers for successive reads. Payload copying allocates no firmware
buffer; returned metadata dictionaries allocate in Python. The copy is bounded
by the recorder's 1024-byte record and 256-byte parameter limits. Its critical
section and the application's processing cost still belong in the timing budget.

### Concurrent execution example and evidence

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
observation. The separate recorded actuation example below correlates periodic
commands with their originating steps; robot-specific replay remains application
work.

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
Sustained combined-load and worst-path stack characterization remain open.
The normal image reports stack use through application status; it does not need
the diagnostic kernel Shell.

The same Inventor F4 runs Babybot's existing `learning_begin`, `learning_rows`
and `learning_update` C routines as a background `.mpy` module. Python reads 30
synthetic observations from the production recorder, calculates a complete
60-byte candidate model and publishes it to the native predictor. USB/BLE runs
spend 351/382 ms processing the batch and updating the model while the 20 ms
controller advances by 17/19 steps. Cumulative counters show no skipped releases
through the first applied update; maximum observed dispatch delays are 1510/403 us.
Host replay uses the recorded observations, configuration and applied models:
the learned model/history match, and all 31 retained predictions match bit for bit.
These short runs use synthetic data and do not establish physical control timing
or complete robot identification acceptance.

The learning workspace is 3032 bytes with a 544-byte history buffer. After return,
observed Python/controller stack use is 5768/776 bytes from 16384/4096-byte
allocations. Native `.mpy` code uses 11388 bytes of its 16 KiB reservation. These
are workload observations, not worst-case resource bounds. A separate longer
information-diffusion C call also permits native progress and host stop; the USB
stop round trip plus status read takes about 7 ms, followed by a successful fresh
Python/controller run. Neither probe commands actuators.

### Native learning workload

On the same Inventor F4, the original numerical learning functions also run as
a native primary ELF in the full firmware, with Python present but inactive.
A separate native context reads
real IMU, battery and passive A/B encoder snapshots every 15 ms. Learning uses
synthetic observations; it does not identify the connected robot. The 30-row
fixture uses 3/5/4 ms intervals; the larger fragmented fixture uses 15 ms. Complete
60-byte model revisions pass through the common control API and recorder.

| Bounded workload | Learning time | Largest release-to-post-record timestamp interval |
| --- | --- | --- |
| Six 30-row updates, USB | 437–444 ms per update | 1790 µs |
| Same ELF and inputs, BLE | 425–432 ms per update | 854 µs |
| One 320-row fragmented update, USB | 6925 ms | 2121 µs |

All three runs have no skipped periodic releases through the first recorded
application of the final candidate. The applied models and final information
history match the same C calculation on the host bit for bit. The timestamp
interval includes observation copying, optional recording, preemption and clock
reads; final diagnostic bookkeeping follows it. The loop only acquires sensors,
so these measurements do not establish an observer/actuator deadline. Host output
polling is best-effort and does not impose equal USB/BLE traffic.

A further bounded F4 check adds the original state observer to the 15 ms native
context while six 30-row learning updates run in the native primary. Both use
synthetic observations at 15 ms spacing; passive hardware acquisition also runs,
but its values do not drive the numerical model. USB/BLE runs have no skipped
periods through the final applied update. Their largest recorded intervals from
scheduled release through record completion are 5692/6741 µs. The last 16 retained
observer results in each run, and the final learned model/history, match the same
C computation on the host bit for bit. Observed controller stack use is 2696 of
8192 bytes. This includes observer computation and concurrent learning, but no
actuator commands, physical identification or worst-case deadline guarantee.

A separate read-only F4 USB check feeds the observer with actual calibrated IMU
and passive A/B encoder observations. All 128 periodic steps at 15 ms complete
without skipped releases; the largest scheduled-release-to-record-completion
interval is 2465 µs. The last 32 retained observations reproduce unit/coordinate
conversion, consecutive observer states and results bit for bit using the same C
code on the host. The record includes source times, errors and connection/reference
identities. This uses a fixed coordinate configuration and a synthetic model,
without learning or actuation; robot mounting, model accuracy and combined-load
deadlines remain unverified.

An extended passive workload uses 16 actual IMU/encoder observations for the
existing C learning kernel, then applies its complete candidate to the independent
observer. Native-primary USB and Python-primary USB/BLE checks pass on the same
Inventor F4. A Python load check repeats that calculation 48 times on the same
window, with 48 GC cycles and best-effort IMU/port telemetry. During learning and
GC, the 15 ms controller advances 170/166 steps over USB/BLE without skipped
releases or reported acquisition errors. The largest recorded scheduled-release
to post-record intervals are 3251/2670 µs; the 30 retained observations, applied
parameters and candidate model replay bit for bit on the host. These timings
include preemption and recording, but precede final diagnostic bookkeeping.
USB forced stop during the load completes in a 5.53 ms host round trip, retains
the recording and permits both contexts to restart without a cleanup error.

The full profile accommodates the 30-record, 904-byte payload ring alongside
the Python arena, native module and controller. These short runs start with a
synthetic model and issue no motor commands. Repeating one observation window
exercises computation and lifetime, not identification on successive new data;
physical control, sustained timing and worst-path memory bounds remain open.

The larger workload accumulates ten independent 30-row synthetic fragments and
one 20-row fragment at 15 ms spacing using the existing learning implementation.
It reuses a 1440-byte scratch tape, a 3032-byte learning workspace and a 544-byte
history. Its 19144-byte ELF has a 31675-byte admission budget; LLEXT reports
26837 loaded bytes. The controller separately reports 7381/2559 bytes. The budget
is no longer retained as an arena: individual allocations share the system heap.
Observed primary/controller stack use is 6120/648 bytes from 16384/2048-byte
allocations. These figures exclude the source upload, recorder, kernel thread
metadata and other platform services; loaded bytes are not a peak heap measurement.

With the full profile's 199808-byte shared heap and no idle Python reservation,
a separate 18592-byte USB capacity probe also runs on the Inventor F4. It reserves
a 320-row BSS array but computes only six 30-row updates; each result matches the
same host C calculation, with no skipped controller releases. LLEXT reports
56203 loader bytes and 51373 loaded bytes; primary/controller stacks are
16384/2048 bytes. Cold start and a start after Python cleanup both pass. This
establishes storage capacity for that fixture, not computation of all 320 rows.
The identical ELF previously failed stack allocation with a permanent Python
arena and a 134272-byte shared heap, despite about 60 KiB total free space.
Contiguous space and MPU alignment still constrain other workloads.

Processing those 320 rows takes longer than their 4.8 s input budget. Applications
must choose retention and update frequency to match available computation;
independent execution does not promise processing of every acquired observation.
The system-heap backend now returns alignment padding outside regions to the
shared heap. MPU rounding inside regions and the active Python arena's size and
alignment remain costs.
Clear an already-collected recording before admitting a large application when
its retained allocation prevents loading; a rejected start preserves it.

## Experimental native profile

The opt-in F4 and Technic profiles run one ARM ELF32 relocatable C image through
Zephyr LLEXT. Technic support is build-only; see its
[profile limits](builds.md#experimental-technic-base).
The SDK emits calls through a versioned, read-only firmware API table. Its resident
wrappers enter the existing Zephyr syscall verifiers; applications do not embed
Zephyr syscall numbers. Only the current versioned API table (`bp_api_v2`) may be
imported. TLS, other unresolved
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
application stack in these runs. The earlier arena allocator reserved 10214 bytes;
that admission budget was not a measured peak heap allocation. Sensor accuracy, fixed-rate timing
and sustained combined load remain unverified.

### Portable ELF ABI

`.brickphyr.abi` carries a 20-byte, little-endian record: the `BPA1` magic,
16-bit major/minor versions, then 32-bit architecture, required-capability mask
and stack requirement. The ABI uses fixed-width public structures, 32-bit
pointers and ARM EABI5/AAPCS. F4, Technic and the Cortex-M4 simulator use
ARMv7E-M Thumb with FPv4-SP-D16 hard float. Software-float ELF applications
are not supported.

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

The current version is 2.5. It adds the paused variable recorder and shared
capture-frame packer described below. Version 2.4 added paused fixed recording;
2.3 added explicit [Brickphyr settings saving](settings.md#saving-from-an-application).
Version 2.2 added controller loading from a named installed ELF; 2.1 added
explicit application user-data saving. ABI 2.0 through 2.2 ELF
applications retain their existing table slots and remain supported.
Version 2 retains each grouped motor command's per-port application and cleanup
times instead of relying on mutable output snapshots.
ELF files built for ABI 1 must be rebuilt with the matching SDK; they are rejected
before replacing a running application. Firmware updates preserve stored files
but do not convert their ABI. Python source and compatible `.mpy` formats are
unaffected by this ELF version change.

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

On the current ARMv7-M targets, LLEXT allocates each native region and its metadata
from the shared system heap, returning them individually after successful cleanup.
Alignment padding outside each allocation remains available to other users of
that heap. Native regions retain their individual MPU permissions. Active Python
allocates a 64 KiB, power-of-two, size-aligned arena from the same heap and maps it
as one MPU partition after zeroing it. Native execution never uses that arena,
including in Python-enabled builds; idle and native-only execution hold no Python
arena. A live Python replacement reuses it only after the previous threads have
joined and their domain mappings have been removed, without a second arena. It
is freed after successful device, domain and thread cleanup. The arena has no
flash initialization image. ARM checks cover replacement, fault cleanup and heap
reclamation. Inventor F4 USB/BLE checks cover live Python replacement, native-code
REPL reset and stop; the USB capacity check above also runs after Python cleanup.

The native admission budget includes merged ELF region spans rounded for the MPU,
symbol/string tables and loader bookkeeping. It excludes metadata that is only
streamed or skipped; gaps inside a merged region still require memory. This
conservative bound is checked against the configured execution-memory limit; it
is neither a reserved buffer nor a measurement of peak heap use. Aligned allocation
also needs sufficient contiguous free space, so admission cannot guarantee loading.

A native replacement completely loads and links its regions and reserves its
stack before stopping the old application. Insufficient or fragmented heap and
link errors therefore reject that start without interrupting the running program.
This briefly requires both images to fit; explicitly stopping the old program first
releases its allocations. Cleanup failure retains the old memory until devices,
MPU partitions and the loader are released.

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
`CONFIG_BRICKPHYR_NATIVE_MEMORY_SIZE` limits each native image's loader budget.
`CONFIG_BRICKPHYR_PYTHON_HEAP_SIZE` independently sets the on-demand Python arena
(64 KiB on F4); an idle Python engine reserves none of it. Both F4 profiles permit
64 KiB of ELF and a 214144-byte native loader budget. Their shared system heaps
are 221696 bytes for native-only and 217600 bytes with full BLE Python.
Core Python also reserves 217600 bytes; enabling the Classic Python profile
leaves 193536 bytes for the shared heap. These reservations leave room for the
linked capture and SMP services; Python's arena remains allocated on demand.
The Technic native profile permits 4 KiB images and a 16 KiB loader budget in a
25 KiB shared heap.
An image limit does not reserve upload space or guarantee that the loaded image
fits; another retained upload or firmware allocation can leave insufficient heap.
Alignment, symbols and scratch space count against the loader budget; small
images with large BSS can still exceed it. `CONFIG_BRICKPHYR_APPLICATION_STACK_SIZE`
limits the requested user stack: 16 KiB in both F4 profiles, 2 KiB on Technic.
Native applications allocate only their declared ABI requirement, rounded for the MPU;
raising the maximum reserves no memory. The privileged syscall stack, guard and allocation metadata
also come from the shared heap. The supervisor reserves a replacement's stack
before stopping the current program; insufficient heap or alignment space leaves
that program running. It joins the thread before freeing the stack on return,
fault or explicit stop. `app status` reports the image, admission budget (`heap`),
LLEXT allocation and stack high-water mark from the last run after termination;
it is not a live memory snapshot. LLEXT's allocation count excludes allocator
overhead and is not the peak RAM requirement.

For Python with an independent controller, status also retains `control_stack`
(allocated user-stack bytes) and `control_unused` (untouched bytes). The latter is
negative while no measurement is available. The supervisor inspects the joined
thread once, before releasing its stack, on return, fault or forced stop; periodic
control and live status polling perform no stack scan. `control_stack == 0`
means this run did not start a controller. The Shell's `app status` shows these
values too. Stack filling estimates the observed high-water use, not the maximum
required by paths that have not run.

`app memory` in the USB/BLE Shell and `brickphyrdev app memory` through SMP report
current shared-heap allocated/free bytes and the allocation peak since boot.
`heap_total` is the configured heap size; the counters and component sizes are
different views of the same heap, not additive free-space budgets. The snapshot
also identifies RAM or flash Python source, separate upload staging, the Python
arena, native primary/controller allocations, native Python code reservation and
use, and retained recording storage. Component counts exclude allocator metadata;
flash source bytes do not consume system heap. Python GC `gc_used`/`gc_free` are
inside the Python arena and must not be added to it. Each memory request asks
the VM for a sample at its next poll, without forcing a collection. Until a
sample arrives, or when no Python VM runs, GC values and `gc_age_ms` are `-1`;
the age identifies an older sample when the VM has not polled again.

The same command lists kernel, primary and controller thread stacks. `requested`
is `-1` where no request size is known; `user` is the actual usable stack size
after alignment, and `unused` estimates untouched bytes at the scan. A negative
`unused` is an inspection error. User-mode contexts also report their reserved
privileged syscall stack, but the pinned ARM port cannot measure its high-water
mark (`priv_unused` is `-ENOTSUP`). Live stack scans can race execution and do
not bound future stack use. Heap counters do not reveal the largest contiguous
free block or prove that loading, replacement, or concurrent components will fit.

`brickphyrdev app budget [NAME]` checks a completed RAM upload (no name) or an
installed application without starting it. It validates the real native ELF or
Python package through the firmware's admission readers; `--controller NAME`
checks a separately installed ELF, while `--resource PATH` checks an ELF inside
the selected Python package. Native image and recorder budgets come from the
same firmware validators used at launch, including target stack rounding and
recorder layout. The command does not stop an application, consume a ready
upload, install another one or write flash. Declared record size/count/configuration,
controller parameter bytes, Python native code and other dynamic bytes are
accounted only when supplied; a wrong declaration is not enforced against later
application behavior.

The report separates current `app memory` measurements from candidate accounting.
`resident_accounted` sums declared components, and `phases.load.accounted` also
includes candidate transfer bytes. `phases.replacement.before_release_accounted`
counts the current heap plus allocations prepared before releasing the old
primary; it reuses an existing Python arena and does not count a ready RAM source
twice. Controller, recorder and native-code allocations occur later. These phase
amounts exclude allocator/kernel-object metadata and transient loader allocations;
native admission `native` is a conservative bound, not measured retained memory.
Alignment, fragmentation and undeclared runtime allocations leave both phase
peaks and overall fit unknown unless the minimum known arena, stack and source
bytes already exceed the heap. `potential_store_mutation_snapshot` describes a
possible RAM copy when republishing an active stored source, not normal start.
On Inventor F4, USB checks compare RAM/stored package and controller estimates
with live allocations and verify that preflight leaves staging and execution
unchanged. A native-image preflight also passes over BLE. These bounded checks
do not establish peak usage or fit for arbitrary application code.

Protected ARM checks and bounded USB/BLE checks on one Robot Inventor F4
(PCB revision unknown) cover live GC/stack reads, RAM and stored Python starts
with 4/8 KiB user stacks, a native module and a passive native controller. On F4
this small workload uses 2128 user-stack bytes in Python and 560 in the controller;
these are observations, not general stack recommendations. Stop releases both
contexts; clearing retained records returns the system heap to its pre-run usage.

A protected ARM check loads a 64 KiB padded ELF containing a 64 KiB zero-initialized
array, verifies every byte through the application, and reclaims its memory on exit.
The upload and loaded regions coexist during startup. Padding exercises transfer
capacity, not 64 KiB of executable code. This uses the native F4 budgets with
synthetic peripherals. On a Robot Inventor F4, the native-only profile also runs
that maximum-size RAM upload over USB and BLE. A separately compiled stored ELF
runs with 64 KiB BSS plus 16 KiB initialized writable data over both transports.
Both verify their complete data regions, return without application or cleanup
errors, and leave existing stored files unchanged across the profile switch.
Those checks used the earlier per-image arena allocator: reported loaded
allocations were 66258 and 83037 bytes, with 136864/170291-byte arena reservations.
These synthetic memory workloads establish neither capacity under maximum peripheral/communication load nor a worst-case stack bound.

With the system-heap loader, the full BLE Python profile also runs a native ELF
with 64 KiB BSS and an 8 KiB stack on the Inventor F4 over USB and BLE. The program
checks every byte; its native admission budget is 71072 bytes, above the former
64 KiB limit. A subsequent Python start retains its separate 64 KiB arena and
12 KiB default stack request. Both exits restore the pre-run heap usage and leave
stored files unchanged. The corresponding protected ARM regression probes pages
and the final byte rather than repeating the full memory sweep.

With that earlier allocator, separate USB checks on the same F4 with Zephyr heap
statistics enabled measured system-heap allocation peaks of 203368 bytes for the RAM workload and 171260 bytes
for the stored workload, measured with a 210944-byte heap. These counts include
the reserved loader buffer; they do not measure its internal live allocations or
the current system-heap backend.
Both workloads return system-heap allocation to the 960-byte idle baseline.
ELF admission reads bounded offsets directly from RAM or a locked file, without
allocating a linker context. Only actual linking needs LLEXT's section cache;
file metadata lives in a separate stack frame. File errors retain their original
cause, including when closing the file also fails. The F4 Shell retains its
4096-byte stack for storage, Python and other commands.

Earlier board checks verified repeated execution/unload, constructor state,
floating-point arithmetic, forced termination, recovery after a prohibited RAM write,
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
budget for a controller; see [independent control](#native-control-and-background-computation)
for the execution mechanism and its remaining timing acceptance.

## Application recording

Native ABI 1.3 adds `bp_record_begin(&spec)` and `bp_record_write(data, length)`.
An application configures one fixed-size binary record format before periodic
work. `spec` names its schema and version, record size (1–1024 bytes) and retained
record count. The retained data may not exceed 32768 bytes. The supervisor
allocates it from the shared system heap while the caller waits; a failed
allocation reports `ENOMEM`. A second successful configuration in the same run
is rejected. Applications that do not record allocate no recording buffer.

Native ABI 1.5 adds `bp_record_begin_config(&spec, config, length)`. It copies up
to 1024 bytes of immutable experiment configuration together with the buffer;
configuration and records share the 32768-byte data budget. The record
schema defines the configuration's binary format too. Use it for model values,
initial state, units and configuration identity needed for replay. Ring overwrite
does not erase it. This configuration is not a mutable parameter store.

Native ABI 1.6 adds optional `bp_control_record_begin(&spec, config, length)`.
Use it instead of the ordinary begin operation before the first control wait.
Each `bp_record_write` then automatically retains the latest successful
control wait step: release/start times, sequence, missed releases and the
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
within the same 32768-byte data budget. An over-budget request returns `EINVAL`
before allocation. Storage uses small blocks so protected arenas and stacks do
not require the whole recording to fit one contiguous heap region. A block lookup
table and allocator headers add a small allocation overhead. All blocks are
allocated before publication; a failed begin releases them without publishing a
partial recording. Eviction/reuse takes constant work without scanning the ring.
No allocator, transport or storage operation runs in a control step. Ordinary
recordings reserve none of these optional buffers. Control step sequence exhaustion
returns `EOVERFLOW`; it does not silently wrap to a previous sequence.

Each write copies one complete record and returns without allocation, transport
work or waiting for a reader. Wrong lengths are rejected before modifying the
buffer. The ordinary recording, including `bp_control_record_begin`, replaces its
oldest record when full. This is the default telemetry policy when recent values
matter more than a continuous trajectory. Its 64-bit record indices expose
overwritten ranges. The collector skips ahead within a live compact ring if it
falls behind, instead of repeatedly requesting records already overwritten.
Sequence exhaustion returns `EOVERFLOW`.

Native ABI 2.6 adds `bp_record_begin_segments(&spec, config, length, control)`
for long continuous observation series. It shares the same allocation, record
format and optional control metadata. When the buffer fills, further writes
return `ENOSPC` until the preceding series has been transferred; the application
and its control loop keep running. The hub releases complete records when it
encodes their SMP response. Neither a separate acknowledgement nor a later read
request confirms their receipt. If a response is lost, or saving it on the
computer fails, those records appear as an index gap in the collected file.
If the host disconnects while the buffer is full, recording waits for another
collector but control continues. The next series begins only after the retained
backlog drains; a partially freed buffer cannot produce a short fragment between
long series. Choose this policy when contiguous trajectories matter more than
minimizing pauses in observation.

For a periodic compact segmented stream, `brickphyrdev` may wait up to 120 ms
before its next read so several records share a block. It uses consecutive
control timestamps and the available recording capacity to bound this local
delivery lag. The application and control loop do not wait for the collector;
ordinary rings, finite captures and noncompact streams are not batched this way.

The collector reads applied parameters before releasing a segmented control
record. When they fit in the SMP reply, the hub includes them with the record;
otherwise the collector first reads the retained parameter snapshot separately.
Compact blocks stop at a parameter revision boundary. For other control schemas
with parameters, the collector reads individual records rather than using the
multi-record block operation. No parameter request acts as an acknowledgement.

Every telemetry observation needs a time value. `control=true` retains the exact
`scheduled_us` and `started_us` of its control step, its sequence and applied
parameter revision. The `brickphyr.capture/2` payload also retains the selected
sources' acquisition times. A custom schema without control metadata must encode
its own timestamp and any additional source times needed for replay. Neither
policy reads hardware again or guesses missing values during download.

Native ABI 2.4 also offers the optional acknowledged fixed-size stream
`bp_record_begin_paused(&spec, config, length, control)`. With `control=true`,
it attaches the same step and applied parameters as `bp_control_record_begin`.
When its buffer fills, `bp_record_write` returns `ENOSPC` immediately,
retains all unacknowledged records and leaves the application running. After a
collector receives and flushes complete records, it acknowledges their exclusive
end index through the same SMP connection;
those slots become available for later steps. The acknowledgement confirms
computer receipt, not completion of any analysis or a durable disk sync. An
interrupted download can resume after validating the recording identity. The
current management session alone can retire records; disconnect releases that
claim without erasing data. The collector acknowledges again after reconnection
if its saved file contains records that the hub still retains.
For compact `brickphyr.capture/2` control blocks, newer firmware accepts the
flushed prefix as part of the next read request; the final prefix is acknowledged
separately. Older firmware uses separate acknowledgement requests throughout.

A full segmented or acknowledged buffer can leave an unrecorded interval while
control continues. The application must make that interval visible in its own
record sequence, or use the attached control-step sequence. A later write is a
new observation, not a retry of a stale control step. Finite RAM cannot preserve
a continuous history across an arbitrarily slow or absent collector. Triggered
capture is unavailable in both streaming modes; the ordinary ring and finite
diagnostic capture retain their existing behavior.
The offline reader exposes the number of intervening unrecorded control steps
as `Frame.preceding_step_gap` and `Observation.preceding_step_gap` on the first
row of a record. A block boundary alone has no gap. Strict replay rejects such
an interval unless `allow_gaps=True`; `record-info` counts these boundaries
separately from missing record indices.

Native ABI 2.5 adds `bp_record_begin_variable_paused(&spec, config, length)` for
independently decodable blocks of 1–`spec.size` bytes. Each slot reserves the
maximum size in RAM, while the transfer sends its actual length with a two-byte
prefix. This mode uses the same acknowledgements and gap rules, without automatic
control-step or parameter metadata; a block format that needs those values must
encode them. `bp_capture_pack_frame(config, config_size, step, frame, data,
capacity)` formats one already acquired `bp_control_frame` using the built-in
capture field packers. It returns a `brickphyr.capture/2` row without another
sensor read. The 31-byte configuration selects sources and fields as described
in [finite RAM capture](telemetry.md#finite-ram-capture); the supplied frame must
include selected ports and matching timestamp resolution. An application can
write the row to a fixed control recording, preserving its applied parameters.
For control recordings with this schema, the management service packs
several complete steps and rows into an independently decodable block when
downloading. It reuses the diagnostic capture field descriptions: unchanged
metadata is omitted within a block, shared IMU metadata is sent once, and
timestamps and counters use exact differences when shorter. The client restores
the original step and row bytes before saving them, so recording files and
offline readers keep the same format. Applied parameter blocks remain separate
and are fetched only when their revision changes. Encoding runs on the download
path, outside the controller's recording call; retained RAM still holds the
original fixed-size records. Other application schemas use the ordinary binary
block transfer.

For a controller that needs to record only the commands it actually used,
`brickphyr.capture+app/1` extends the same row with up to eight named values.
Build its configuration once with `bp_capture_app_config` from the ordinary
31-byte capture selection and field names/types (`b/B/h/H/i/I/f`, little-endian;
include units in names). After `bp_capture_pack_frame`, append those values to
the row and use a control recording. The existing compact transfer and offline
reader decode them under `app`; the control step supplies timestamps and applied
parameter revisions. Only selected hardware fields and values copied by the
application are sent. To replay the actual input, append values after applying
any dead zones or limits, in the same step in which the controller uses them.
An ARM/QEMU check and a passive SPIKE Prime F4 BLE run recovered 100 consecutive
rows with two synthetic command values; neither run drove motors.

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
them and can be repeated. Capable firmware advertises block reads, allowing one
response to carry several records or parts of records within the negotiated SMP
buffer. Older firmware retains single-record reads. Every response
reports the current generation and retained index range. A client must discard
a partial record if it was overwritten, and must not combine generations.
Generations identify runs within one boot, not persistent experiment identities.
Firmware with resumable transfer also retains a 16-byte capture tag. The first
collector binds a random host-generated tag; later collectors obtain that same
tag. Resetting or clearing the recorder discards it. This distinguishes a
retained capture from another boot that happens to reuse its generation number.

The [built-in passive capture](telemetry.md#finite-ram-capture) uses this same
recorder with finite, non-overwriting semantics. It owns the recorder while
running and while retaining data; application launch and ordinary record clearing
return `EBUSY` until `capture clear`. The application ring/trigger behavior above
is unchanged. Capture rejects the application's retained data instead of replacing
it implicitly. Its end reasons describe capture completion, not application exit.

```sh
uv run brickphyrdev app run build/native-examples/control_record.elf
uv run brickphyrdev app record control.jsonl
uv run brickphyrdev app --ble Brickphyr record control-again.jsonl
uv run brickphyrdev app --ble Brickphyr record control.cbor
uv run brickphyrdev app --ble Brickphyr record control.cbor --resume
uv run brickphyrdev record-info control.cbor
uv run brickphyrdev app record-clear
```

Use the matching SDK to build that example. `app record` creates a new file,
refusing to overwrite an existing one before connecting. The `.cbor` suffix
selects a standard CBOR Sequence with binary byte strings; other suffixes select
JSONL with hex strings. `--format cbor` or `--format jsonl` overrides that choice
for collection and `record-info`. `app run/start --record FILE` follows the same
suffix convention. Both encodings use the same event schema and offline reader;
a truncated CBOR event is rejected rather than treated as a complete sample.
`--resume` opens an incomplete file, validates its existing events, then checks
the hub's capture tag, generation and metadata before appending. A mismatch
leaves the file unchanged. Collection restarts at the first missing whole record;
the file retains interrupted collection endings and explicit `resume` events,
with cumulative record/gap counts. Overwritten records remain explicit gaps.
Resume requires a tagged recording and cannot be combined with `--after`.
Malformed/truncated files are rejected; no automatic repair discards their bytes.

Collection reads the retained tail and follows a live application until it ends or the user interrupts
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
revision once in the output; it discards an incomplete record if its parameter snapshot was
overwritten before retrieval and reports the gap. A replay must use these values
and the application schema, not current hub settings or a later sensor read.

Programs calling `brickphyrdev.recording.collect()` may pass `on_frame` to receive
one `records.Frame` per saved record, with its applied parameter bytes, control
step, decoded values and preceding record/step gaps. The callback may be synchronous
or awaitable. If `on_observation` is also set, the frame callback runs first;
both receive data from the same saved record. Slow callbacks delay downloading,
not control execution. Resuming a download calls callbacks only for newly appended
records; replay the saved file to recover earlier frames.

### Offline inspection and numerical replay

Inspect a collected file without connecting to a hub:

```sh
uv run brickphyrdev record-info control.jsonl
uv run brickphyrdev record-info interrupted.jsonl --allow-incomplete
```

The summary distinguishes completed collection, missing records, application
result/fault/cleanup and triggered-capture completion. A completed collection can
contain gaps or report a failed application. `--allow-incomplete` permits inspection
of a cancelled, failed or unfinished collection; malformed data still fails.
These checks validate structure and consistency, not cryptographic integrity.

Application-owned replay tools use the streaming `brickphyrdev.records.Recording`
reader. It retains the current parameter revision rather than loading the full
history. `config` contains the original immutable bytes; each `Frame` provides
`index`, raw `data`, applied `parameters`, optional `control` step metadata and
`gap` (the number of missing records immediately before it). Known schemas also
provide `values`, decoded from raw bytes instead of trusting cached JSON values.
Unknown application schemas remain available as raw bytes.

```python
from brickphyrdev.records import Recording

with open("control.jsonl", encoding="utf-8") as source:
    recording = Recording(source)
    for frame in recording.frames():
        # Decode using the application's schema, then call its numerical core.
        print(frame.index, frame.data.hex(), frame.control)
    print(recording.complete, recording.outcome)
```

Consume the iterator fully before accepting replay results: termination and final
counts are validated at the end. Gaps and incomplete collection fail by default.
`frames(allow_gaps=True)` is appropriate only when the application can interpret
the retained steps independently; it never reconstructs missing history.
`allow_incomplete=True` exposes a valid prefix but leaves `complete` false.
The reader consumes its caller-owned stream once and does not close it.

The DSP SDK builds `imu_record.elf` from the same source as `imu_filter.elf`,
enabling recording without adding it to the smaller filter example. It retains
64 observations in 4096 bytes, plus 16 bytes of FIR coefficients. Its
`brickphyr.imu-fir/1` schema uses little-endian `float32` configuration and 64-byte
records (`<q4I10f`): sample time in microseconds, sequence, IMU epoch, reset flag,
reserved zero, input/output in millidegrees per second and four-word FIR states
before/after the calculation. It neither claims nor drives motors.

Build the [DSP SDK and native examples](builds.md#optional-cmsis-dsp-native-sdk),
then the desktop library and collect an event capture:

```sh
uv run cmake -S examples/host -B build/imu-replay -G Ninja \
  -DCMSIS_DSP_DIR="$PWD/../modules/lib/cmsis-dsp"
uv run cmake --build build/imu-replay -j 4
uv run brickphyrdev app run build/native-dsp-examples/imu_record.elf
uv run brickphyrdev app record reports/imu.jsonl --after 8
uv run brickphyrdev app stop
uv run brickphyrdev record-info reports/imu.jsonl
uv run python examples/host/replay_imu_filter.py reports/imu.jsonl \
  --library build/imu-replay/libbrickphyr_imu_fir.so --allow-gaps
uv run brickphyrdev app record-clear
```

The desktop library compiles the same CMSIS-DSP `arm_fir_f32` source from the
pinned workspace module. The replay script uses recorded coefficients and each
original input state, checks continuity where history is present, and compares
output/state bytes. `--allow-gaps` explicitly permits replay of the retained tail;
the summary still reports missing records. A mismatch or incomplete collection
returns a nonzero exit status. Exact equality is checked for this scalar filter
with quarter coefficients, not guaranteed for arbitrary algorithms or compiler
floating-point options. Replaying original steps does not simulate a different
closed-loop trajectory after changing the controller.

On one Robot Inventor F4 (PCB revision unknown), a USB event capture retained
68 observations with explicit gaps; all 68 matched desktop output/state bits.
The filter continued after capture and was then explicitly stopped. Its 4192-byte
ELF used 800 of the requested 2048 stack bytes in that bounded run. Application
fault, cleanup and output-drop counters were zero; stored applications remained
unchanged. This establishes the example's capture/replay path, not lossless
streaming, calibrated sensor accuracy or a robot's complete replay contract.

### Control recording examples

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

`control_sample.elf` builds the same observation source using IMU-triggered
control waits. It records 40 distinct samples, retaining the last 32 with their
automatic step metadata. Its four configuration bytes are the little-endian
`uint32` wait timeout, 100 ms. No ports are claimed or driven. For example:

```sh
uv run brickphyrdev app run build/native-examples/control_sample.elf
uv run brickphyrdev app record reports/control-samples.jsonl
uv run brickphyrdev record-info reports/control-samples.jsonl --allow-gaps
```

The initial eight records are explicitly reported as overwritten when collection
starts after completion. A slow controller consumes the newest fresh sample and
reports omitted ready edges; its rate need not equal the sensor's configured rate.

On one Robot Inventor F4 (PCB revision unknown), the example completes both
standalone and alongside Python repeatedly allocating 1 KiB and collecting garbage.
Both runs finish without application or cleanup errors; the concurrent run's
retained 32 records match over USB and BLE. Consumer omissions are reported
explicitly. Motors remain passive and stored applications are preserved.

A bounded combined-load probe reads real IMU and passive A/B encoder observations
while Python executes six learning batches on synthetic data. It measures the
first `bp_time_us()` call after each sample wait, including parameter copying,
recorder staging, syscall return and clock-read overhead. On the same Inventor F4:

| Host activity | Python batch time | Steps in load phase | Omitted ready edges | Maximum edge-to-application timestamp |
| --- | ---: | ---: | ---: | ---: |
| USB output polling | 2785 ms | 2310 | 10 | 1983 us |
| USB output polling plus four telemetry sources at 20 Hz | 3638 ms | 2623 | 403 | 5761 us |
| BLE output polling | 3135 ms | 2605 | 9 | 1853 us |

The load phase includes Python handoff around the timed batches. Output polling
requests a 20 Hz refresh; that requested cadence does not establish an equal
completed request load across transports. Acquisition missed/failure counters
remain zero, with no source errors, IMU epoch changes or port-generation changes
in these short runs.
The bounded record ring retains diagnostic points rather than every observation;
overwritten points are reported explicitly. One retained USB point has 2351 us
between `started_us` and the application timestamp, which includes possible
preemption and clock-read overhead, not just syscall execution cost.
These checks do not establish an uninterrupted 833 Hz control rate or a deadline
bound. Acquisition continuity alone does not prove that the controller consumed
every sample; selecting a lower control rate still requires its own budget.

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

Its `brickphyr.control-group/2` records are 440 bytes each, with four retained
slots (1760 bytes). The first 176 bytes use the input schema above and remain
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
output. The group retains each member's original `group_mode`, `group_duty_permille`,
`group_applied_us` and subsequent `group_cleanup_us`; the group's `applied_mask`
and `cleanup_mask` qualify those times. These survive expiry and cancellation
even when the latest port snapshot has already changed. The record includes
`timestamp_resolution_us`. The decoder also accepts the earlier `/1` schema,
which does not contain these retained times.
The sample's 1 ms polling may miss an intermediate phase under load; an
absent applied observation is not evidence that no output occurred. Driver
completion timestamps do not measure physical voltage or shaft response.
This is one command experiment, not a scheduled controller or a lossless trace
of every driver transition.

Protected ARM/SMP checks exercise opposite duties, expiry/coast, the passive
variant, input rejection and host decoding. One Robot Inventor F4 (PCB revision
unknown) has checks of the passive variant and identical USB/BLE record retrieval;
physical group drive and expiry remain unverified. The example adds no resident
firmware worker or memory reservation. Autonomous flash recording and integrated
robot replay remain [roadmap work](roadmap.md).

### Recorded periodic actuation

The [periodic example](../examples/native/control_actuation.c) uses a 20 ms native
release to capture IMU, battery and A/B port input, then issues one grouped raw
command. The default `control_actuation.elf` applies opposite 20% duties for its
first 15 commands and coasts on its sixteenth; clear both mechanisms before use.
`control_actuation_coast.elf` runs the same 16 steps with passive outputs only:

```sh
uv run brickphyrdev app run build/native-examples/control_actuation_coast.elf
uv run brickphyrdev app record actuation.jsonl
```

Both build from the same source and use `brickphyr.control-actuation/2`: a
little-endian 368-byte record, 32 retained slots and eight configuration bytes
containing the requested period in microseconds and step count as two `uint32`
values. Each of the 16 command records holds the exact input frame, commanded duty/generation,
absolute deadline, admission error and request ID. The atomic previous-group
snapshot includes its request, per-port request/generation, original mode/duty,
application and cleanup timestamps with qualifying masks and clock resolution,
first operation and cleanup errors, and the control sequence that issued it. A final
seventeenth record reports completion of the last coast under that same step's
control metadata. `phase` is 0 for an issue and 1 for that final observation.
The host decoder exposes `input`, `command` and `previous`
while preserving the raw bytes. A successful admission does not imply that the
previous command succeeded or that the new output has reached a driver.

Only the immediate previous group is copied on each issue; there is no driver
event queue. A rejected issue may report that previous group still pending.
`previous.control_sequence == 0` means its originating step is unknown, and a
retained result cannot restore an input record lost to ring overwrite. Input and
group results are separate snapshots: correlate each input port's `completed`
and `output_generation` with the prior result's per-port request and generation
before interpreting it as that command's current output. For original application
and cleanup edges, use the retained `previous.ports` timestamps and corresponding
masks; later port updates cannot overwrite them. The host decoder retains `/1`
support for earlier recordings. Driver
completion does not measure electrical output or shaft response. The example
stops after 16 releases; its requested cadence is not a deadline guarantee.
Protected ARM/SMP checks cover the 17-record sequence, request and origin IDs,
signed synthetic outputs and final coast. On one Robot Inventor F4 (PCB revision
unknown), the coast-only path retained 16 commands plus the final observation;
USB and BLE returned identical records and all 16 prior outcomes matched their
origin steps. No motors were energized. Physical grouped drive and timing under
combined load remain open.

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
ordinary data remains non-executable. The shared storage worker initializes and publishes code below control and
hardware-service priorities. Builds without storage use a 1 KiB worker;
large memory operations do not occupy the cooperative system workqueue.
Committed code stays allocated until the
application ends, including functions no longer referenced by Python. Exhaustion
raises `MemoryError`. Status reports its reservation as `code` and usage as
`code_used`, separately from the execution arena.
`MemoryError: native code reservation failed` identifies this code-region
admission rather than a GC-heap allocation: free Python heap alone cannot satisfy
its system-heap capacity and alignment requirements.

The full F4 Python profile accepts packages up to 64 KiB. The 64 KiB Python
arena holds the GC heap; package bytes live outside it. RAM uploads retain one
immutable allocation, shared with a retained repeat upload when requested.
Stored packages are read from LittleFS in bounded chunks. Imports, ordinary
resource streams and controller resources use the same bounded reader; native
controller linking still allocates its executable/data sections in protected RAM.
RAM execution needs space for both the package and VM; stored execution avoids
that package allocation. Neither image-size admission nor free GC bytes alone
prove that the complete application will fit the shared heap.
`CONFIG_BRICKPHYR_PYTHON_STACK_SIZE` independently sets the default Python stack
request to 12 KiB; the F4 MPU
rounds its allocation to 16 KiB. The CLI `--stack BYTES` option on `app run` or
`app start` can request a Python user stack from 4 KiB through the configured
16 KiB maximum on this profile; the actual allocation is MPU-rounded. Native
ELF stack size remains its declared ABI requirement. The Python stack allocation
exists only while Python is loaded; a native application in the same image uses
its own ABI requirement. Stack checking
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
The native-only profile provides a 221696-byte heap, including on-demand loader
allocations and stacks. The BLE full and core Python profiles each provide
217600 bytes, including the on-demand 64 KiB Python arena.
Idle and native-only execution leave that capacity available to other allocations.
Active stacks and native loader
allocations, including a concurrent controller, also come from that heap.
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

The normal native profile exposes live `app memory` diagnostics; the optional
[memory diagnostics profile](builds.md#memory-diagnostics) adds Zephyr's `kernel heap`
Shell command. In REPL, `gc.mem_free()` and `gc.mem_alloc()` report the separate
GC heap. Use `app status` after a program ends for its retained stack high-water and
native-code usage; its live `unused` field is not a current scan.

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
RAM and stored starts and verify system-heap reclamation. The Python-enabled
native SMP fixture now configures a 147456-byte shared heap, including the on-demand
64 KiB arena and stack, instead of an 80 KiB heap plus a static 64 KiB arena;
it does not establish F4 USB/BLE driver overhead or other programs' compiler needs.

### Concurrent Python memory workload

On one Robot Inventor F4 (PCB revision unknown), the full BLE-only Python profile
runs a 43,702-byte package containing Babybot's C learning module and an independent
native observer. The Python primary retains all 320 input rows (15,360 bytes),
320 flags, a 3,032-byte learning workspace and 544-byte history. The inputs repeat
a 16-row synthetic fixture with rebased timestamps. Twenty incremental append
calls and GC collections precede one model update; this is a numerical and memory
workload, not a recorded continuous robot trajectory.

The observer runs every 15 ms, receives copied 60-byte model revisions and keeps
64 records of 176 bytes, plus control metadata. The package is retained once
outside GC for RAM execution, or read directly from LittleFS for stored execution.
The controller ELF is read through that source without an extra whole-image copy;
its MPU-rounded executable/data regions are separate allocations.

These measurements used the earlier 512-byte SMP buffers and a 223232-byte heap;
the current 1024-byte buffers trade 2048 bytes of shared heap for larger transfers.

| Live allocation | RAM start | Stored start |
| --- | ---: | ---: |
| System heap configured | 223,232 B | 223,232 B |
| System heap used | 185,208 B | 141,500 B |
| System heap free | 37,760 B | 81,472 B |
| Retained RAM package | 43,702 B | 0 B |
| Python GC used/free reported after learning | 28,928 / 35,072 B | 28,928 / 35,072 B |
| Python native code used/reserved | 11,472 / 16,384 B | 11,472 / 16,384 B |
| Loaded controller regions | 18,276 B | 18,276 B |
| Recorder requested allocation | 18,108 B | 18,108 B |
| Primary stack used/reserved | 5,848 / 8,192 B | 5,848 / 8,192 B |
| Controller stack used/reserved | 2,888 / 8,192 B | 2,888 / 8,192 B |

The table shows USB snapshots; BLE runs have the same system-heap counts and
35,072–35,712 bytes free inside GC. GC belongs to the allocated Python arena and
cannot directly satisfy native allocations. Heap counters include allocator rounding, while
component sizes exclude allocator metadata. Each application context also
reserves a 1,024-byte privileged stack whose high-water mark is unavailable.
These live counts and observed stack use do not bound transient peaks, future
execution or a different application's contiguous-memory requirements.

USB and BLE RAM/stored checks reproduce the learned model/history and retained
observer results bit for bit on the host. Learning with GC takes 8.15 s over USB
and 7.22–7.23 s over BLE while the independent controller continues. Host polling
differs between transports; neither time is an isolated numerical-kernel benchmark.
Both exceed the 4.8 s represented by 320 rows at 15 ms.
The robot must select its update frequency and retained data accordingly.
Normal completion and USB forced stop during learning reclaim both execution
contexts. Clearing the retained recording then restores the initial system-heap
usage exactly. No sensors or actuators participate in this workload; complete
robot migration and sustained timing acceptance remain separate work.

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
| `struct`, `memoryview` | Endianness, in-place packing, buffer boundaries, shared views, read-only buffers and mutable bytearray/memoryview slices |
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

The full F4 profile limits the encoded image to 64 KiB, at most 64 files plus optional entry
metadata, and 128 bytes per relative path. Absolute paths, empty/`.`/`..` components, embedded NUL,
duplicate keys and file/directory collisions are rejected. CBOR keys and values
must be definite-length text and byte strings. Admission validates the complete
map and each Python/MPY format envelope; syntax and bytecode-body errors remain
execution errors. Unsupported native architectures are checked again on import.

Named images use the same LittleFS staging, validation, atomic rename, listing and
removal as native ELF. Upload/install never starts or stops a program. Admission
scans the map and code envelopes through bounded reads; execution keeps the
read-only source open, without copying the complete package into RAM or GC.

Later replacement or removal cannot change a running instance's imports, even
when a module is imported again after removal from `sys.modules`. LittleFS does
not pin all blocks of an unlinked read-only file, so before mutating the active
source's installed path, the supervisor snapshots that old package into RAM.
This allocation occurs only on replacement/removal of the running package.
Insufficient memory rejects the mutation and preserves both the installed and
running versions. Account for this snapshot in the update peak; stop first when
that extra reservation is unwanted. Ordinary launch/read never writes flash.

The source and any retained RAM upload have explicit references. Stop/fault joins
both execution contexts and cancels pending reads before releasing the source;
an accepted store writer retains its own reference until its write completes.

Temporary directories use the same image and interpreter without any application
storage write. `open()` and `io.open()` read bundled resources in text or binary
mode, with seeking, iteration and context-manager close. Write modes fail with
`EROFS`; a missing or escaped path fails with `ENOENT`. An absolute path is rooted
in this same program namespace, never in the firmware filesystem. `__file__` and
`sys.argv[0]` identify the root entry.
See [CLI and terminal commands](host.md#python-applications).

Protected ARM checks cover bounded resource reads, source/bytecode/native imports,
running-version preservation during replacement/removal, and complete system-heap
reclamation after RAM and stored execution. On one Robot Inventor F4, a synthetic
64 KiB package with padding and a matching-SDK C module ran from RAM and LittleFS
over USB; both left 62,944 B free after GC. USB/BLE checks also cover the native
named-controller example and Python controllers loaded directly from RAM/stored
package resources, with passive observations only. These bounded examples do not
establish capacity for a complete Babybot workload.

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

Applications can explicitly save without exiting:

```python
from pybricks.tools import wait

hub.system.storage_save()                  # Wait; OSError on failure.
request = hub.system.storage_save(wait=False)
while not hub.system.storage_save_status(request):
    wait(1)
```

Inside `run_task`, `await hub.system.storage_save()` waits cooperatively with
other Python tasks. `wait=False` returns immediately with a positive request ID;
status returns `False` while pending, `True` on success, and raises `OSError` for
failure. Closing an awaitable only stops waiting, not the accepted save.

The native equivalents are `bp_user_data_save()` and
`bp_user_data_save_start()`/`bp_user_data_save_status(request)`. Start returns a
positive request or negative errno. Status returns `-EINPROGRESS`, zero, or the
save error. Both are primary-context operations; an independent controller gets
`EPERM`. The synchronous C convenience function yields while polling the same
request. None of these calls gives the application filesystem access.

One save can be pending. Acceptance copies all 512 bytes into a firmware-owned
snapshot without allocation or flash I/O in the caller. A pending save or a
firmware save reservation returns `EBUSY` without replacing the request. A newer
accepted save expires the previous ID (`ESTALE`); read the result before starting
another, and coordinate other writers. IDs are valid only for the current boot.
Success means the snapshot was committed, not merely queued. Unchanged data needs
no flash write. Stop, fault or host disconnection does not cancel accepted work.
Changes to RAM after acceptance need a later save, even if made before the worker
begins. Application code owns the meaning and versioning of the saved bytes;
publishing a controller parameter revision does not implicitly persist it.

The filesystem worker is shared with Pybricks program-slot operations, separate
from the hub loop and system workqueue. The full F4 profile reuses their 4 KiB
stack; a storage-enabled profile without slots also needs this worker. The snapshot
adds 512 bytes independently of Python. There is no per-save thread or heap buffer.
Control and hardware services can preempt the worker, but this is not a measured
control-latency guarantee or a bound on flash completion time.

Dirty user data is also saved at orderly hub shutdown, after applications and device
outputs stop. Shutdown joins an accepted save before committing later RAM changes
and releasing power. The Shell uses the same mechanism:

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
lose unsaved RAM changes or interrupt a pending save. `config save` and user-data saving are independent:
neither persists the other's pending changes. Reads outside the 512-byte user
region are rejected; adjacent private Pybricks settings/program memory is not
exposed. General writable Python files remain outside this interface.

ARM checks cover the protected application request and Python synchronous,
nonblocking and awaitable calls. LittleFS simulation covers snapshot acceptance,
concurrent RAM updates, errors/retry, unchanged saves and fresh-process restoration.
On the Inventor F4, earlier native USB/BLE checks cover RAM read/write, Shell save,
retained data after reboot and loss of unsaved changes. Application-requested saves
and automatic saving during physical shutdown remain unverified there.

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

### Host-to-native messages

The native primary can call `bp_host_open()` once per run with up to eight
`bp_host_type` descriptors. Each descriptor has a numeric type, human-readable
name, binary-format version, maximum size (1–1024 bytes), and `BP_HOST_VALUE`
or `BP_HOST_COMMAND` policy. Combined declared payload capacity is at most
4096 bytes; it is allocated only while this native application runs. The
application defines and validates the contents of each complete block. This
supports controller parameters and other application commands without baking
their schemas into the firmware. Python and background controllers cannot
register an inbox. Include the declared capacity and descriptor overhead in
`app budget --dynamic` when preflighting an application.

`bp_host_read(type, buffer, capacity, &info)` copies one complete pending block
and returns 1, including for an empty-payload command; 0 means no block. `info`
contains the request number, byte length and hub receive timestamp. A short
buffer returns `-EMSGSIZE` without consuming the block. The application applies
it and calls `bp_host_finish(type, request, result, detail)` with zero or a
negative error and an optional application-defined detail, such as the applied
parameter revision. Neither call communicates with the host or allocates memory.
An existing controller can then receive validated parameters through its normal
primary-to-controller update API.

`BP_HOST_VALUE` replaces an older **pending** value of the same type. An already
read value remains in processing while a newer one waits. `BP_HOST_COMMAND`
rejects another command of its type while one is pending or processing. Each
type has monotonically increasing nonzero 32-bit request numbers; duplicates
with the same payload return their current status, while changed retries and
stale numbers are rejected. Status distinguishes pending, processing, completed
and superseded. Messages and outcomes are volatile: a stopped, faulted, or
replaced application loses them. The host must rediscover the new run ID after
restart. Receipt is distinct from completion; reconnecting does not stop an
autonomous application. The [CLI](host.md#native-application-messages) and Shell
share these handlers over the existing SMP transport.

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
