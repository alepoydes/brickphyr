# Compatibility

Preserving Pybricks' Python behavior is a design commitment. The optional Python
library currently provides parameters, geometry, non-device tools and initial
hub bindings. The target contract is broader than the implemented
coverage below.

## Reference and scope

The selected behavioral reference is Pybricks v4.1.0b6. Its source revision is
pinned by the [workspace manifest](../west.yml). Before implementing compatibility, pin the
selected Pybricks/MicroPython build and publish the covered API version. Changing
that baseline later is an explicit compatibility decision.

Compatibility includes signatures, argument defaults, units, coordinate
conventions, return values, exceptions, calibration behavior, motor stopping,
wait/cancel semantics, and relevant asynchronous behavior. Matching a module or
method name alone is insufficient. Preserve behavior using the reused Pybricks
implementation where practical, not independent reimplementations of each binding.

Compatibility covers supported, useful operation; it does not require reproducing
bugs, deadlocks, or a loss of system control. Document deliberate corrections and
test the resulting behavior rather than treating every upstream failure as a
contract to preserve.

The contract applies to the APIs available on the given hub and build. Hardware
absence and an unfinished port are different reasons for unavailability and must
be documented separately. Required compatibility deviations are explicit errors
or documented experimental differences, never plausible fake measurements.

## Multitasking and system responsiveness

Support Pybricks `async`, `await`, `multitask`, and `run_task`, including normal
completion, waiting, and cancellation. Zephyr services must continue independently
of a Python program yielding. Reusing these Python interfaces does not require
reusing PBIO's cooperative scheduling for the whole firmware or mapping every
coroutine to a kernel thread.

A non-yielding or faulty application must remain stoppable while management and
essential device services continue. If a coroutine cannot be terminated safely
in isolation, terminate its application and report the cause. Do not resume a
damaged runtime merely to preserve a connection. Verify this property separately
for Python, native, and WASM under the [application contract](applications.md).

## Existing host clients

Retain a Pybricks-compatible interface so existing Pybricks Code and host tools can
discover the hub, upload supported Python programs, start/stop them, and exchange
status and input/output. Python source compatibility alone does not provide this
client compatibility. Validate discovery, protocol versions, capabilities, program
packaging, and `.mpy` format against a specified client/runtime combination.

Once Bluetooth is implemented and enabled, an available hub must advertise for
host connection without requiring a user program. A connected client must see
only implemented capabilities; an identification reply is not proof that Python
execution or every built-in Pybricks tool works. Firmware installation and
restoration have their own [recovery contract](recovery.md).

The [host interface](host.md) defines session ownership and the proposed USB
selection experiment. Native/WASM delivery and management may use standard
mechanisms beyond the unchanged Pybricks client's capabilities.

