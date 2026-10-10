# Architecture

This is the selected design. Implementation status belongs in
[hardware](hardware.md) and [builds](builds.md).

## Purpose and boundaries

Brickphyr is a reusable application platform for LEGO hubs. Its first useful
endpoint is a separately uploaded native application that reads sensors and
commands motors without starting Python. Python compatibility and WASM use the
same hardware services; they are part of the architecture from the beginning.

The firmware contains device support, execution engines, and application lifecycle
management. Robot-specific estimation, planning, balancing, and learning remain
applications. In particular, babybot is a separate repository consuming an SDK
and an installed firmware, not a component compiled into Brickphyr.

One user application runs at a time. This does not mean one system thread:
hardware, communication, and lifecycle services must continue independently.
General-purpose multi-process scheduling and simultaneous native/Python/WASM
applications are outside the initial scope.

One Zephyr system serves all execution engines and host interfaces. Selecting a
Pybricks-compatible or extended management interface does not switch operating
systems. Use useful POSIX APIs where they help portability; full POSIX conformance,
a Unix process model, and a POSIX shell are not project goals.

## Execution and ownership

```text
one application: native | MicroPython + Pybricks | WASM
                         |
             bounded hardware/service interface
                         |
       LEGO device and reusable control algorithms
                         |
        Zephyr drivers, kernel, and communication
                         |
                    hub hardware
```

This is a dependency view, not a requirement to wrap every call in another layer.
[Subsystems](subsystems.md) assigns implementation ownership. Reuse Zephyr where
it provides the general mechanism and Pybricks where it provides LEGO-specific
behavior. Do not run the entire cooperative PBIO system as one opaque task and
claim its original scheduling dependencies have disappeared.

Each hardware resource has one owner. A port is not concurrently controlled by a
raw PWM writer, a motor servo, and a diagnostic tool. Runtime bindings translate
language values at the boundary; they do not reimplement the device behavior.
Sensor snapshots and command results carry the timestamps and validity needed to
interpret them without guessing how long a call took.

Use interrupts/DMA for peripheral work that requires them and bounded Zephyr
services for deferred processing. A slow application cannot hold a lock needed
by essential device service indefinitely. Long-lived ownership and temporary
locks are different: a program may own a motor while a firmware thread services it.
Driver callbacks and queues need explicit cancellation and lifetime rules.

## Isolation and application lifetime

Use Zephyr user mode and MPU protection on the initial capable target. Native
application entry, constructors, and cleanup execute in the application's domain.
Drivers and the privileged supervisor remain trusted. Protect firmware data and
flash controls; export only the services an application needs.

This choice is made. Measure its costs during implementation rather than treating
unmeasured overhead as a reason to postpone isolation. MPU protection does not
prove freedom from kernel bugs, starvation, or incorrect driver cleanup. Smaller
hubs may require a different documented build; do not silently weaken protection.

The supervisor owns loading, stopping, resource release, and unload. A program
fault ends that application and reports its cause while preserving the platform
where possible. Stopping must quiesce device callbacks and DMA before releasing
application memory. Normal host disconnection leaves an autonomous application
running. See [applications](applications.md) for the lifecycle contract.

## Time and memory

Native applications must be able to use caller-owned buffers and run without a
garbage collector or per-step heap allocation. GC belongs to runtimes that need
it; it must not suspend essential hardware services. Native code is not a proof
of bounded latency by itself: driver waits, interrupt masking, locks, and radio
work also count.

Use one monotonic timebase for measurement and command timing. A periodic app
schedules against absolute deadlines, with explicit overrun reporting, rather
than hiding elapsed work by resetting its clock after each cycle. No universal
robot control period is imposed by the firmware.

Choose priorities, stacks, buffers, and queue bounds from service requirements,
then verify the complete workload. Report flash, static RAM, stacks, runtime heap,
application memory, and peak loading overhead separately. Avoid reserving large
heaps for execution engines that are not active.

Program capacity follows the board's real storage, execution-memory, and recovery
constraints. Do not copy the five-slot size division from existing firmware. More
external flash does not automatically provide executable or directly mapped RAM.
File/storage layout is still to be selected; [recovery](recovery.md) defines the
regions that cannot be repurposed casually.

## Compatibility and evolution

The Python surface preserves specified Pybricks behavior. The initial native ABI
is experimental and paired with a matching SDK; application format and required
capabilities determine admission. A firmware build selects available engines;
source language alone does not select a runtime. Unsupported formats do not cause
an implicit firmware replacement.

[Host connections](host.md) define a single computer management session and
interface selection. [Settings and power](settings.md) define shared configuration,
explicit persistence, and inactivity shutdown independently of user programs.

Keep future extensions local to an existing boundary. Add a controller protocol,
device driver, or transport when there is concrete hardware and a test, not a
universal framework in anticipation of all possible devices. See
[compatibility](compatibility.md) and [builds](builds.md).

## Upstream foundations

- [Zephyr user mode](https://docs.zephyrproject.org/latest/kernel/usermode/overview.html)
  supplies memory and device-access mechanisms, not a complete application manager.
- [LLEXT](https://docs.zephyrproject.org/latest/services/llext/index.html) supplies
  native loading; lifecycle and the public hardware contract remain our work.
- [PBIO](https://github.com/pybricks/pybricks-micropython/tree/master/lib/pbio)
  contains reusable behavior and platform-dependent execution that must be separated.

These upstream references describe mechanisms, not Brickphyr measurements or
pinned build versions.
