# Project orientation

Brickphyr is an open application platform for LEGO hubs, built on Zephyr RTOS.

Read [AGENTS.md](../AGENTS.md), [CONTRIBUTING.md](../CONTRIBUTING.md), and
[the architecture](../docs/architecture.md), then the document owning the task:

- [Builds](../docs/builds.md) and [testing](../docs/testing.md): setup and checks.
- [Hardware](../docs/hardware.md) and [recovery](../docs/recovery.md): board support
  and firmware installation constraints.
- [Subsystems](../docs/subsystems.md): dependency and implementation ownership.
- [Compatibility](../docs/compatibility.md) and [Bluetooth](../docs/bluetooth.md):
  Pybricks behavior, host interaction, and radio integration.
- [Applications](../docs/applications.md): native, Python, and WASM lifecycle.
- [Python](../docs/python.md): runtime, REPL, Pybricks integration and acceptance plan.
- [Host connections](../docs/host.md): interface selection and one management session.
- [Settings and power](../docs/settings.md): runtime changes, explicit save, and shutdown.
- [Upstream](../docs/upstream.md): dependency changes and contributions.

## Current implementation

The experimental `spike_prime_f4` image runs on one Robot Inventor F4 (PCB revision
unknown). It provides four buttons, central/Bluetooth lighting, USB Shell,
Pybricks identification, settings, inactivity shutdown, button restart and DFU
entry through the standard Pybricks installer. The optional Bluetooth profile
adds Pybricks identity/status, standard SMP and the same interactive Shell over
BLE. USB and BLE share one management owner. The installed `brickphyrdev` CLI
provides USB/BLE terminals, monitoring and USB selection/identity operations.

