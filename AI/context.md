# Project orientation

Brickphyr is an open application platform for LEGO hubs built on Zephyr RTOS.
Read [AGENTS.md](../AGENTS.md), [CONTRIBUTING.md](../CONTRIBUTING.md) and
[the architecture](../docs/architecture.md), then the document owning the task:

- [Builds](../docs/builds.md) and [testing](../docs/testing.md): setup and checks.
- [Hardware](../docs/hardware.md) and [recovery](../docs/recovery.md): board
  support, installation and recovery limits.
- [Subsystems](../docs/subsystems.md) and [upstream](../docs/upstream.md): code
  ownership and dependency integration.
- [Applications](../docs/applications.md): shared lifecycle and storage.
- [Python plan](../docs/python.md) and [compatibility](../docs/compatibility.md):
  implementation sequence and Pybricks behavior.
- [Host](../docs/host.md) and [Bluetooth](../docs/bluetooth.md): clients,
  transports and one management connection owner.
- [Settings and power](../docs/settings.md): persistence, shutdown and watchdog.

## Current implementation and evidence

The experimental `spike_prime_f4` image has checks on one Robot Inventor F4;
its PCB revision is unknown. The image provides USB Shell, optional BLE
Shell/SMP, Pybricks identity, settings, buttons, lighting, shutdown and standard
installer/DFU recovery. USB and BLE share one management owner. The installed
`brickphyrdev` CLI provides USB/BLE terminal, monitoring and application
commands. The owning documents state each procedure and its evidence limits.

