# Platform roadmap

Brickphyr provides the firmware foundation for robots with application-defined
control, state estimation and learning. Babybot is the first integration workload
on SPIKE Prime / Robot Inventor F4. Its algorithms remain in the separate robot
project; Brickphyr supplies reusable execution, device, timing and observation
mechanisms. Pybricks code and tools are foundations for reuse and compatibility,
not a requirement to reproduce every hub and peripheral in its ecosystem.

Complete SPIKE support, resource optimization and the new execution capabilities
are all required work. Full Python on F4 retains source compilation, REPL,
compatible `.mpy` modules and the agreed Pybricks behavior. Native Technic/Control+
remains a desirable secondary port; it must not delay the SPIKE/Babybot path.
Python on Technic and WASM are outside this milestone.

## Starting point

The F4 implementation already includes protected ELF loading, a versioned SDK,
RAM and named stored applications, MicroPython, Shell/SMP over USB/BLE, settings,
power management, and device services. Hard-float execution, FPU context fixes,
optional CMSIS-DSP and an ABI independent of Zephyr syscall numbering are existing
foundations. On-demand UART metadata, motor/paired-controller state and the
native-only loader backing buffer, plus recovered linker space, are completed
optimizations to preserve. The Python profile still reserves a shared execution arena.
The owning [application](applications.md), [hardware](hardware.md) and
[Python](python.md) documents distinguish implemented behavior from acceptance.

Native and Python execution currently alternate through one application thread
and arena. Independent native execution alongside Python is new work. Existing
[monitoring](telemetry.md) reads current snapshots; it does not record every
control step. Babybot's numerical code and replay provide useful algorithms and
integration evidence. Its current scheduling, interfaces and recording format
are not compatibility requirements: evolve them to use the new platform.