Normal installation, Pybricks 4.1.0b4/b6 restoration and return to Brickphyr pass
on that hub. Linux USB/BLE checks cover reconnection, editing/history, protocol
handover, partial-input isolation, unread USB output followed by BLE admission,
SMP settings and persistence. Pybricks Code discovery and reconnection pass in
Chromium Web Bluetooth, with a profile-version warning. Pybricks execution is
experimental in the optional Python profile; see its [client coverage](../docs/host.md#implemented-pybricks-subset).
An opt-in native profile now provides a matching C SDK,
protected LLEXT execution, RAM uploads and lifecycle control through SMP,
`brickphyrdev app` and the shared Shell. Separate SDK images, constructors,
soft-float arithmetic, MPU fault recovery, forced stop, interrupted transfers,
USB/BLE reconnection and application power holds pass on the Inventor F4.
The profile also installs several applications in LittleFS through standard SMP
and runs them from the same Shell/CLI lifecycle. The optional Python profile adds
temporary and installed Python programs with imports; WASM remains unimplemented.

USB/battery transitions preserve a BLE session and uptime. An active management
session inhibits idle shutdown. USB and battery expiry, long-press shutdown, and
button restart with USB left attached have operator confirmation. Saved and
runtime settings are separate; changes apply immediately and save is explicit.
See the owning documents for procedures and evidence limits.

Central cyan/yellow reactor lighting and white BT menu indication were accepted
on the Inventor F4. New Bluetooth connection indication has native SPI checks:
blue waiting flashes, cyan Pybricks and yellow native connections, with menu white
retaining priority. Its perceived colors and transitions still await operator
verification.

The F4 matrix, raw/calibrated IMU, tone/PCM speaker and battery observations now
share native APIs and Shell/SMP services. Ownership and application cleanup are
implemented for matrix and sound. IMU settings apply in RAM with explicit save;
missing measurements start a visible new orientation epoch. The CLI text monitor
supports filters, host JSONL recording and a shared terminal session over USB/BLE.
Bluetooth waiting flashes have a three-second period. Native matrix, IMU,
tone/PCM and motor examples now run on the Inventor F4 over USB/BLE and release
their hardware on stop/exit. Bounded repeated launches preserve hub responsiveness
and both attached encoder links with the required Zephyr MPU and thread-cleanup
corrections. Perceived display/audio output, calibrated IMU
accuracy and voltage/current measurement accuracy remain pending.
Electrical A–F detection and LUMP UART negotiation now have native/Shell/SMP
snapshots, including raw DATA, with synthetic GPIO/UART and protected ARM checks.
Known SPIKE/BOOST/WeDo sensors now have converted measurements, owned mode
selection and validated sensor supply in that same service. Synthetic UART/GPIO
checks cover units, settling, malformed calibration and cleanup. Read-only BOOST,
Technic and SPIKE motor encoders share that service, with synthetic turn counting,
rollover and inactive-output checks. The shared GPIO/PWM output adapter has F4
wiring and synthetic duty/direction/cleanup checks. Native/Shell direct drive
shares port ownership, asynchronous completion and coast on application cleanup
or device loss. Experimental PBIO speed/position trajectories and hold now use
that same owner/output path, a 5 ms port queue and validated battery feedback.
Synthetic UART/ADC/PWM checks cover stall, completion, input loss, deadline
overruns and release during output. Numerical ARM checks cover long angles and
timer wrapping. Two physical ID-75 motors on A/B now pass identification,
encoder direction, short direct/timed drive, position hold and release checks
with suspended wheels. Startup BREAK is accepted only before a valid TYPE.
Preemptible CDC and acquisition-first scheduling remove new IMU losses in a
bounded USB telemetry check; this is not a worst-case timing guarantee.
Physical sensor power/readings and sustained combined load remain unverified.
The shared terminal's `port` inventory and `port A` detail expose
[device metadata and raw/decoded observations](../docs/ports.md#device-inspection),
including unknown UART IDs. Synthetic checks cover malformed optional fields,
reconnects, escaped device strings and unchanged outputs. Physical third-party
compatibility remains unverified. Remaining hardware checks include physical sensor
coverage, visual/audio acceptance, calibration and motor/service timing under
combined load. Repeated BLE application transfers still produce occasional IMU
acquisition failures; their cause remains to be isolated.

The current milestone is [Python](../docs/python.md). One protected MicroPython
engine shares the application supervisor, memory arena, store and transports with
native applications. RAM and installed source/bytecode programs, imports,
native/Viper and dynamic C `.mpy`, streams and resources have ARM QEMU and Inventor
F4 USB/BLE checks. Fresh state, forced stop, fault recovery, transport handover
and application power holds are covered; see [applications](../docs/applications.md#experimental-python-profile).

Friendly/raw REPL, Shell/CLI integration, interactive output backpressure and
reference standard modules have ARM/PTY checks. Parameters, geometry, timing and
multitasking reuse Pybricks code. Initial hub bindings provide buttons, static and
animated button lighting, display text/animation/orientation and battery snapshots
through the protected SDK and common services. Synthetic SPI/ADC checks cover
validation, ownership, driver errors and cleanup, including REPL reset. On the
Inventor F4, the hub example, friendly/raw REPL, editing, soft reset and plain Shell
reconnection now pass over USB/BLE. Perceived output and combined flow-control
acceptance remain open. The remaining hub, device, motor and robotics bindings
are required work, not optional follow-ups.

The Pybricks execution adapter has protected ARM checks through the actual serial
client and a synthetic BLE adapter: RAM transfer, persistent slots, native ABI
6.3, imports, output backpressure, interruption and reconnection. Both actual
USB/BLE clients upload and repeat the hub example on the Inventor F4. Power-cycle
slot retention, broader client checks and unchanged Pybricks Code remain open. See
[client coverage](../docs/host.md#implemented-pybricks-subset) and
[library coverage](../docs/compatibility.md#implemented-library-subset).

The F4 task/hardware watchdog supervises startup, the hub loop and system queue,
records reset flags and preserves USB-powered shutdown. Interrupt-lock and
workqueue-stall recovery, USB management and subsequent DFU/reinstallation pass
on the Inventor F4. Shutdown/button behavior has simulator checks; its physical
recheck and battery-only watchdog recovery remain pending. See
[power](../docs/settings.md#system-watchdog). Final combined acceptance, reduced
profiles and published dependency pins are still required by the Python plan.

## Dependencies and remaining work

Dependencies remain separate west repositories. Corrections independently useful
to Pybricks, pybricksdev and Zephyr belong in those forks. Brickphyr-specific
adaptations belong here, including licensed ports with recorded provenance.
[Upstream integration](../docs/upstream.md) defines the boundary. Never silently
discard local dependency work during `west update`.

The [application milestone](../docs/builds.md#native-application-milestone) now has
both RAM and LittleFS image sources under one protected lifecycle and both
transports. Installation validates a staging file before atomic replacement; it
never starts or stops an application. Reboot and power-cycle persistence are checked on the F4.
Completed RAM uploads can be started from a later session; incomplete uploads
are discarded on disconnect without stopping the application.
The native ABI is experimental. Native builds require the additional Zephyr
LLEXT build fixes, MCUmgr file-close API and upstream ARM syscall register correction
described in [upstream integration](../docs/upstream.md).
IMU numerical checks additionally require its local PBIO finite-input correction.
The F4 speaker requires the local bounded STM32 DMA-stop correction.
Battery/button acquisition additionally requires the local F4 ADC timeout correction.
Motor trajectories, preemptible CDC, MPU switching and bounded thread cleanup
require the local dependency corrections described in upstream integration.
A RAM filesystem is outside the milestone.

LEGO restoration, interrupted updates, charging, residual current and host-initiated
USB wake remain unverified. Startup with a failed radio preserves USB/buttons and
shutdown on the Inventor F4; active-session radio failure/recovery is still open.
Synthetic HCI/UART checks cover bounded errors and transport cleanup, not complete
host-stack recovery. Current radio errors remain latched until reboot.

Everything tracked by Git is public. Local development handoffs, device captures,
experiment results and pending session work belong in ignored `reports/`. Recheck
workspace and hardware state before resuming; those notes are neither public
contracts nor continuing authorization to operate hardware.
