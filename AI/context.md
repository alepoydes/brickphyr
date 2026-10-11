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
provides the BLE terminal and USB selection/identity operations.

Normal installation, Pybricks 4.1.0b4/b6 restoration and return to Brickphyr pass
on that hub. Linux USB/BLE checks cover reconnection, editing/history, protocol
handover, partial-input isolation, unread USB output followed by BLE admission,
SMP settings and persistence. Pybricks Code discovery and reconnection pass in
Chromium Web Bluetooth, with a profile-version warning. Program upload/execution
is not implemented. An opt-in native profile now provides a matching C SDK,
protected LLEXT execution, RAM uploads and lifecycle control through SMP,
`brickphyrdev app` and the shared Shell. Separate SDK images, constructors,
soft-float arithmetic, MPU fault recovery, forced stop, interrupted transfers,
USB/BLE reconnection and application power holds pass on the Inventor F4.
Installed applications and Python/WASM engines are not implemented.

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

## Dependencies and remaining work

Dependencies remain separate west repositories. Portable corrections belong in
Pybricks, pybricksdev and Zephyr; [upstream integration](../docs/upstream.md) records
their purpose and publication status. Never replace a pinned dependency with a
source copy or silently discard local dependency work during `west update`.

The [application milestone](../docs/builds.md#native-application-milestone) continues
with several installed applications in LittleFS.
Use the existing protected lifecycle for both image sources and both transports.
Completed RAM uploads can be started from a later session; incomplete uploads
are discarded on disconnect without stopping the application.
The native ABI is experimental. Fixture builds require the additional Zephyr
LLEXT build fixes described in [upstream integration](../docs/upstream.md).
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