The native [control input frame](applications.md#control-input-frame) supplies
read-only IMU, battery and selected encoder/output snapshots with source times,
errors and connection identity. [Grouped raw output](ports.md#grouped-raw-motor-commands)
adds whole-request validation, absolute expiry and group coast on failure through
the same motor worker. Physical group-drive acceptance, independent execution
and cycle recording remain work; these APIs alone do not complete block 2.

## Delivery blocks

Each block ends with usable behavior and evidence through the production path.
Start with F4 reliability and budgets; add timing and lifecycle contracts before
concurrent execution. Minimal cycle recording starts with the timing work, so
the new controller is observable from its first run. Work on independent parts
can proceed while a particular physical check awaits hardware or an operator.

| Block | Required work | Checkable result |
| --- | --- | --- |
| 1. Complete SPIKE services | Finish remaining behavior and acceptance for buttons/lights, matrix, sound, IMU, six ports, supported sensors and motors, USB/BLE, settings/storage, charging, power, watchdog and recovery. Close known UART startup/reconnection issues and characterize acquisition under combined load. Complete the corresponding native/Python paths and outstanding client-transfer recovery. | Cold-start native and Python workflows use the shared hardware services, preserve stored data and recover from application errors. Named-board evidence covers the planned hardware surface; unsupported devices remain explicit. |
| 2. Timed observations and actuation | Add a bounded input snapshot with per-source times, validity, sequence and connection generation. Add logically grouped motor commands with request/completion identity, output state and an explicit partial-failure policy. Record the input and command of each control step. | A native example consumes fresh, attributable measurements and issues a paired command; data gaps, stale inputs, command errors and delay are observable. |
| 3. Independent native control and Python | Extend the existing supervisor to one application containing a native control context and an optional Python context. Provide periodic or sample-triggered execution, versioned parameter exchange and independent command-expiry enforcement. Retain standalone native and Python operation. | A loaded controller continues under Python GC and USB/BLE activity within a measured timing budget; stop, faults and replacement quiesce all owned work before memory is reclaimed. |
| 4. Experiment recording and replay | Extend existing management/recording tools with bounded binary cycle records, explicit gaps, event prehistory and versioned configuration metadata. Decode on the host and adapt useful robot replay algorithms to the platform's records. Add autonomous flash recording only after partition and timing checks. | The recorded inputs, applied-command reports and parameter versions reproduce the numerical calculation on the computer. Slow readers do not block control; incomplete recordings are identified. |
| 5. Background computation | Allow bounded lower-priority numerical work to exchange results with the control context. Apply complete parameter revisions at a step boundary, with the application owning candidate validation and rollback policy. | A representative existing Babybot identification workload progresses without violating the control budget; stop and unload also quiesce background work. |
| 6. Babybot migration acceptance | Reuse validated numerical code for the SDK and host; restructure Babybot's execution, hardware access, recording and Python orchestration around the new platform capabilities. Exercise standalone and Python-managed execution. Publish dependencies, reachable pins, firmware, SDK and focused CI checks. | An end-to-end Babybot workload can be built, uploaded without reflashing, run, observed, stopped and replayed using reproducible artifacts, with explicit remaining physical limitations. |

Blocks 2–4 form the first integrated control experiment, not three unrelated
frameworks. Full SPIKE support remains a completion requirement; its remaining
acceptance is not discarded when the experiment first runs. Preserve the existing
Python acceptance inventory, but do not require every external Pybricks accessory
before testing the Babybot execution path. USB-initiated wake, battery watchdog,
charging-source behavior and power-cycle persistence remain explicit tasks.
Define storage-reset semantics before implementing that operation.

Use Babybot's existing calibrated IMU, encoder angle/speed, battery and direct
motor-duty paths as the first integration surface, including input discontinuities
and output saturation. Preserve optional controller input through the shared BLE
services. Compare the reused numerical core on recorded inputs before enabling
closed-loop motion; then progress through inactive outputs, suspended wheels and
operator-assisted balancing. A synthetic workload or a free-spinning motor check
does not establish balance. Keep the robot's tuning and physical acceptance in
its own project.

## Architecture work within the blocks

One application owns its device claims and component lifetimes. Reuse LLEXT,
MicroPython and the current supervisor rather than creating a parallel loader or
a general process/plugin system. User computation runs in protected thread
context, never as a privileged timer callback. A native control step must not
depend on Python objects, GC, formatting, storage writes, unbounded queues or
locks held by lower-priority application work. Specify component memory access,
stop/reset/fault propagation, pending-call cancellation and unload ordering before
enabling concurrent execution. Ordinary host disconnection retains autonomous work.

Use the existing monotonic clock. A coherent snapshot is not a simultaneous
physical sample: preserve the meaning of each acquisition or receive timestamp,
data age and discontinuity. Distinguish requested duty, driver-confirmed output
and measured electrical behavior. On F4 the hub generates motor PWM locally;
encoder UART timing and output application are separate paths. A grouped request
does not imply simultaneous physical switching or rollback after a partial write.
Choose periods and deadlines from those paths and the robot's requirements;
neither the motor service's 5 ms cadence nor a previous 15 ms experiment is a
universal scheduling requirement.

Define command validity deadlines and fault handling outside the controller that
can fail. Whole-system watchdog recovery does not replace timely motor handling.
Measure acquisition, scheduling, computation and output delay, including IRQs,
copies and driver waits; a high thread priority alone proves no deadline bound.

Record exactly the observations used by the step, its output and the subsequent
actuation result, with schema/build/configuration identity, sequence, times,
validity and termination reason. Store parameter values on revision changes,
not in every frame. Size buffers from actual record size and transport budgets.
Finite RAM cannot guarantee lossless recording across arbitrary disconnection.
Event capture must distinguish a robot fall from a firmware reset or power loss.
Reuse SMP and the existing connection owner for transport; audit external-flash
regions under the [recovery contract](recovery.md) before expanding storage.

Robot-specific models, learning acceptance and trajectory generation remain in
Babybot. SDK libraries may supply selected CMSIS-DSP operations; no mandatory
large numerical or ML runtime is needed. Host replay validates computation on
recorded inputs, not a new closed-loop trajectory after changing the controller.

## Retained optimization work

Measure native, full Python and the new concurrent workload separately. Report
firmware flash, static state, stacks, live/peak system heap, Python GC capacity,
loaded code, loading scratch and useful application capacity. Linked RAM includes
reserved allocatable memory; it is not the firmware's always-live working set.

- **Placement and alignment:** inspect MPU padding, generated metadata and linker
  reservations. Recover real waste without consuming loader/recovery memory or
  repeating already completed F4 layout work. Technic-specific probe gaps are not
  evidence of the same gap on F4.
- **Stacks and service contexts:** measure high-water use and worst-path margin.
  Share contexts only when blocking dependencies permit it; merge neither a
  worker with a service it waits for nor live stacks of concurrent components.
- **Loading and buffers:** reduce redundant copies, growth peaks and conservative
  reservations using observed allocation lifetimes. Preserve rejected-replacement
  behavior, retained output, first-error diagnostics and independent UART/DMA RX.
- **Python and native memory:** account for retained packages, compilation/GC,
  executable-code allocation/alignment and concurrent controller state. Allocate
  for admitted workloads; do not overlay regions that are now live together.
  Keep full F4 Python, REPL, native modules and MPU protection.
- **Firmware flash:** remove unreachable/unneeded dependencies and tables, link
  only selected DSP routines, and reuse existing algorithms. Keep board-specific
  feature selection without disguising loss of required SPIKE functionality as
  equivalent-feature optimization.
- **Execution cost:** follow measured acquisition, UART, motor, allocation,
  copying, interrupt and transport bottlenecks. Compare the entire affected path,
  not only an isolated arithmetic kernel; report memory and timing together.

Evaluate each opportunity by useful capacity or timing gained for the target
workload. Do not postpone completion indefinitely for byte-level savings. FPU
context and ABI checks extend to the new concurrent lifetimes; existing FPU/DSP
integration is not a new implementation task.

## Secondary port and delivery

Preserve the build-only native Technic port and the goal of a common ELF when
CPU/ABI, services and memory requirements match. Its complete four-port UART,
regulated-motor, BLE and storage profile must fit as a whole before claiming a
usable port. The reference budgets remain 208 KiB firmware flash, 16 KiB user
flash and 64 KiB RAM, with the factory loader preserved. Physical acceptance
requires a board; no Technic board has been available. Additional hubs are driven
by concrete robot needs, not a compatibility checklist.

Prepare reproducible dependency pins and artifacts at useful SPIKE milestones,
not only after the secondary port. Use the [upstream workflow](upstream.md) and
[focused-check policy](../CONTRIBUTING.md#checks-and-pull-requests): quick affected
checks during development, combined acceptance at major milestones, and explicit
physical evidence. Completed optimizations and supported APIs remain foundations
when priorities change; pending work is not silently relabeled as complete.