F4 charging works from a configured, awake USB host and continues after shutdown;
Python current/status observations have ARM and F4 USB/BLE checks. The F4 build
also classifies USB charger sources before attaching CDC and exposes the class
through `connected()`. CDP/DCP may charge without enumeration at the conservative
100 mA preset; physical source classification and adapter charging are unverified.
Critical-battery cutoff has synthetic ADC checks. See
[battery and charger support](../docs/hardware.md#hub-battery).

One protected application lifecycle serves the native C SDK and optional
MicroPython engine. RAM uploads and named LittleFS programs use the same
supervisor, store, Shell/SMP operations and USB/BLE transports. Application stop,
fault recovery and disconnect handling release or retain resources according to
the [application contract](../docs/applications.md); ordinary host disconnect
does not stop an autonomous application. WASM is unimplemented. The native ABI
and Python profile remain experimental.

On the Inventor F4, native and Python programs run over USB/BLE; checks cover
protected execution, imports, forced stop, reconnection and application power
holds. Native matrix, IMU, speaker and motor examples run there, including short
motor drive/hold and release with two suspended ID-75 motors. Python hub,
speaker, system, battery chemistry/temperature, REPL and explicit user-data save/reboot workflows also have
F4 USB/BLE checks. These checks do not establish perceived display/audio quality,
sensor accuracy, sustained combined load or full device compatibility.
`hub_menu` reuses the reference selection loop with the shared buttons/matrix.
Navigation and stop restoration have ARM checks; F4 USB/BLE checks cover initial
rendering and host interruption/stop, with physical selection still pending.

Native simulator and protected ARM/QEMU checks cover additional driver, motor,
storage, VM, binding, REPL and host boundaries using synthetic devices and
transports. Such checks do not establish physical USB/BLE timing or device
behavior. [Hardware](../docs/hardware.md), [applications](../docs/applications.md)
and [testing](../docs/testing.md) separate simulated from named-board evidence.

The Pybricks execution adapter has ARM checks for transfer, persistent slots,
imports, interruption, output and reconnection. USB/BLE client uploads and a
hub example pass on the Inventor F4. Unchanged Pybricks Code over BLE also passes
upload, imports, input/output, stop, repeat and reconnection there. Pybricks Beta
accepts profile 1.7 and adds checked REPL reset/interruption and asynchronous tools;
its browser compiler cannot yet build native/Viper decorators. Complete client
acceptance remains open;
see [host coverage](../docs/host.md#implemented-pybricks-subset).
The F4 watchdog has USB-powered fault-recovery checks; battery-only recovery
remains pending.

## Active Python milestone

Follow the [Python implementation plan](../docs/python.md) for the full required
surface and delivery order. Use the workspace summary and incremental checks
linked there. The next blocks complete Pybricks client execution and the remaining
bindings. Ctrl-D soft-resets Python; Ctrl-X returns to Shell.
The [full and reduced F4 profiles](../docs/builds.md#experimental-python-profile)
have package builds. Python's library and client adapter are independently
selectable; native Python interaction and adapter isolation have ARM checks,
plus bounded Inventor F4 USB/BLE checks. Reduced profiles preserve stored files;
see [profile coverage](../docs/builds.md#experimental-python-profile).
Both plain Shell `python` and `brickphyrdev terminal --repl` show entry hints and
have ARM/PTY and Inventor F4 USB/BLE exit/reset checks. Native SMP output
backpressure and combined REPL/application-monitor operation have F4 USB/BLE
checks. REPL combines output with compact state; native BLE uses negotiated
packet sizes. Monitor rates remain best-effort on the shared connection.

Remaining hub, wired-device, motor, robotics and wireless bindings are required
work. Generic `PUPDevice.info/read/reset` has ARM and idle-encoder F4 USB/BLE checks.
Writes and Force/Tilt/Infrared and Color/Ultrasonic/ColorDistance Python classes
have protected ARM and synthetic UART checks, including shared palettes, sensor
lights and port cleanup. `Light` and `ColorLightMatrix` additionally have protected
ARM checks for shared PWM/UART output, stale commands and cleanup. `PFMotor`
has synthetic UART coverage for IR commands, cleanup and reconnect errors;
physical PF output and other sensor acceptance remain pending. `DCMotor` has
ARM checks for the shared native direction/voltage configuration, PWM, hotplug
and cleanup. `Motor` adds shared gearing/origin, measurements, regulated motion,
blocking/awaitable completion, replacement and shared `control` settings/trajectory
checks. Model tuning/state and motor/controller CSV logging also have synthetic checks;
physical Python motor acceptance remains pending.
Native/Python DriveBase uses the same paired port worker and shared IMU, with
synthetic scheduling, gyro reset, cancellation and cleanup checks. `move_by`
and SpikeBase movement use that worker, with numerical and protected ARM checks.
Both classes expose shared per-axis controls and logs with protected ARM checks
for settings, trajectories, CSV units and memory cleanup. `Car` calibration,
steering, drive commands and interruption have synthetic ARM checks; physical
robotics acceptance remains pending.
See [paired drive control](../docs/ports.md#paired-drive-control).
The native motor observer now continues through DC/brake/coast while claimed;
synthetic service checks cover speed, model load/stall and feedback loss.
`UARTDevice` uses the shared UART service with ARM echo/cleanup and P1/P2 supply
checks. Custom-power permission uses the common RAM/explicit-save settings path;
physical serial and supply acceptance remain pending.
`messaging.AppData` and its legacy tools alias share the Pybricks USB/BLE
dispatcher and protected application mailbox. Transfer and lifetime behavior have
ARM and Inventor F4 pybricksdev USB/BLE checks. Passive `BLERadio` observation and
legacy hub `observe_channels` use
the shared radio and protected snapshots; F4 reception and expiry pass with a
synthetic Linux advertiser. `version()` reads cached TI script metadata.
`Remote`, `LWP3Device`, `TechnicMoveHub`, `MarioHub`
and `DuploTrain` share native LEGO BLE setup, commands and snapshots, with
protected ARM checks for operations, reconnects and lifetime cleanup.
`XboxController` uses the same peer owner with a licensed SDL/Bluepad32 codec;
standard/Elite input, rumble and pending-read cleanup have protected ARM checks.
Physical controller behavior remains unverified. `HubNetwork` uses shared native
Classic routing/inboxes with protected ARM checks for both roles, the Python
codec, forwarding and cleanup. F4 leaf exchange with a synthetic Linux peer
also passes alongside BLE management. Physical Pybricks/peripheral interoperability,
broadcasting remain pending. See [Bluetooth](../docs/bluetooth.md).
Bounded IMU USB/BLE checks pass with preemptible USB and native-code preparation; sustained combined
load and acquisition latency remain open. See [hardware](../docs/hardware.md).
Account separately for static reservations, stacks, Python heap and peak loading memory
before expanding the F4 profile. Bounded F4 USB measurements cover a maximum-size
synthetic package, native import and idle HubNetwork inboxes; they do not establish
general program capacity. See [memory limits](../docs/applications.md#experimental-python-profile).

Define `reset_storage` scope before implementing it: decide what happens to
Pybricks slots, shared user data, named applications and Brickphyr settings.
Both pybricksdev transports pass native-module, finite long-output and active-stop
checks; selected-slot execution after software reboot also passes. A final USB
upload acknowledgment timed out once and needs diagnosis. Open acceptance includes
remaining browser reference examples, slot retention across power cycles,
sustained BLE send-failure recovery, battery-only watchdog recovery and operator
checks for physical output, sensors, shutdown and persistence. See the owning
documents for precise procedures and current status; simulation does not close
these physical items.

## Dependencies and local state

Pybricks, pybricksdev and Zephyr are separate west repositories. Their local
corrections support current builds and checks; publication and release pins
remain pending under the [upstream workflow](../docs/upstream.md).
Inspect intentional dependency edits before updating or resetting a workspace.
Brickphyr-specific adaptations belong here, with licenses and provenance.

Everything tracked by Git is public. Keep session handoffs, captures, raw logs
and local device observations in ignored `reports/`; they are not public contracts
or continuing authorization to operate hardware.
