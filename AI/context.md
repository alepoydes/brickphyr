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
Chromium Web Bluetooth, with a profile-version warning. Pybricks program upload/execution
is not implemented. An opt-in native profile now provides a matching C SDK,
protected LLEXT execution, RAM uploads and lifecycle control through SMP,
`brickphyrdev app` and the shared Shell. Separate SDK images, constructors,
soft-float arithmetic, MPU fault recovery, forced stop, interrupted transfers,
USB/BLE reconnection and application power holds pass on the Inventor F4.
The profile also installs several applications in LittleFS through standard SMP
and runs them from the same Shell/CLI lifecycle. Python/WASM engines are not implemented.

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
Bluetooth waiting flashes have a three-second period. Synthetic acquisition,
protected ARM SDK and SMP checks cover these new services; physical display,
IMU accuracy/timing, audio and voltage/current verification remain pending.
Electrical A–F detection now has native/Shell/SMP snapshots with synthetic GPIO
and protected ARM checks. UART negotiation, external sensor data and motors
remain the next implementation work; physical port identification is unverified.

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
