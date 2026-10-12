# Python implementation plan

The [MicroPython core](../src/python/README.md) has native-simulator and ARM
user-mode checks. The shared supervisor now runs temporary and installed Python
programs with source/bytecode/native imports through SMP, Shell and `brickphyrdev`,
with ARM QEMU and Inventor F4 USB/BLE coverage.
See [runtime support and limits](applications.md#experimental-python-profile).
REPL has ARM/PTY and Inventor F4 USB/BLE checks; the remaining interactive
acceptance and Pybricks hardware bindings remain open. Parameters, geometry and
non-device tools have [executable coverage](compatibility.md#implemented-library-subset).
The Pybricks execution
adapter has RAM-program and persistent-slot checks, not complete client acceptance.
[Watchdog recovery](settings.md#system-watchdog) is implemented, with the physical
acceptance limits described there. This plan takes the existing application
supervisor, LittleFS store, Shell and USB/BLE transports
through a usable Python implementation on the initial Prime/Inventor F4 target.
The working native engine remains supported. The behavioral reference is
Pybricks v4.1.0b6, as selected in [compatibility](compatibility.md).

## Starting point

Continue the existing implementation; the steps below are not all new work.
Software checks and physical acceptance remain separate. The owning documents
record their [runtime](applications.md#experimental-python-profile),
[client](host.md#implemented-pybricks-subset) and
[recovery](settings.md#system-watchdog) evidence and limits.

| Steps | Current foundation | Work still required |
| --- | --- | --- |
| 0 | Task and hardware watchdogs; USB recovery checks on F4 | Physical shutdown/button restart and battery-only recovery acceptance |
| 1–4 | Protected VM, RAM execution, native/Viper and C `.mpy`, named programs and imports; ARM and F4 USB/BLE checks | Preserve regression coverage and finish combined memory, storage and power-cycle acceptance |
| 5 | Common streams/resources, standard modules and REPL with interactive output flow control; ARM/CLI and Inventor F4 USB/BLE interaction and exit/reset checks | Combined interactive flow-control acceptance |
| 6 | RAM execution, persistent slots and native ABI 6.3 negotiation; ARM checks, an F4 hub example through both actual clients and unchanged Pybricks Code execution | Profile negotiation, power-cycle slot retention and combined client acceptance |
| 7–9 | Tools and initial hub bindings: buttons, static/animated lighting, display text/animation, speaker tones/notes, system information/stop/shutdown, user-data storage, battery and IMU observations/frame/heading/calibration controls; shared native services | Storage reset, remaining hub/device bindings, missing shared-service behavior and required reference examples |
| 10 | Existing build profiles and application tooling | Combined acceptance, reproducible dependencies and delivery |

The remaining delivery order groups related APIs into usable increments. The
detailed steps below remain the acceptance contract; their implemented parts
are not work to start again.

The [workspace summary](builds.md#setup-and-board-bring-up), short
[project orientation](../AI/context.md) and
[incremental check commands](testing.md#selecting-checks) provide the development
workflow for these blocks. Plain and managed REPL exit/reset have ARM/PTY and
Inventor F4 USB/BLE checks. Ctrl-D rebuilds Python state and Ctrl-X returns to the
same usable Shell through both frontends.

| Delivery block | Remaining work | Usable result |
| --- | --- | --- |
| Interactive use | Complete combined interactive flow control; preserve the checked plain Shell and managed REPL exit/reset paths and visible hints | Slow input/output consumers and monitoring remain usable together with interruption, exit and reconnection |
| Pybricks clients | Resolve the profile warning, complete slot power-cycle retention and combined input/output acceptance; extend browser checks to covered reference examples | Reference examples upload, run and stop through the browser over BLE and pybricksdev over USB/BLE; management remains available after reconnect |
| Hub APIs and observations | Complete charger source detection (`connected()`) and storage reset; current/status use the shared service; accept physical menu selection; isolate and fix IMU acquisition gaps under BLE transfers | Reference hub examples share the existing hardware services, with valid observations or explicit errors and reliable cleanup |
| Wired devices | Accept physical writes, the six sensor classes, Light/ColorLightMatrix and PFMotor; complete UART custom-supply permission/persistence and physical serial acceptance | Reference programs use device modes and measurements; generic access and the existing terminal inventory expose unknown compatible devices |
| Motors | Accept reference examples/physical motion; coordinates, motion, shared control/model settings, motor/controller logging and blocking/awaitable bindings have synthetic ARM checks | Reference motor programs complete or cancel without stranding port ownership or active outputs |
| Robotics | Implement `DriveBase`, `SpikeBase` and `Car`, coordinated motion, gyro use and multi-port ownership | Reference robot programs use the shared controllers; partial acquisition or cancellation releases all affected outputs |
| Wireless devices and messaging | Complete hub radio arguments, remote/controller bindings, `LWP3Device`, `AppData`, `BLERadio`, `HubNetwork` and required aliases | Reference communication programs connect, exchange data and release peripheral links while preserving one management owner |
| Integrated delivery | Accept the full and reduced profiles, combined user workflows and physical behavior; publish and pin required dependency commits, then add CI for clean builds, focused checks and artifacts | Reproducible firmware with usable instructions and explicit hardware evidence and limitations |

Before expanding the F4 profile, account for static reservations, stacks, Python
heap and peak loading memory separately. Preserve the checked VM, native module,
store and REPL paths. Reuse configured builds and select checks using the
[incremental validation policy](../CONTRIBUTING.md#checks-and-pull-requests);
full integration belongs to acceptance of a large milestone.

Storage-reset semantics remain a decision before that API is implemented: specify
whether Pybricks slots, shared user data, named applications and Brickphyr settings
are cleared or preserved. Do not infer permission to erase existing user data
from permission to install firmware.

Group pending physical checks into bounded sessions: interactive flow control,
slot retention and automatic user-data saving across shutdown, watchdog recovery
on battery power, button restart, and sensor/actuator observations. Before an
unattended fault experiment, verify watchdog recovery for the power state under
test. Missing hardware or operator observations delay that acceptance, not
independent software work. Ordinary development can proceed with the existing
watchdog; independent hardware reset equipment is not a prerequisite. Watchdog
recovery limits still apply.

## Target behavior

One MicroPython engine executes temporary programs from RAM, installed named
programs and interactive REPL sessions. Pybricks clients use that same engine
through their protocol adapter. Only one application executes at a time. A
temporary upload/run requires no application-storage write; persistent installation
is explicit. USB and BLE provide the same native application operations.

Support Python source, compatible `.mpy` bytecode, native/Viper functions and
standard dynamic C `.mpy` modules for the selected architecture and runtime ABI.
Modules tied to private Pybricks structures or fixed firmware addresses require
adaptation. Reuse MicroPython's compiler, import machinery, native loader, GC and
REPL instead of implementing alternatives.

Use the MicroPython revision selected by the reference Pybricks build, with its
matching compiler. The selected revision and inspected upstream delta are recorded
in the [core integration](../src/python/README.md). Reuse suitable Zephyr-port
code; Brickphyr owns the integration with application
lifetime, memory domains, transports and hardware services. Keep dependencies in
their owning repositories and preserve licenses and provenance for adapted source.

The VM and loaded native code belong to the application's protected execution
domain. Compilation, loading and imports must not grant unrestricted kernel or
flash access. The supervisor owns resource cleanup independently of Python
finalizers. A native loop must remain stoppable, and a failed Python application
must permit a subsequent application to start.

Each independent start receives a fresh Python state. REPL retains its state
between commands. A normal host disconnection is not an application stop;
reconnection must allow control to resume without starting another VM. Explicit
REPL exit ends that application and returns input to Shell. Starting REPL while
another application runs must respect the existing confirmed-stop/admission
contract. Running Python holds power under the existing application policy.

Interruption, application stop and system recovery are distinct operations:

- Ctrl-C requests a catchable Python interruption. It must not silently become
  a hub reset when a program handles or delays that interruption.
- Explicit stop belongs to the supervisor and must also terminate a native loop
  that does not return to the VM. Device cleanup cannot depend on Python running
  a finalizer or `finally` block.
- Watchdog recovery handles lost system progress. It resets to management with
  no application running; reconnecting does not restart the failed program.

Check these boundaries through scripts, REPL and Pybricks clients. A normal
application failure should not require a system reset or physical intervention.

Installed programs may import their accompanying modules. A running application
must retain a consistent version of its code: an update must not substitute new
modules into an older running instance. Reuse the existing filesystem and atomic
installation principles. Do not introduce a second filesystem or package manager.

Adapt the Pybricks C bindings to the same services used by native applications.
Preserve useful Python semantics, including arguments, units, errors, waits and
cancellation. Keep additional Brickphyr features distinguishable. Do not run a
second PBIO hardware scheduler or expose internal driver objects to Python.
Unchanged Pybricks Code must upload and execute the covered reference examples
over BLE. The existing pybricksdev USB transport and BLE client must use the same
execution handlers; browser USB support is a separate client capability.

The milestone includes the device-service work needed by the agreed F4 Python
API and example coverage. An unimplemented operation must fail explicitly during
development, but that error does not complete a required API. Other hub ports,
WASM, automatic application startup and unrestricted CPython package compatibility
are outside this milestone.

## Implementation sequence

Every step ends with usable behavior and focused checks through its production
path. Native simulation, ARM emulation, an F4 build and physical verification are
distinct evidence. Group related methods and their shared service work into one
increment with an executable example, source review and build/check cycle.

The runtime and common user operations in steps 1–5 are the foundation for the
Pybricks execution adapter and hardware bindings in steps 6–9. Reuse that
foundation while its remaining physical acceptance proceeds separately. Step 0
gates unattended hardware fault checks, not independent software development.
Existing watchdog and core checks do not complete later integration steps.
Step 10 accepts the combined result; unfinished required features remain part of
this milestone.

The checkpoints below summarize the detailed acceptance criteria that follow.
They describe required results, not completed implementation.

| Step | Usable result |
| --- | --- |
| 0. Recovery | A system hang resets into working management with applications stopped and motor outputs inactive |
| 1. Runtime baseline | The pinned VM executes source and reference-client bytecode within an explicit F4 memory budget |
| 2. RAM execution | A temporary script runs, reports output and can be stopped through USB or BLE without a storage write |
| 3. Native Python code | Native/Viper functions and dynamic C `.mpy` execute under application memory protection |
| 4. Programs and imports | Named programs and accompanying modules install atomically and run from the common store; temporary bundles stay in RAM |
| 5. Interactive use | Shell, CLI and REPL support input, interruption, reconnection and a Python reset without rebooting the hub |
| 6. Pybricks clients | Unchanged Pybricks Code and the selected pybricksdev integration upload, run and stop Python through the shared supervisor |
| 7. Hub and tools | Reference hub, timing, geometry and multitasking examples use the existing services |
| 8. Robot devices | Sensor, motor and DriveBase examples preserve units, waits, ownership and cleanup |
| 9. Compatibility coverage | Remaining required F4 APIs and examples have executable coverage and explicit physical acceptance status |
| 10. Delivery | The integrated build passes ordinary user workflows from cold start and has reproducible dependencies and usable instructions |

### 0. Establish watchdog recovery

Use the STM32 independent watchdog through Zephyr's driver and Task Watchdog
as the recovery prerequisite for unattended hardware experiments. Supervise
progress of essential system work, with deadlines derived from legitimate
initialization, storage and
device waits. An idle connection is not a stalled service. Application loops
remain the supervisor's responsibility; a watchdog reset is recovery from system
failure, not the normal way to stop a program.
Feed supervision only after the watched work makes progress; an unrelated timer
that keeps feeding during a service deadlock defeats the recovery guarantee.

Cover startup from the earliest usable initialization point and record the
remaining unprotected interval. Preserve shutdown with USB attached, button
restart and the existing reset-to-DFU path. The running IWDG cannot simply be
disabled before reset: the USB-powered off loop must continue its required
supervision without bringing the hub back on. Preserve the factory loader and
its protection under [recovery](recovery.md).

Report the reset cause on the next boot. After a watchdog reset, management must
be available, applications stopped and motor outputs inactive. A diagnostic must
not repeat itself after reboot.

Acceptance: short simulated checks cover supervision and power states. On F4
with USB attached, a one-shot hardware expiry returns a working console with the
watchdog reset cause; an expired service channel also recovers. Normal operation
does not spuriously reset, and DFU remains reachable. Check legitimate long
operations, including program installation, compilation, GC and device cleanup,
as they become available. After recovery, verify that settings and previously
installed programs remain usable. Exercise reset during installation against
disposable simulated storage before attempting a physical fault: interrupted
staging must not destroy the last committed program or trigger automatic erasure.
Also verify shutdown and button restart with USB left
attached, and recovery on battery power: an off hub must not restart merely
because watchdog supervision expired. Unattended fault experiments require
recovery evidence for the power state being exercised; pending battery-only
checks do not block work under a verified USB recovery path. Independent software
work can proceed without a hub. Watchdog recovery does not prove recovery from
broken boot code or an interrupted firmware write.

### 1. Establish a running VM and compatibility baseline

Inspect the MicroPython revision selected by Pybricks, its fork delta and the
client compiler formats. Pin one runtime and matching compiler in the existing
workspace/build system. Build a minimal embedded VM for simulation and F4;
execute source and a reference-client bytecode program in simulation.

Inventory the public F4 API and reference examples needed for completion,
including required standard MicroPython modules, imports and client features.
Record example provenance, expected behavior and physical prerequisites. Derive
cases from the selected source/docs, not an invented list of matching names.
Missing hardware services become explicit work in steps 7–9; they do not silently
reduce the compatibility target.

Acceptance: both program formats execute; the F4 image links with a budget for
firmware, stacks, compiler, heap, native code and peak loading overhead. Check
FPU requirements and context handling before selecting the native `.mpy` ABI.
Reuse the inactive engine's application allocation where practical instead of
reserving a permanent heap for every runtime.
Compiler, runtime and client compatibility must be reproducible from pinned
dependencies; a separately installed, unspecified `mpy-cross` is not the baseline.

### 2. Run protected Python applications from RAM

Extract only the shared lifecycle code needed from the existing native supervisor.
Add Python loading, memory-domain setup, start, output, stop, join and cleanup.
Reuse existing upload and lifecycle operations so this step delivers a temporary
source/bytecode program runnable from `brickphyrdev`, not just a private test entry.
Provide fresh interpreter state and release all application resources on exit.

Acceptance: normal return, syntax/runtime errors, memory exhaustion and a
non-yielding Python loop leave management responsive; forced stop permits the
next program to run. Check input/output backpressure, disconnect and power holds.
Existing native lifecycle behavior remains working. Simulators cover faults;
bounded physical smoke checks follow the watchdog gate.
Repeated Python/native transitions must reclaim the previous engine's memory and
hardware ownership. Measure peak loading memory and stack use through this public
launch path, not only the isolated interpreter fixture.

### 3. Support native and Viper `.mpy`

Integrate MicroPython's maintained native loader, executable allocations,
relocations and GC roots in that same protected domain. Support native/Viper
functions and a small separately compiled dynamic C module; document the matching
compiler/architecture/ABI. Expose no unrestricted kernel or flash-controller API.
Preserve write/execute separation: executable application code is not writable,
and ordinary application data is not executable. Account for MPU regions and
native-code allocation in the same memory budget as the compiler and GC heap.

Acceptance: source decorators and precompiled modules calculate correct results;
native-code references survive GC, repeated runs reclaim memory, and
incompatible/truncated images are rejected. ARM emulation checks a non-yielding native loop and prohibited
memory access followed by a successful program. Physical checks initially use a
bounded calculation only. Importing these modules joins the common import path in
step 4; direct module execution alone does not complete native-module support.

### 4. Install named programs and their modules

Extend validated installation, listing, starting, replacement and removal to
Python through the common store. Select a minimal representation for a single
file and its optional accompanying modules. Reuse import machinery, resolve paths
within the application namespace and preserve the consistent running-version
rule. Keep settings and existing native applications intact. Upload/install must
not implicitly stop or start an application.

Acceptance: RAM and installed programs import source and compatible `.mpy`
modules; several named applications coexist and run by name from the hub Shell.
Temporary multi-module runs keep both the entry point and accompanying modules
in RAM; resolving an import must not silently persist the uploaded program.
Cover source, bytecode and native-module imports through that same resolution
path, including GC after import and a fresh namespace on the next application run.
Interrupted or rejected replacement preserves the previous version. Installed
programs survive reboot, temporary programs do not. Path escape, exhausted storage
and interrupted writes are checked against disposable simulated storage.

### 5. Complete Shell, CLI and REPL interaction

First complete the common program input/output path: `sys.stdin`, `sys.stdout`,
`sys.stderr`, `print()`, `input()` and interrupt delivery must serve ordinary
scripts, REPL and later Pybricks clients. Transport adapters must not implement
separate interpreters or hardware APIs. Define bounded buffering and visible
overflow/disconnection behavior; a program must not lose system control because
its output consumer stopped reading.

An attached interactive consumer must apply backpressure to the VM instead of
silently losing output. Waiting for that consumer must remain interruptible and
must not block essential services. Detaching releases the wait and restores the
bounded, counted log behavior for autonomous execution. Apply this contract to
native Shell/CLI attachment as well as the Pybricks adapter through the common
stream path.

Finish the user-facing application commands and add explicit REPL entry/exit.
Reuse MicroPython's interactive implementation and the existing transport owner:
only the selected Shell or REPL consumes input. Support multiline input, editing,
completion/history, `input()`, output, tracebacks and interruption. Exiting REPL
returns to the same Shell. Reconnection attaches to the existing application;
it does not create another interpreter or replay unfinished input.
Distinguish MicroPython soft reset from hub reboot: a soft reset releases the
session's application resources and initializes a fresh interpreter while keeping
management and essential device services running. Keep explicit Shell return
separate from the standard REPL reset action. Show Ctrl-X exit and Ctrl-D soft
reset hints on entry. Verify and, if needed, fix Ctrl-X return through both plain
Shell `python` and `brickphyrdev terminal --repl` over USB and BLE; acceptance
requires the same Shell to remain usable after REPL stops and releases its
resources, while Ctrl-D continues to reset Python.

Deliver this in three usable increments: interpreter entry and interruption over
the existing application operations; interactive Shell/CLI attachment over USB
and BLE; then reset, detach/reattach and device-cleanup acceptance. Reuse the
existing host terminal's keyboard, terminal-state restoration and flow control.
The Shell and REPL have different command languages but must share transport
ownership; changing the input consumer must not leave two readers or prevent
management from stopping an application. Check the maintained friendly and raw
REPL paths without implementing another Python command parser.

Enable and check the standard MicroPython modules required by the selected
[reference surface](compatibility.md#initial-coverage-plan), including stream,
numeric, serialization, polling and memory-management operations. Reuse their
upstream implementations. Provide read-only access to bundled resource files
through `open()` with the same namespace and lifetime as imports; temporary
programs must not write flash merely to read their resources. General writable
Python storage is not implied by this read-only program namespace. Required
Pybricks persistent-data APIs belong to step 7 and must use their documented
storage and lifetime rules.

Acceptance: use the same firmware operations from `brickphyrdev` and the USB/BLE
terminal. Check both transport adapters, partial input, blocked sends, Ctrl-C,
soft reset, explicit exit and disconnect/reconnect. Keep input bounded without
dropping ordinary typed characters or terminating a session on transient
backpressure. Synthetic BLE delivery is labelled as such. Provide copyable
commands for temporary run, install/list/run/remove and
REPL only once their actual CLI is implemented.
Check resource reads, missing files, namespace boundaries and rejection of writes,
including after a stored program is replaced while its previous instance runs.
The standard-module inventory must distinguish working operations from omissions;
an import that succeeds with unusable methods is not acceptance.

### 6. Integrate the Pybricks execution protocol

Connect version/capability reporting, transfer validation, start/stop, program IDs,
stdin/stdout and running status to the same supervisor. Reuse Pybricks packaging
and multi-module imports; map persistent client slots to the common store without
exposing reserved staging files as programs. Advertise only actual capabilities.
Validate the protocol/profile version against the selected clients, including
the currently observed Pybricks Code profile warning.

Negotiate bytecode and native `.mpy` formats with the actual client compiler and
loader. A native capability is accepted only after a client-produced native/Viper
program and a compatible dynamic C module execute through this protocol path.
Reject ABI or architecture mismatches before replacing an installed program;
changing a capability flag or successfully transferring bytes is not acceptance.

Deliver three increments: temporary transfer/execution and input/output;
persistent slot installation/selection through the existing atomic store; then
complete client acceptance over USB/BLE and unchanged Pybricks Code over BLE.
Temporary uploads must remain usable without installing a program. An incomplete
transfer must not replace a good installed slot, and a reconnect must not restart
an application or discard a completed RAM upload. Slot operations must preserve
named native/Python programs and settings. Profile changes must preserve the
existing single management owner and application lifetime.

Acceptance: the reference client's compiled multi-module program uploads and
runs through the real protocol handlers. Output, exceptions, input, stop,
interrupted upload, a second run and reconnection work. Exercise pybricksdev over
BLE and the existing USB integration. Run a simple program through unchanged
Pybricks Code over BLE; discovery alone does not complete this step.
Include duplicate/invalid module names, size and ABI mismatches, a busy runtime,
slow output consumers and interruption at transfer boundaries. Protocol errors
must preserve the next valid transfer and report failure without claiming an
unsupported operation succeeded. Check installed-slot persistence after reboot
and RAM-only behavior for temporary runs.

### 7. Integrate Python tools and hub APIs

Reuse Pybricks bindings for parameters, geometry, timing and asynchronous tools,
including `async`, `await`, `multitask` and `run_task`. Preserve waits, results,
exceptions and cancellation while Zephyr continues essential services.

Port the selected hub API over existing buttons, lighting, matrix, sound, battery
and IMU services. Complete required system/storage and calibration behavior.
Add the distinguishable Brickphyr hardware extensions needed by the native Python
workflow, such as Bluetooth-button lighting, with explicit resource ownership.
Python hardware operations and diagnostics must share the existing services.

Preserve the implemented tools, light, display, sound, system/storage and IMU
bindings while completing the remaining hub surface. Group related operations
with a working script using the public API and its relevant failure or
cancellation path. Port missing service behavior at its existing owner before
exposing the binding; do not reproduce it in Python-specific drivers.

Acceptance: reference hub/tools examples run unchanged with checked values,
timing semantics and cleanup. Device I/O is simulated where physical observation
is unavailable. Canceled or failed programs release owned devices. Each increment
ends with a usable script rather than empty module types.

### 8. Integrate sensors, motors and DriveBase

Adapt `pupdevices`, applicable `iodevices`, motor control and `robotics.DriveBase`
using the existing port owner, measurements and PBIO algorithms. Complete missing
service behavior required by the reference, including coordinated motion and
gyro use where exposed. Preserve direction, gearing, units, limits, completion,
stall and stop modes; Python awaitables observe the same asynchronous operation.

Deliver raw port access and wired sensors first, one-motor operations next, then
coordinated robotics APIs. Keep existing working direct-drive and trajectory
services in use. Account for multi-port ownership and cancellation before adding
DriveBase: a partially acquired robot must not leave one motor running when the
other port is unavailable.

Acceptance: unchanged reference sensor, motor and DriveBase examples pass through
the bindings and real service logic with simulated I/O. Cover hotplug, invalid or
stale measurements, interrupted motion, cancellation and resource cleanup. An
absent physical sensor delays its hardware acceptance, not its software port.
Wheel motion and trajectory accuracy require separate authorized physical checks.

### 9. Close remaining reference API and example gaps

Review the step-1 inventory against executable coverage. Complete remaining F4
features it requires, including hub BLE communication, controllers or charging
observations where present in the selected API. Reuse their underlying services;
respect the existing host-session policy and discuss any necessary contract
change before implementation. Do not substitute fabricated values or exclude a
required example merely because its implementation needs another device service.

Acceptance: each required example has a passing software case and an explicit
physical verification status. Record intentional differences from upstream bugs
or private internals. Required unresolved features keep the milestone incomplete;
changes to its agreed scope need a decision, not a success label.

### 10. Accept and deliver the integrated Python build

Build the canonical F4 configuration with Python, compatibility and the existing
native facilities. Check final flash/RAM budgets, cold start, repeated execution,
GC/native-module lifetime, idle/active power behavior and continued USB/BLE
management. Engine options must allow a smaller build without Python and its
dependencies. Preserve the existing native profile and firmware recovery path.
Also build Python without the optional Pybricks library and protocol execution
adapter: named scripts, imports and REPL must remain usable through the native
USB/BLE interface. All three profiles use the same application supervisor and
hardware services; disabling compatibility must not select another Python engine.
Exercise each ordinary first-use workflow from cold start, without diagnostic
reads or warm-up commands establishing hidden prerequisites.

Run the complete user workflows on the physical hub: temporary script with
imports, named installation and restart, terminal launch, REPL, a bounded native
module, and representative unchanged examples from Pybricks Code and pybricksdev.
Use actual clients and record board/build/client/runtime versions. Complete the
remaining sensor, actuator and operator-observation cases before claiming their
physical compatibility.

Acceptance: publishable usage/support documents describe working commands,
runtime ABI, capacities and evidenced limits. A clean workspace can resolve the
documented dependencies and reproduce the build; shared manifests never point to
unpublished dependency commits. Prepare a coherent release candidate and concise
change description. Firmware rollout and publication follow their own current
authorization.

## Completion boundaries

The delivered build must let a user perform all of these workflows:

- Run a source or compatible `.mpy` program from the computer without persisting
  it, then stop it and start another program after return, exception or forced stop.
- Install several named programs with imports, reboot and launch any of them from
  the hub terminal, with rejected updates preserving the previous installation.
- Enter REPL over USB or BLE, inspect and control supported hardware, interrupt or
  reset Python, and return to the same terminal without rebooting the hub.
- Use program input/output, the required standard modules and bundled resource
  files through the same runtime from scripts, REPL and the Pybricks interface.
- Run native/Viper functions and a compatible dynamic C `.mpy` module in the same
  protected runtime, including imports, GC and cleanup after termination.
- Upload, run and stop the reference Pybricks examples through unchanged Pybricks
  Code over BLE and through the selected pybricksdev USB/BLE integration.
- Use the required hub, sensor, motor and robotics APIs with Pybricks waiting and
  multitasking semantics while system services remain responsive.
- Use the existing native applications and build a reduced image without Python.

The software milestone is complete when all required paths and reference cases
above work, not when the VM first prints a message. Runtime support, individual
Pybricks APIs and physical device behavior have separate acceptance. A missing
sensor or operator must not prevent progress on the remaining software checks,
but pending physical acceptance must remain visible at delivery. Simulation must
not be presented as physical compatibility or proof that every Pybricks example
works.

Use the following completion gates when reviewing the final result:

| Gate | Required evidence |
| --- | --- |
| Runtime and interaction | Source, bytecode and native modules use one protected lifecycle; imports, streams, REPL, interruption and cleanup pass through the public operations |
| Reference compatibility | The selected F4 API inventory is covered by executable cases, with no required method left as an unsupported stub; actual clients run the covered examples |
| Hardware acceptance | Each physical claim identifies the board and checked device behavior; unavailable devices and operator observations remain pending |
| Delivery | Dependencies resolve from documented revisions, full and reduced builds work, and instructions reproduce the supported user workflows |

A software-complete result may still await named physical checks. The complete
Python milestone remains open until the required target workflows and physical
acceptance pass; finishing an unattended work session does not close those gates.
Neither software completion nor a successful client connection establishes
complete hardware compatibility. Do not remove difficult APIs from the required
inventory to close the milestone. Additional features beyond the selected
reference are separate work unless already included above.

## Unattended validation

Use short tests for observable behavior and regressions through production paths;
do not test source contents. Invalid images, memory faults, unbounded execution
and storage-failure scenarios belong first in simulation/ARM emulation. Hardware
checks have bounded duration, one connection owner and explicit cleanup. If
automatic recovery fails, stop hardware experiments and continue offline work.
Reset does not roll back firmware or guarantee recovery before watchdog startup.
Before an unattended fault check, retain the known working image and verify the
current application's stopped state. Run each fault trigger once, then verify
the reset cause, restored management and inactive motor outputs. A lost diagnostic
connection alone does not justify repeating the fault. Checks needing a button
press, power-cycle, visual judgement or unavailable sensor remain pending while
independent implementation and simulated checks continue.

After each authorized firmware installation, check ordinary startup, management
and watchdog status before exercising application faults. A rollback image is
useful only while the installer remains reachable; do not treat it as an automatic
recovery mechanism. Keep fault injection out of startup and persistent programs.
If the hub becomes unavailable, preserve the first failure and continue with
builds, emulation, bindings and client checks that do not need it. The unattended
work ends with a stopped application and closed diagnostic links; outstanding
physical checks remain explicit acceptance items.

Extend the owning usage documents as commands become available. Keep support
status and dependency revisions current. Captures, measurements and operational
handoffs belong in ignored `reports/`, not in this plan.