The base [USB endpoint](host.md#implemented-pybricks-subset) implements
identification and explicit unsupported-command errors. The optional Python
profile adds experimental execution through the shared protocol handlers, with
ARM serial-client and synthetic BLE-adapter checks. Its
[persistent slots](host.md#stored-pybricks-programs) share the application store;
the hub example executes through both actual clients on the Inventor F4.
Power-cycle slot retention and complete client acceptance remain open. The optional
[BLE endpoint](bluetooth.md#experimental-f4-controller) exposes those same handlers.
Pybricks Code discovery, program upload, imports, input/output, stop, repeat and
reconnection also pass on the Inventor F4 through Chromium Web Bluetooth; see the
[tested browser and profile limitation](bluetooth.md#pybricks-code).
Pybricks Beta additionally passes REPL reset/interruption, asynchronous tools and
the unchanged reference status-light example. Browser native/Viper compilation
has a client limitation; matching pybricksdev builds and executes these modules
through both physical transports. Software-reboot slot retention passes, while
a sporadic final USB upload acknowledgment timeout remains unresolved.

## Initial coverage plan

The target comes from the selected
[F4 configuration](https://github.com/alepoydes/pybricks-micropython/blob/a7b1b04ae4f8449dd46b3dfb97c420334439fd35/bricks/primehub_f4/mpconfigport.h)
and its enabled bindings. This inventory includes services still missing from
Brickphyr; source availability is not implementation coverage.

| Reference surface | Validation to add with implementation |
| --- | --- |
| `PrimeHub` / `InventorHub`: buttons, light, display, speaker, battery, charger, system | Hardware ownership, animation, power, storage and stop-button behavior |
| Hub IMU | Units/axes, orientation, filter/sample timing, calibration and persistence |
| `parameters`, `tools`, geometry alias | Values, matrix math, clocks, input, COBS, hub menu, `run_task` / `multitask`, waiting and cancellation |
| `pupdevices`: motors and wired sensors/actuators | Mode selection, units, direction, gearing, limits, stall, stop modes and hotplug; includes light/matrix and infrared features |
| `robotics`: `DriveBase`, `SpikeBase`, `Car` | Coordinated motion, gyro use, quantization, completion and cleanup |
| `iodevices`: `PUPDevice`, `UARTDevice`, `LWP3Device`, controller bindings | Raw device access, ownership, bounded I/O, connection and disconnect behavior |
| Wireless Powered Up devices and `messaging` | Remote/hub device bindings, `AppData`, `BLERadio`, `HubNetwork` and legacy aliases; distinguish peripheral links from the management owner |
| Standard modules enabled by the reference | `io`, `math`, `micropython`, `struct`, `sys`, `errno`, `json`, `random`, `select`, `gc` and source/bytecode imports |
| Pybricks Code and pybricksdev | Capability/profile negotiation, multi-module packaging, program slots, execution and input/output |

Hardware binding coverage and complete client acceptance are pending. Core
interpreter checks do not establish Pybricks API compatibility. Expand the
coverage below with executable cases as bindings become available.
Test the same small programs on the reference firmware and Brickphyr, retaining
expected tolerances and genuine hardware differences.

Reuse the reference repository's
[virtual-hub cases](https://github.com/alepoydes/pybricks-micropython/tree/a7b1b04ae4f8449dd46b3dfb97c420334439fd35/tests/virtualhub)
for imports, color, tools, multitasking and motor/drive-base semantics, and its
[Powered Up cases](https://github.com/alepoydes/pybricks-micropython/tree/a7b1b04ae4f8449dd46b3dfb97c420334439fd35/tests/pup)
for sensor units, motor gearing and hub lighting. Add the runnable examples from
the [hub manual](https://docs.pybricks.com/en/latest/hubs/primehub.html) and
[robotics manual](https://docs.pybricks.com/en/latest/robotics.html) to client
acceptance, recording their version when captured. Online manuals can describe a
different beta; they do not silently change the pinned behavior. Physical sensor,
controller, sound/light and motion acceptance requires the corresponding device
or observation, separately from simulated execution.

## Implemented library subset

`CONFIG_BRICKPHYR_PYBRICKS` enables the experimental library within the Python
profile. Disabling it retains native Python scripts and REPL. The selected
Pybricks sources supply the bindings directly; this is not a parallel Python
implementation. The standard MicroPython importer supports both these builtin
subpackages and application modules.

| Surface | Current executable coverage |
| --- | --- |
| `parameters` | Axis, Button, Color, Direction, Icon, Port, Side and Stop; color values, arithmetic, hashing, GC and fresh application state |
| `tools` geometry and `geometry` alias | Matrix/vector arithmetic, transpose, cross product, norms, incompatible dimensions and GC |
| `StopWatch`, `wait` | Monotonic milliseconds, pause/reset/resume, blocking and asynchronous waits, zero-duration yields and interrupt delivery |
| `run_task`, `multitask` | Results, concurrent waits, racing, exception propagation, cancellation despite cleanup errors and forced-stop recovery |
| `hub_menu` | Reference selection loop over the shared buttons and matrix; wraparound, release before return and restoration of the prior stop-button combination |
| Input and COBS | Shared stdin, byte/character reads, and the reference framing example |
| `PrimeHub`, `InventorHub`, `ThisHub` | Shared hub type, read-only button state, battery chemistry and voltage/current/temperature snapshots; charger current/status |
| Hub lighting | `on`, `off`, `blink` and `animate` through the common button-light service; copied sequences and background execution without Python yielding |
| Hub display | `on`, `off`, `pixel`, `icon`, `char`, `number`, `orientation`, background `animate` and blocking/awaitable `text`; shared orientation and reference fonts |
| Hub speaker | `volume`, blocking/awaitable `beep` and lazy `play_notes`; common tone service, rests/articulation, cancellation, ownership and driver-error cleanup |
| Hub system | `name`, `info`, `reset_reason`, `set_stop_button`, `storage` and supervised `shutdown`; shared boot cause, launch identity, management connection and persistent user data |
| `PUPDevice`, `LUMPDevice` | `info`, blocking/awaitable `read` and `write`, asynchronous UART `reset`; shared raw modes, signed/float wire values, passive touch input and connection generation checks |
| `UARTDevice` | Shared custom UART, exact reads, copied writes, pattern waits, concurrent read/write, timeout/overflow and supervised release; `power_pin=1/2` remains unsupported |
| `ForceSensor`, `TiltSensor`, `InfraredSensor` | Blocking/awaitable native measurements with reference units, force calibration, tilt axis order and infrared count offset; generation-bound selection and cleanup |
| `ColorSensor`, `UltrasonicSensor`, `ColorDistanceSensor` | Native measurements, HSV/color matching, shared copied palettes and blocking/awaitable sensor lights |
| `Light`, `ColorLightMatrix` | Passive light brightness/off and UART 3×3 colors/off, native output encoding, connection-bound commands and port cleanup |
| `PFMotor` | Native IR command encoding through ColorDistanceSensor, blocking/awaitable duty/coast/brake, direction, generation checks and coast transmission during cleanup; no remote acknowledgement |
| `DCMotor` | Shared native direction/voltage limits, duty, coast, brake, settings and close; generation-bound output and supervised release |
| `Motor` | Shared direction/gears/origin/profile, angle/speed/load/stall, DC and regulated motion, stop modes, blocking/awaitable completion, replacement, cleanup, `control` settings/trajectory/status, `model` tuning/state and independent `log`/`control.log` CSV export; physical acceptance remains pending |

Checks execute through the protected ARM application path. Unchanged reference
color, multitasking and COBS programs also pass through the actual pybricksdev
serial client/compiler against QEMU. The [tools example](../examples/python/tools.py)
uses concurrent timers without accessing hardware. These results do not establish
physical USB/BLE delivery or clock accuracy. Hub binding checks use synthetic
button/ADC inputs and a TLC5955 SPI peer with the production driver. They cover
rendering, ownership conflicts, rejected input, I/O errors and cleanup on return,
exception, forced stop and REPL reset. This does not verify perceived colors,
display orientation or measurement accuracy on a physical hub. The
[hub example](../examples/python/hub.py) exercises display text, icons and background lighting.
It also compiles and runs repeatedly through the actual pybricksdev serial client
against the ARM fixture and through both USB/BLE clients on the Inventor F4.

Generic wired-device checks use a synthetic UART peer and the production port
service. They cover all four read formats, async reads, replacement without
silently rebinding an existing object, passive touch input and release on return.
Protected ARM write checks cover signed and float32 formats, payload copying,
forced stop and port reuse. These checks do not establish physical output or
third-party device compatibility. Reset checks cover transient electrical absence,
renegotiation, replacement after completion and release on stop. Synthetic
Force/Tilt/Infrared and Color/Ultrasonic/ColorDistance peers exercise cold
construction, native conversions and Python results. Color checks include
custom/shared palettes and captured UART light commands. Physical acceptance of
these classes remains pending. See
[raw device usage and ownership](ports.md#arbitrary-uart-modes).
Custom UART checks cover binary/chunked duplex, mutable input/GC, overlapping
patterns, timeout/overflow recovery and failed cleanup with a synthetic echo peer.
Physical UART acceptance and custom-supply support remain pending; see
[custom serial devices](ports.md#custom-serial-devices).
Accessory checks exercise real port PWM/UART services with synthetic peers:
brightness, matrix palette/brightness encoding, short-iterable rejection without
output, stale Light rejection and forced-stop coast. These are not physical
brightness or perceived-color measurements; see [external lights](ports.md#external-lights).
On the Inventor F4, `PUPDevice.info()`, reset and synchronous/async CALIB reads
from an idle ID-75 motor pass through USB/BLE application upload and execution,
with port release afterwards. No motor drive is part of this check; physical sensor,
touch-switch and third-party raw-mode acceptance remain pending.
Display cleanup and error-free driver completion are checked there; perceived
output and running that example through the browser remain separate acceptance items.

Speaker checks use synthetic DMA through the same protected API, including
ownership during rests, iterator errors, replacement and cleanup. The
[speaker example](../examples/python/speaker.py) also executes on the Inventor F4
through native USB/BLE management and both actual Pybricks clients. Interruption
and REPL reset release playback there. Acoustic quality remains unverified.

`hub.system.info()` reports the accepted USB/BLE management link, excluding work
still draining after disconnection. Pybricks program IDs identify the selected
slot; REPL uses 128, while named programs and temporary SMP uploads use -1. Current
starts are remote (3). Reset reasons use the reference values: 0 normal, 1 software,
2 watchdog. A target without reset-cause support omits that field from `info()`
and raises `OSError(ENOTSUP)` from `reset_reason()`.

`set_stop_button()` accepts a hub button, an iterable specifying a combination,
or `None` to disable the program stop button. A fresh press of the whole
combination raises catchable `SystemExit`; an uncaught one ends the script without
a traceback. Each new interpreter restores `Button.CENTER`. The connection menu
consumes its own button events. Long-center-press power-off and host-forced stop
remain independent. `shutdown()` requests the ordinary supervised cleanup and
power-off path and does not return. The [system example](../examples/python/system.py)
inspects launch information and temporarily selects a two-button combination.
System information, REPL identity/reset, and autonomous USB-to-BLE handover pass
on the Inventor F4. Both actual Pybricks clients execute the example with the
correct slot and connection fields. Stop-button combinations and Python-requested
shutdown have protected ARM checks; their physical acceptance remains pending.

`hub_menu(*symbols)` displays at least two entries, each a printable ASCII
character or a number. Left/right cycle through entries; center confirms after
release, returning the original selected object. It works without constructing
a hub first and uses the same matrix and orientation as `hub.display`.
The prior stop-button combination is temporarily disabled (except Bluetooth
alone), then restored on return or exception. Host interruption and forced stop
remain available. Call it before starting `run_task`; it is a blocking menu,
not an awaitable. See the [menu example](../examples/python/menu.py).
Navigation, argument errors, interruption and stop restoration have protected ARM
checks with synthetic buttons. On the Inventor F4, USB/BLE checks verify the
initial controller frame and release/blanking after USB interruption and BLE
forced stop. Physical selection and perceived output remain to be verified.

`hub.imu` reads the common IMU service: `acceleration`, `angular_velocity`,
`tilt`, `up`, `orientation`, `ready`, `stationary`, `heading`, `reset_heading`,
`rotation` and `settings`. Acceleration is in mm/s²
and angular velocity in degrees/s. Vector reads return a three-row `Matrix` or
a scalar projection onto the supplied `Axis`/three-element `Matrix`. The
`calibrated=False` option reads raw measurements; calibrated tilt and `up` use
the fused gravity direction. Default axes are +X front, +Y left and +Z top.
`PrimeHub(top_side=Axis.Z, front_side=Axis.X)` selects the application frame,
following the reference constructor's axis convention. Acceleration, angular
velocity, tilt and integrated rotation use that frame. `up` and `orientation`
retain physical hub coordinates, as do native telemetry observations.
The IMU object and application frame are shared by hub instances; constructing
another instance selects its axes and resets heading. Application exit, forced
stop and REPL soft reset restore the default frame independently of finalizers.

Headings are clockwise-positive degrees; `reset_heading(angle)` changes their
reference without changing physical orientation or accumulated axis rotations.
`rotation(axis, calibrated=False)` undoes gyro scale correction while retaining
bias correction. Integration restarts after missing data or calibration changes;
Brickphyr's `imu.epoch()` exposes this reference change. Compare angles only
within one epoch, checking it before and after a measurement sequence. A heading
reset alone does not change that epoch. This is not an absolute compass heading.
`settings()` reads or atomically changes the shared calibration in RAM using
Pybricks units and tuple order. Values have the integer resolution of the
[common settings](settings.md#imu-calibration); saving remains explicit.
Invalid or stale measurements raise `OSError`; readiness/stationary return false
while measurements are invalid. A build without IMU support raises
`OSError(ENOTSUP)` without preventing construction or other hub operations.
See the [IMU example](../examples/python/imu.py).
Protected ARM checks use actual acquisition and processing with synthetic I2C/GPIO
for selected frames, heading resets, integrated rotation, atomic calibration,
missing samples and cleanup on return/REPL reset. Six poses and numerical boundaries
have direct calculation checks; bias learning has acquisition-service checks.
On the Inventor F4, native USB/BLE scripts check frame selection, heading reset,
calibration access and epoch changes; REPL observations and soft reset pass.
Both actual Pybricks clients run the expanded example in bounded checks.
Python observations can report transient `EOVERFLOW` during acquisition gaps,
including idle operation and USB/BLE application transfers. Subsequent reads
recover, but sustained error-free combined operation and physical calibration
accuracy remain unverified.

Radio constructor arguments, charger source detection (`connected()`),
system `reset_storage` and messaging remain unfinished required work.
Charger `current()` and `status()` read the shared native snapshot and propagate
acquisition errors; this does not establish BC1.2 source classification.
Battery chemistry, voltage/current and temperature use the common observation
service; voltage/current and temperature retain independent acquisition errors.
The remaining device bindings are not yet exposed.
`hub.bluetooth_light` is a Brickphyr extension with the same color-light interface;
system connection-menu indication takes priority over an application color.
Light sequences have 1–256 entries; frame durations must be positive and fit a
signed 32-bit millisecond value. Empty sequences and invalid intervals fail before
replacing a running pattern. Blink durations alternate on/off, starting on; an
odd-length sequence starts on again when it repeats. `on` and `off` stop animation.
See the [shared service's memory and timing limits](hardware.md#application-button-lighting).
Portable fixes for builtin imports, cancellation and numeric validation remain local dependency
changes listed in [upstream integration](upstream.md#python-library-prerequisites).

## Boundaries that are not automatically compatible

- Python source compatibility does not guarantee `.mpy` bytecode compatibility or
  the native MicroPython dynruntime ABI. Both depend on the selected runtime build.
- The new native SDK is initially unstable and checked at load time. It is not
  the PBIO internal ABI or an unrestricted Zephyr export table.
- Compatible firmware flashing does not guarantee the old program-upload protocol
  can carry new native/WASM applications or larger images unchanged.
- Replacing the scheduler must preserve externally visible waits and cancellation;
  it need not reproduce Pybricks' cooperative internal implementation.
- Hub calibration, pairing data, and user programs are different persistence
  domains. Firmware replacement/restoration needs explicit policies for each.

## Extensions

Keep additional Brickphyr features distinguishable from Pybricks compatibility.
Advertise available engines and capabilities so tools can report unsupported
operations before starting them. Avoid modifying established Python meanings to
expose a new native feature. Extra input devices and transports should extend the
appropriate service, without forcing an unrelated API rewrite.

Extensions may expose additional hardware, such as the hub's Bluetooth-button
LED. Select names and capability reporting when implementing the feature; define
ownership relative to system connection indications. Do not change an existing
call's meaning to introduce the extension.

The reference Pybricks API and implementation remain the source of truth for
compatibility; Brickphyr documents coverage and intentional differences, rather
than copying the entire upstream manual.
