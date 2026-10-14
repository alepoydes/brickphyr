# External ports

The experimental F4 image identifies electrical connections on A–F with
`CONFIG_BRICKPHYR_PORTS`. Native applications use `bp_port_get()` with indices
0–5; the shared USB/BLE Shell and SMP monitor read the same cached state:

```text
observe port.A
observe port.F
```

```sh
uv run brickphyrdev monitor --source port.A --source port.B
uv run brickphyrdev monitor --ble Brickphyr --source port.A --log ports.jsonl
```

Current support distinguishes empty ports, passive device IDs and UART devices.
`CONFIG_BRICKPHYR_PORT_UART` adds LUMP negotiation and raw binary I/O, using
Zephyr UART drivers. Both options default on for F4. ID 14 is the electrical
UART signature; successful negotiation provides the device's actual ID.
Known sensors and motor encoders also provide converted measurements and explicit
mode selection.
Direct motor drive is available with `CONFIG_BRICKPHYR_MOTORS`; experimental
speed/position regulation adds `CONFIG_BRICKPHYR_MOTOR_CONTROL`. Reading state does not start I/O, select a mode or
claim a port.

The build-only [Technic native profile](builds.md#experimental-technic-base)
reuses the electrical detector on A–D. E/F are absent and reads return `-ENODEV`;
indices outside the public A–F range remain `-EINVAL`. This profile supports direct
passive-motor output; UART negotiation and regulated motors remain unavailable.
`port A` explicitly reports electrical-only device identification.
Its sensor supply starts after the shared service releases all probe and motor
pins, and shutdown removes the supply and releases those pins again. Synthetic
four-port checks do not establish physical Technic behavior.

Private state follows the board's highest enabled port index while public SDK
numbering remains fixed. Sensor samples, palettes, write buffers, resynchronization,
custom serial and remote PF output state are compiled into a port only when UART
operations are enabled. Public snapshot structures retain the same layout across
profiles; sensor ownership is read from the shared port claim.

## Device inspection

The same Shell command works through the USB and BLE terminals:

```text
port
port A
```

`port` lists A–F. `port A` displays that connection's cached information without
requiring a model-specific driver:

- Port, connection state, electrical and negotiated device IDs, and recognition
  status. A reported ID identifies a protocol type, not a verified manufacturer.
- Advertised hardware/firmware versions, capabilities and communication rate.
- Every advertised mode: index, name, input/output mapping, data type and value
  count, units, scaling ranges and display format, where supplied by the device.
- The current mode and latest values decoded by its negotiated binary format,
  alongside raw bytes, timestamp, validity and connection/protocol errors.

Protocol INFO fields 0x07–0x0c are shown as unchanged hexadecimal payloads,
including wire padding; their meaning is not inferred. Undefined INFO types
outside the protocol's list are counted, but their payloads are not retained.
Device-supplied strings are escaped before terminal output. Versions retain
their advertised BCD words, rather than assuming a vendor-specific version scheme.

Missing metadata remains explicitly unavailable. Unknown models retain their
reported identity, modes and raw data; they do not acquire guessed units or the
semantics of a similarly named LEGO sensor. Passive devices show only the
information their electrical interface provides. Inspection does not select
modes, claim a port or actuate a device. Explicit mode selection uses the
existing port ownership and validation rules.

Wire values are signed integers or IEEE-754 float32, without applying the
advertised scaling or decimal precision. Ranges are device claims, including
non-finite or reversed values if supplied; inspection does not use them to
convert measurements. Use `observe sensor.A` for supported, converted quantities.

Native applications use `bp_port_get_mode(port, generation, mode, &info)` with
the generation from `bp_port_get()`. It copies one mode into caller-owned memory;
`-ESTALE` means the connection changed, `-EAGAIN` means negotiation is incomplete,
and `-EINVAL` rejects an out-of-range mode. Metadata can be read before the first
DATA sample. Optional fields require their presence bit or payload length.
The terminal checks generation while reading modes and after printing the sample;
if the connection changed, repeat inspection. Neither reader locks the device
for the duration of the display.

The UART parser owns the only metadata table: at most 16 modes per port, with
bounded strings and payloads. It allocates the advertised number of modes from
the shared system heap during negotiation. Combination masks and uninterpreted
INFO payloads use separate packed storage per mode, grown only for bytes actually
advertised; the getter expands them into the unchanged public structure.
The UART contribution to the minimum heap budget follows enabled devicetree ports:
5 KiB per port for complete mode tables, plus 2 KiB for allocator and growth
overhead. This is 32 KiB for six ports and 22 KiB for four; it does not reserve
that memory separately from other heap users.
Disconnect, restart, failure and shutdown release both allocations. Internal baud changes retain the table. Publication
occurs after negotiation, and reset invalidates readers before freeing storage.
An allocation failure reports `-ENOMEM` and fails that negotiation; it does not
publish incomplete metadata. No allocation occurs for DATA packets or getters,
and getters perform no device I/O.
Shell range/float32 output enables Zephyr's floating-point formatter.

Synthetic checks cover recognized and unknown UART peers, all four wire formats,
optional and malformed metadata, four/six maximum-size tables and their reclamation,
out-of-order INFO, allocation failure, maximum mode/payload sizes, reconnects and unchanged ownership/outputs. USB Shell inspection on one Inventor F4 displays
the modes, formats, ranges and current data of original LEGO ID-61 and ID-62
sensors. Physical third-party compatibility and BLE Shell inspection remain
unverified. First inspect what a device advertises; a matching ID alone does not
establish compatibility.

## Custom serial devices

`pybricks.iodevices.UARTDevice(port, baudrate=115200, timeout=None, power_pin=0)`
claims a coasting port for ordinary 8N1 UART through the existing Zephyr driver.
LEGO detection and protocol traffic stop until the application releases the port.
`port A` then reports `serial`, its baud rate and any error, without inventing a
LEGO device identity. A second UART object cannot take over the live claim.

The binding provides blocking/awaitable `read(length=1)`, `write(data)` and
`wait_until(pattern)`, plus immediate `waiting()` and `read_all()`. Reads return
exactly the requested bytes; `timeout` is a positive whole-operation timeout in
milliseconds, or `None` for an interruptible indefinite wait. `wait_until` has no
timeout, consumes through the matched pattern, and leaves subsequent bytes for
reading. Read and write can run concurrently through `multitask`; replacing an
operation exhausts the previous awaitable in that direction. Async writes retain
immutable data, copying bytearrays before yielding; temporary patterns remain
rooted through garbage collection.

`clear()` cancels both directions and flushes the receive buffer.
`set_baudrate()` does likewise while configuring a positive baud supported by the
driver. Receive overflow reports `OSError(errno.ENOBUFS)`; clearing or explicit
reconfiguration starts a new exchange. The 512-byte RX ring is shared with the
existing UART implementation. Write completion includes the final stop bit,
without acknowledging reception by the external device. Physical baud accuracy
and custom-device wiring remain unverified. Ordinary UART cannot identify an
unplugged/replaced peer; connection generations identify the application's lease.

`power_pin=0` leaves P1/P2 unpowered. Values 1/2 apply battery voltage with the
selected pin positive and the other low. As in Pybricks, the first powered
constructor asks for `Y` before claiming the port or enabling power; any other
answer refuses with `ENOTSUP`. This acknowledges custom wiring; it does not
identify the connected device or establish its voltage tolerance. Permission
uses the shared [`uart/power_allowed` setting](settings.md#custom-uart-power).
It remains in RAM across application exits; only `config save` persists it.

Clearing buffers and changing baud retain power continuously. UART/driver errors,
application return, forced stop and shutdown attempt to coast both power pins.
A failed constructor releases its claim even when the program catches its
exception. Driver failures remain explicit; cleanup failure retains the claim
and reports that electrical state is uncertain.

Native applications use `bp_uart_open/configure/clear/waiting/read/write` with
bounded copied buffers and `bp_port_get()` for asynchronous configuration/write
completion. `bp_uart_open_power` adds the power-pin selection and returns `EACCES`
before side effects if permission is missing. `bp_uart_allow_power(0|1)` applies
that permission in RAM through the same validator as Shell/Settings. No
application pointer survives a syscall. Return, forced stop and
REPL reset use the common port release, stopping UART IRQ access and coasting
outputs before restoring LEGO detection. Cleanup errors retain the claim and are
reported by the supervisor; ordinary host disconnection leaves the application
and its port running.

Protected ARM checks cover both power polarities, refusal/acknowledgement,
continuous power across UART reconfiguration, failed-constructor release and
supervised cleanup using synthetic GPIO/UART. Permission save/reload uses the
actual LittleFS/Settings stack in the native simulator. Physical UART signals,
custom wiring and power continuity remain unverified.

## Arbitrary UART modes

`bp_port_select(port, generation, mode)` claims an idle UART port and queues an
advertised mode through the same selector used by normalized measurements.
Invalid modes, stale connections, competing owners and active or unconfirmed
motor outputs fail before selection. This operation sends SELECT only; it does
not write device data or command motor motion.

Read `bp_port_get()` until `error` and `mode_error` are zero, checking the same
`generation` and matching `selected_mode` and `mode`. Selection invalidates the
previous observation immediately. Matching DATA must satisfy the existing
settling and freshness rules. The returned bytes retain their advertised wire
format and are not scaled. Normalized sensor observations report `-ENOTSUP`
while raw selection owns the mode. Release restores automatic measurement;
`selected_mode=UINT8_MAX` means there is no explicit raw selection.

`bp_port_write(port, generation, mode, data, size, &token)` selects an advertised
writable mode and queues its output DATA. The mode must have a nonzero output
mapping; `size` must equal its FORMAT value count times the wire type width
(1–32 logical bytes). The service copies the payload before returning and allows
one pending write per port. Rejected modes, sizes, stale generations and owner
conflicts have no write side effects. Match `token` against
`bp_port_get().write.request` and `.completed` with the same connection generation;
`write.error` is `-EAGAIN` while pending. Completion follows UART transmission
and the device-specific post-write delay, not an acknowledgement from the device.
Release cancels unsent DATA; once transmission starts, it retains ownership
until the bytes and settling interval finish.

`bp_port_reset(port, generation, &token)` claims an idle, coasting UART port and
requests fresh electrical detection and UART negotiation. It invalidates the
old generation immediately; it does not guarantee a device power cycle. Match
the returned token against `bp_port_get().reset.request` and `.completed`.
`reset.error` is `-EAGAIN` while pending; successful completion records the exact
new generation in `reset.generation`. A later reconnection cannot change that
result. Negotiation errors, a different device type or release terminate the
request explicitly. Pending output, unreleased PF outputs or active motor control
prevent reset.
Electrical detection can temporarily report an empty/passive port while the
device leaves its old UART session. Reset retains ownership and waits for the
new handshake within the existing detection/negotiation deadline. The protocol
cannot distinguish a same-type replacement during this explicit reset.

The Python binding provides `pybricks.iodevices.PUPDevice` (`LUMPDevice` alias):

```python
from pybricks.iodevices import PUPDevice
from pybricks.parameters import Port

device = PUPDevice(Port.C)  # Choose the connected device's port.
print(device.info())
print(device.read(0))      # Select an advertised mode appropriate to that device.
```

`info()` returns the device ID and advertised mode names, value counts and data
types. `read(mode)` returns a tuple of signed integers or float32 values; it is
awaitable inside `run_task`. `write(mode, data)` accepts a tuple or list with
exactly the advertised value count, converts signed int8/int16/int32 or float32
values, and validates them before queuing; it blocks or can be awaited inside
`run_task`. Passive touch switches return a boolean and have no UART modes.
Objects retain their original connection generation: create a new object after
unplugging/replacing a device. Program cleanup releases the port; disconnecting
the host alone does not. `reset()` returns `None` after requesting negotiation;
`info`, `read` and `write` report `OSError(EAGAIN)` until it completes. Only the
object that requested a successful reset accepts its resulting connection.
Other objects remain invalid, as does the requesting object after an
unplug/reconnect following reset completion. Reset is unavailable for passive
devices.

## Sensor measurements

`bp_sensor_get()` and `observe sensor.A` through `sensor.F` read the same latest
measurement. The service selects a default measurement when a recognized sensor
connects, independently of a host or application. The monitor can record it:

```sh
uv run brickphyrdev monitor --source sensor.A --log sensor.jsonl
uv run brickphyrdev monitor --ble Brickphyr --source sensor.A
```

| Sensor (device ID) | Default measurement | Other measurements |
| --- | --- | --- |
| SPIKE color (61) | `surface`: reflection and HSV | `ambient`: intensity and HSV |
| SPIKE ultrasonic (62) | `distance`: millimetres | `presence`: another ultrasonic source detected |
| SPIKE force (63) | `force`: millinewtons and displacement in micrometres | None |
| BOOST color/distance (37) | `surface`: reflection and HSV | `ambient`: intensity; `distance`: relative estimate |
| WeDo 2.0 tilt (34) | `tilt`: two angles in degrees, in Pybricks order | None |
| WeDo 2.0 infrared (35) | `distance`: relative estimate and reflection | `surface`: same measurement; `count`: sensor counter |
| BOOST Interactive Motor (38) | `angle`: relative encoder | None |
| Technic L/XL (46/47), SPIKE M/L/S (48/49/65), Technic angular M/L (75/76) | `angle`: absolute encoder and counted turns | None |

`reflection_permille` and `ambient_permille` use 1000 for nominal full scale;
HSV uses degrees and percent. `distance_mm=2000` means no ultrasonic return below
2 m. Infrared `distance_pct` is a relative estimate, not a length. The WeDo
conversion is preserved from Pybricks and can exceed nominal percentage limits;
applications must not interpret it as calibrated ranging. The signed counter
has no application-specific zero offset. Infrared transmission and other device
APIs beyond the subset below remain pending.

The Python `pybricks.pupdevices` sensor classes are constructed with `Port.A`
through `Port.F`. Their blocking or awaitable reads use these same native
measurements:

| Class | Methods and units |
| --- | --- |
| `ForceSensor` | `force()` in newtons, `distance()` in millimetres, `touched()` and `pressed(force=3)` booleans |
| `TiltSensor` | `tilt()` returns two angles in degrees, in Pybricks order |
| `InfraredSensor` | `distance()` is relative percent, `reflection()` is percent, `count()` is relative to construction |
| `ColorSensor` | `reflection()` and `ambient()` in percent; `hsv(surface=True)`, `color(surface=True)`, `detectable_colors()` and three-channel `lights.on/off` |
| `UltrasonicSensor` | `distance()` in millimetres, `presence()` boolean and four-channel `lights.on/off` |
| `ColorDistanceSensor` | Relative-percent `distance()`, percent `reflection()` and `ambient()`, `hsv()`, `color()`, `detectable_colors()` and `light.on/off` |

Color sensors share a copied palette per port. The default is red, yellow, green,
blue, white and `Color.NONE`; each new Python color-sensor object restores it.
`detectable_colors(colors)` replaces it with up to eight `Color` values; an empty
palette makes `color()` return `None`. `detectable_colors()` returns a tuple.
Native callers use `bp_sensor_palette_get/set(port, generation, ...)` with packed
HSV values defined in the SDK; setting `NULL` restores defaults. Replacement
validates the whole palette before claiming the port or changing its colors.
It survives release and hotplug until replaced or firmware restarts. The
unchanged PBIO matcher chooses the nearest entry; `bp_sensor_get()` returns it
with `BP_SENSOR_HAS_MATCHED_COLOR`. Telemetry exposes the matched hue, saturation
and signed value alongside the measured HSV. Reads do not claim the port.

SPIKE `lights.on(brightness=100)` accepts one percentage or a tuple/list with
one per light, clipped to 0–100 as in Pybricks. `off()` sets all to zero.
BOOST `light.on(color)` chooses red, green, blue or off using the reference hue,
saturation and brightness rules; `light.off()` switches it off. These calls
block or can be awaited inside `run_task`. Native `bp_sensor_light_write()`
accepts validated percentages or packed HSV and queues the same UART write.
Its token uses the [ordinary write completion](#arbitrary-uart-modes) contract.
The next measurement selects its required mode; cleanup restores the automatic
measurement rather than promising independent persistent light settings.

Selection uses `bp_sensor_select(port, generation, measurement)` so a stale
object cannot change the mode of a replacement device. The existing native
`bp_sensor_configure` selects on the current connection without that generation
constraint. Protected ARM checks use synthetic UART devices; these Python sensor
classes still require physical acceptance. Wired motor bindings remain pending.

Encoders report `angle_mdeg` in thousandths of a degree, in the device's direction
without gear ratios or an application zero offset. BOOST uses its signed POS
counter, extended through 32-bit rollover. The other listed motors use the second
CALIB word, as Pybricks does, and additionally report `absolute_mdeg` in
`[-180000, 180000)`. Their initial accumulated angle equals that absolute angle;
subsequent samples add the shortest signed change. Counting turns requires less
than half a revolution between accepted samples. Missed complete revolutions
cannot be recovered from an absolute sample, even if the UART timeout has not
expired. Exactly half a revolution is ambiguous and resolves in the negative
direction. Reconnection resets the count and changes `generation`; releasing a
measurement claim does not reset it. Reads and SELECT never drive, brake or hold
a motor. Passive motors without encoders have identity observations only.

To change a measurement in the shared USB/BLE Shell:

```text
sensor A ambient
observe sensor.A
sensor A release
```

`bp_sensor_configure(port, measurement)` provides the same operation to native
applications. It validates availability before claiming the sensor or queuing a
command. A successful return means acceptance; `sample.error=-EAGAIN` remains
until the requested measurement is ready. Calls copy bounded state without
allocation, UART access or waiting for the sensor. `bp_sensor_release()` restores
the default. Owner values are 0 free, 1 application and 2 terminal; competing
configuration returns `-EBUSY`. Read-only observers never take ownership.
Application exit releases its claims; management disconnect releases terminal
claims while preserving application claims. Hotplug clears both ownership and
calibration. Release is asynchronous and does not wait for the default sample.

Check `error` and the `fields` mask before using values. `available` is a bitmask
of supported `bp_sensor_measurement` values, including `default`; `measurement`
identifies the resolved measurement and `mode` the wire mode of the last accepted
sample. `generation` matches the port's connection state; `sequence` and
`sample_ms` advance only on accepted measurements. A failed conversion preserves
their previous values but clears `fields`. Getters never make an old sample fresh.

The service checks the negotiated format before sending SELECT. Color data must
settle for 30 ms and ultrasonic data for 50 ms after the first matching frame;
these delays come from Pybricks, not Brickphyr measurements. The force sensor
first supplies calibration, which must have a positive displacement span, then
switches to raw force. Missing SELECT confirmation times out after 500 ms;
subsequently 300 ms without the selected mode invalidates the connection even
when other DATA keeps arriving. Queued mode changes invalidate old observations
immediately and cannot publish a previous selection as the new one.

Modes, calibration and numerical conversions in `src/ports/sensor.c` are a
selected MIT-licensed port of
[`port_lump/sensors.c`](https://github.com/alepoydes/pybricks-micropython/blob/a7b1b04ae4f8449dd46b3dfb97c420334439fd35/lib/pbio/src/port_lump/sensors.c)
and the corresponding `pybricks/pupdevices` bindings at that revision. The pinned
PBIO RGB/HSV converter and color matcher are compiled unchanged. Brickphyr adds
explicit units, format validation, rejected malformed calibration and RGB saturation before
narrowing, preventing unusually bright BOOST input from wrapping. Acquisition,
ownership and scheduling use the existing Zephyr port service.
Encoder adaptation validates the known ID, negotiated format and absolute-angle
capability. It extends the BOOST counter without signed overflow, rejects invalid
absolute values, and uses shortest-angle increments instead of PBIO's quadrant
crossing test. These are measurement semantics, not a motor controller.

### Sensor hardware checks

On one Robot Inventor F4 (PCB revision unknown), the full Python profile reads
an original LEGO SPIKE color sensor (ID 61, port C) and ultrasonic sensor
(ID 62, port D). Both negotiate at 115200 baud and produce default measurements
without an application selecting their modes. USB Shell exposes their advertised
metadata and raw packets.

RAM-only Python programs pass through native USB/BLE management with synchronous
and awaitable reflection, ambient, surface/ambient HSV, matched color, distance
and presence reads. Sensor light writes and subsequent measurement-mode recovery
also complete. Application cleanup returns both sensors to unclaimed default
measurements; connection generations and protocol error counters remain unchanged.
The connected motors remain unclaimed and coasting throughout these checks.

The USB monitor also captures reflection/white classification when white paper
is presented and removed, and distance changes when a hand approaches and leaves.
The bounded 2 Hz recording completes without observation errors or missed polls.

These checks establish communication, mode switching and binding operation.
Calibrated color/distance accuracy, positive detection of another ultrasonic
source, perceived light output, other sensor models and third-party compatibility
remain unverified.

## External lights

`pybricks.pupdevices.Light(Port.A)` supports the passive Powered Up 88005 light
(ID 8). Construction switches it off. `on(brightness=100)` takes a percentage
clipped to 0–100; `off()` and zero brightness coast the output. Calls return
`None` after driver completion. The existing PWM port service owns the output;
application completion, forced stop and hotplug use its ordinary coast cleanup.
An old object cannot switch on a replacement device after reconnection.

Native `bp_port_light_write(port, generation, brightness, &request)` accepts
0–1000 permille, with zero for off. It validates the exact passive device and
connection before queuing. Match the returned request, completion and generation
in `bp_motor_get()`; `observe motor.A` also reports this shared output state.
This light entry cannot drive a motor, and ordinary motor commands continue to
reject lights. The same port ownership rules apply to both.

`ColorLightMatrix(Port.B)` supports the UART Technic 3×3 matrix (ID 64).
`on(colors)` takes one `Color` for all cells, or the first nine colors from an
iterable in the device's cell order. A shorter iterable or invalid color fails
before output. `off()` sends a blank frame. Both methods block or can be awaited
inside `run_task`. The reference conversion rounds hues to the device palette
and quantizes brightness to ten levels, including its lowest-gray correction.

Native `bp_port_matrix_write(port, generation, colors, &request)` copies nine
packed HSV colors; `NULL` sends a blank. It uses the same UART writer and
completion token as generic mode writes. Releasing the port drains/cancels writes
and relinquishes ownership; it does not send a blank frame automatically. Call
`off()` when the image should disappear. Normal host disconnection leaves an
autonomous application's outputs unchanged.

The conversion is a selected MIT-licensed port of
`pybricks/util_pb/pb_conversions.c` and
`pybricks/pupdevices/pb_type_pupdevices_colorlightmatrix.c` at the revision linked
above. Python argument handling comes from the same revision's Light and
ColorLightMatrix classes. No second PWM driver or UART implementation is linked.
Protected ARM checks cover PWM levels, stale-object rejection, forced-stop coast,
matrix output packets and port release using synthetic devices. Physical light
and matrix output remain unverified.

## Power Functions infrared motors

`PFMotor` sends single-output IR commands through a BOOST `ColorDistanceSensor`
(ID 37). Construction names a receiver channel (1–4), red or blue output, and
optional positive direction; it does not send a motor command.

```python
from pybricks.pupdevices import ColorDistanceSensor, PFMotor
from pybricks.parameters import Port, Color, Direction
from pybricks.tools import wait

transmitter = ColorDistanceSensor(Port.C)  # Choose its actual port.
motor = PFMotor(transmitter, 1, Color.RED, Direction.CLOCKWISE)
motor.dc(45)
wait(500)
motor.stop()
```

`dc(duty)` clips signed percentages to −100…100 and uses the reference seven
speed steps: magnitudes below 15 coast, 15–99 truncate to multiples of 15, and
100 selects step 7. `stop()` coasts; `brake()` brakes. These calls block or can
be awaited inside `run_task`. They share the sensor's existing UART writer and
awaitable; measurements and IR commands cannot overlap on that sensor. A later
measurement selects its ordinary sensing mode again.

Native `bp_port_pf_write(port, generation, &command, &request)` copies a
`bp_pf_command`: channel 1–4, `blue` 0/1, duty −1000…1000 permille and `brake`
0/1. Braking requires zero duty. The native service owns quantization and wire
encoding; completion uses `bp_port_get().write`, after transmission and the
reference 250 ms settling interval. No receiver acknowledgement exists, so
successful completion does **not** confirm physical motion or stopping.

The service remembers each addressed channel/output until release. Application
return, forced stop and REPL reset cancel/drain the pending write, then send
coast to those outputs using the same queue. Eight outputs require at least
2 seconds of settling, plus mode selection and transmission. Ports proceed
independently, retaining transmitter power while their commands finish.
Normal host disconnection leaves an autonomous program running.

A failed transmission retains the claim and reports a cleanup error. A later
cleanup attempt can use a reconnected ColorDistanceSensor on the same port to
retry only the remembered coast commands; old device objects still reject the
new connection. The application's first cleanup error remains visible even if
retry succeeds. Generic raw UART writes do not register remote outputs for this
typed cleanup. IR range, alignment and receiver power remain outside the hub's
observations; this mechanism cannot guarantee a remote motor has stopped.

The MIT-licensed encoding and Python argument semantics are ported from
`pybricks/pupdevices/pb_type_pupdevices_pfmotor.c` at the pinned revision linked
above. Protected ARM checks use a synthetic transmitter for command bytes,
spacing, sensor-mode reuse, eight-output release, unplug errors, stale objects
and coast-only recovery. Physical PF receiver/motor acceptance remains pending.

## Direct motor drive

`CONFIG_BRICKPHYR_MOTORS` provides duty, brake and coast for passive medium/train
motors (IDs 1/2) and the recognized encoders above. An encoder motor must have a
valid negotiated angle sample before accepting drive. Other devices and invalid
commands are rejected before acquiring a port or changing its outputs.

The shared USB/BLE Shell accepts these commands. They actuate the motor on A:

```text
motor A duty 200 100
observe motor.A
motor A coast
motor A release
```

Duty is signed permille, from -1000 to 1000, without gear ratios or direction
remapping. An optional duration in milliseconds starts when the driver applies
the command and ends in coast; omitted or zero duration continues until stopped.
`motor A brake [MS]` is zero-duty braking. It is not position hold. The native
equivalents are `bp_motor_drive(port, duty, duration_ms)`, `bp_motor_coast(port)`,
`bp_port_release(port)` and `bp_motor_get(port, &state)`.

Motor drive and sensor-mode selection share one port owner: free (0), application
(1) or terminal (2). Accepted commands copy into firmware storage and return
without GPIO/PWM I/O or a wait. One pending drive is allowed per port; another
returns `-EBUSY`. Coast/release can replace a pending drive. The existing port
worker applies commands and checks identity again. No application-held mutex or
application buffer is used by the output driver.

Coast and duration expiry retain the claim. Release restores automatic sensor
selection and frees ownership only after coast succeeds. Application return,
fault or explicit stop joins that cleanup before unload; a failed coast retains
the claim and reports a cleanup error, with an explicit stop available to retry.
Management disconnect releases only terminal claims. Autonomous application
motors continue running when the computer disconnects. Hotplug, UART loss and
invalid encoder measurements cause coast; reconnect never restarts old commands.

`observe motor.A` through `motor.F` and the existing monitor report the same
state. `requested_ms`, `request`, `requested_mode` and `requested_duty` describe
the latest accepted command; `completed`, `applied_ms`, `mode` and
`duty_permille` describe its execution. Mode is 0 coast, 1 duty, 2 regulated speed
or 3 regulated position. `pending`
includes in-progress output I/O; `releasing` blocks new drive until cleanup.
`valid` means the driver confirmed the output configuration, not measured voltage
or motion. `error` preserves the first operation failure, `cleanup` reports a
separate coast failure, and `failures` saturates. A new accepted command clears
previous errors except while release is in progress. Unknown output state also
inhibits IMU bias learning, as does nonzero drive or active regulation, including
position hold; a start/stop between IMU frames
resets the stationary window.

Durations use the port worker's monotonic clock and are not hard real-time stop
guarantees. GPIO settling intervals remain at least 2 ms; waking the worker for a
command does not shorten them. Scheduler starvation or a stalled driver can delay
command completion and timeout handling. Waveforms, stopping latency and operation
under physical hub load remain unverified. See [paired drive control](#paired-drive-control)
for the shared native and Python DriveBase service.

On one Robot Inventor F4 (PCB revision unknown), an unidentified third-party
passive motor electrically identified as ID 1 passes a bounded USB Shell check at duties
of +350 and -350 permille for 450 ms each. The operator observed rotation in both
directions and subsequent stopping; driver snapshots confirmed timed coast and
release without errors. This device provides no UART encoder feedback. The check
does not establish speed, torque, loaded operation, braking or compatibility with
other motors sold under the same description.

## Python DC motors

`pybricks.pupdevices.DCMotor` and its `pybricks.iodevices` alias use the same
port worker and output driver. Construction accepts `port` and optional
`positive_direction=Direction.CLOCKWISE`, confirms coast, and selects the motor
model's voltage limit: 6000 mV for SPIKE Small, 9000 mV otherwise.

```python
from pybricks.pupdevices import DCMotor
from pybricks.parameters import Port, Direction

motor = DCMotor(Port.A, Direction.COUNTERCLOCKWISE)
motor.settings(max_voltage=3000)
motor.dc(25)
motor.brake()
motor.stop()
motor.close()
```

This example actuates port A. `dc(duty)` clamps percentage to -100..100 and
converts it using the shared battery observation, configured direction and
voltage limit. Integer conversion to native permille quantizes the result.
`brake()` applies zero-duty braking; `stop()` coasts. Both retain ownership and
work without a battery observation. These methods return `None` after driver
completion, without waiting for mechanical motion.

`settings()` returns `(max_voltage,)` in mV. Its setter accepts 0 through the
model's maximum; it changes subsequent DC commands, not an already applied
output. Configuration is shared by objects on that physical connection, stays
in RAM and resets on setup/release. The existing raw native duty API retains its
physical permille semantics. Its connection-bound counterpart uses
`bp_motor_setup`, `bp_motor_dc` and `bp_motor_set_voltage`; inspect their request
completion through `bp_motor_get`.

`close()` coasts and releases the port; repeated close is harmless. Commands on
a closed object or an object from a disconnected device raise `ENODEV`. A stale
close cannot release a replacement device. Release errors preserve ownership for
retry and supervised cleanup. Protected ARM checks cover passive-motor PWM,
voltage limiting, hotplug and cleanup with synthetic I/O. Bounded F4 checks with
an encoded motor also cover `DCMotor`; see [physical coverage](#python-motor-hardware-checks).

## Python encoded motors

`pybricks.pupdevices.Motor` uses the same native port owner, observer and PBIO
controller. Construction accepts `port`, `positive_direction`, `gears`,
`reset_angle` and `profile`. Gear trains may be a list such as `[12, 36]` or
compounded lists such as `[[12, 36], [20, 40]]`; teeth counts must be positive
integers. The resulting scale is shaft millidegrees per output degree.
Construction coasts, initializes the model and sets the model voltage limit.
The optional positive precision profile adjusts position tolerance and gains
using the reference motor defaults; zero selects the default.

```python
from pybricks.pupdevices import Motor
from pybricks.parameters import Port, Stop

motor = Motor(Port.A, gears=[12, 36])
motor.run_angle(120, 90, then=Stop.HOLD)
print(motor.angle(), motor.speed(), motor.load())
motor.stop()
motor.close()
```

This example actuates port A. `angle()` rounds to output degrees; `speed(window=100)`
returns output degrees per second using the shared numerical derivative window.
`load()` is signed estimated shaft torque in mNm, not multiplied by the gear
ratio. `stalled()` uses the active controller or the passive observer as appropriate.
No Python control loop or second set of motor drivers runs.

`reset_angle(angle)` sets the shared output coordinate. Omitting the argument
uses the absolute shaft index, adjusted for direction and gearing; a motor without
an absolute index rejects that operation. Constructor `reset_angle=True` instead
uses zero when no index exists. `reset_angle=False` preserves the existing origin
on the same physical connection. Reset clears speed/model history, retains the
control settings, and stops active motion with its completion behavior. Direct
DC output is retained. Old model frames cannot be read with a newly reset origin.

`run`, `hold` and `track_target` replace the current motion and return after command
application. `run_time`, `run_angle` and `run_target` block for completion, or return
an awaitable inside `run_task`; `wait=False` starts without waiting for completion.
`run_until_stalled` has the same blocking/awaitable behavior and returns the final
angle. It accepts an optional percentage duty limit for that motion. Coast, brake,
hold, continue (`Stop.NONE`) and smart coast/brake reuse the reference controller;
continue is invalid for a stall objective. Smart stops apply their ordinary
coast/brake behavior for timed and stall moves, which have no relative endpoint
to preserve. Zero speed completes angle/target moves
in place. Nonpositive `run_time` duration completes immediately. Finite durations
must be below 300000 ms, the PBIO forever sentinel. `done()` describes the control
objective: an indefinitely running `run()` is considered done, as in Pybricks.

Direction, gearing, origin and voltage limits belong to the shared port, so aliases
see the same configuration. A new motion supersedes an earlier wait. Closing an
old awaitable cannot stop a newer motion, including one started through an alias.
Explicit `Motor.close()` releases the port for all aliases. `dc`, `brake`, `stop`
and `settings` reuse the DC implementation; during regulation, voltage-limit
changes take effect on the next controller frame. Application stop/fault cleanup
uses the existing common release path.

`motor.control` provides `limits(speed, acceleration, torque)`, `pid(kp, ki, kd,
integral_deadzone, integral_rate)`, `target_tolerances(speed, position)` and
`stall_tolerances(speed, time)`. With no arguments each returns its current tuple;
`None` preserves an omitted parameter. Acceleration accepts one value or an
(acceleration, deceleration) pair. Speeds, angles and accelerations use output
shaft units; torque uses mNm and time uses ms. PID gains retain PBIO's shaft
units. The read-only `scale` reports shaft millidegrees per output degree.

Settings belong to the port, so all aliases see changes. Each setter validates
its complete update before publication and waits for the next controller frame
to apply it. A failed update changes nothing. Updating settings preserves the
active movement: gains and tolerances affect its following steps; trajectory
limits and the torque cap are sampled when the next movement starts, as in
Pybricks. A motion's temporary stall torque cap is retained. Stall tolerances
configure regulated motion; passive DC stall estimation uses the observer's
model settings. Settings survive `reset_angle`; a new constructor restores
defaults. These controller settings are runtime state, not persistent hub settings.

`control.trajectory()` returns `None` when inactive, otherwise the reference
13-item phase tuple: four times, four relative angles, three speeds and two
accelerations. Times are in ms. The other fields use shaft degrees and seconds,
with the configured positive direction but without gearing. Legacy
`control.done/load/stalled` expose controller status; `Motor.load/stalled` also
cover passive observation.

`motor.model.settings()` returns the observer's eight raw settings: stall speed
(shaft mdeg/s), stall time (100 µs ticks), negligible feedback voltage (mV), stall
voltage ratio (%), low/high feedback gains (mV/degree), gain threshold (mdeg),
and friction cutoff (mdeg/s). Pass a complete eight-value sequence to change them
atomically at the next frame. Invalid arithmetic ranges are rejected. The same
sharing and reset rules apply as for control settings. `model.state()` returns
estimated shaft angle (degrees), shaft speed (degrees/s), current (mA) and stall
state. Angle, speed and current follow configured direction; angle follows its
origin, without output gearing.

`motor.log.start(duration, down_sample=1)` allocates a bounded RAM log for
`duration // 5 // max(down_sample, 1)` rows. Each row takes 44 bytes. Collection
runs in the native 5 ms worker and stops when full; `stop()` freezes it early.
`save(path=None)` stops collection and exports CSV through application stdout,
using Pybricks' `PB_OF` / `PB_EOF` framing and default name `log.txt`. Pybricks Code
can receive that file; a plain terminal displays the stream. It does not write
hub flash. Columns are:

| Index | Value |
| --- | --- |
| 0 | Elapsed log time, ms |
| 1 | Monotonic control time, 100 µs ticks |
| 2, 6 | Measured and estimated output angle, degrees |
| 3, 7 | Measured and estimated output speed, degrees/s |
| 4 | Actuation in bits 0–1 (coast 0, voltage 2); stalled in bit 2 |
| 5 | Applied voltage, mV |
| 8, 9 | Feedback and feedforward torque, µNm |
| 10 | Observer feedback voltage, mV |

`motor.control.log` uses the same `start`, `stop` and `save` methods with an
independent buffer (52 bytes per row). It records only regulated control updates;
DC, brake and coast do not advance this log. Its columns are:

| Index | Value |
| --- | --- |
| 0 | Elapsed log time, ms |
| 1 | Time since trajectory start, 100 µs ticks |
| 2, 6, 8 | Measured, reference and estimated output angle, degrees |
| 3, 7, 9 | Measured, reference and estimated output speed, degrees/s |
| 4 | Actuation in bits 0–1; stalled, complete and paused integration in bits 2–4 |
| 5 | Requested control payload (torque in µNm for regulated actuation) |
| 10, 11, 12 | Actual proportional, integral and derivative torque, µNm |

The reference angle includes PBIO's speed-integrator pause adjustment. These
values come from PBIO's existing logging hook, without recalculating PID terms.
The final D column repairs an omission in the pinned reference logger's declared
row width. Its intermediate positions use signed 32-bit shaft millidegrees;
exceeding that range stops the control log with a range error, while motion
continues with its existing long-angle support.

The worker captures values before predicting the next state. Signed values use
the configured direction. Logs belong to the port; aliases share them. Closing
or disconnecting the motor stops collection and retains captured rows. A later
`start` replaces the log, while application cleanup frees it regardless of
Python finalizers. Allocation failure preserves the previous log. An acquisition
error stops collection and is reported after exporting its retained rows.

Native applications use `bp_servo_setup`, `bp_servo_get`, `bp_servo_reset` and
`bp_servo_move` for this same coordinate/configuration view. Setup, reset and move
return request tokens; wait for driver completion through `bp_motor_get`.
`bp_servo_settings_get/set` and `bp_servo_trajectory_get` expose the same settings
and diagnostics. A setting update uses the revision from `get`, returns a new
revision, and reports completion through `applied`; it does not replace the motor
request. Reads and commands validate the physical connection generation. The
existing raw motor APIs continue to use physical shaft units without applying
this transform. `bp_servo_model_get` and `bp_motor_log_start/status/read` expose
the same model snapshot and bounded row access without retaining user buffers.
Select `BP_MOTOR_LOG` or `BP_CONTROL_LOG`; status reports the row width.
Rows use only that many entries in the fixed maximum-size output structure.

Protected ARM checks cover coordinate sharing, absolute/explicit reset, externally
moved encoder feedback, motion, voltage limiting, blocking/awaitable completion,
replacement and supervised cleanup with synthetic UART/PWM. Numerical simulation
additionally checks long-origin tracking, smart relative motion and stall-to-hold.
Settings updates, gearing, active-trajectory preservation and diagnostics also
have numerical and protected ARM checks. Model tuning, CSV units/downsampling,
retained logs and forced-stop memory cleanup have synthetic checks too. These
are not physical motion or tuning measurements. Complete Motor compatibility has
not been established.

### Python motor hardware checks

The full Python profile has bounded checks on one Robot Inventor F4 (PCB revision
unknown), with two ID-75 motors on A/B and suspended wheels. Temporary Python
programs run from RAM through native USB/BLE management, with a 3500 mV voltage
limit and cleanup checked through the shared native motor snapshots:

- USB: `Motor.dc()` in both directions on both ports; blocking `run_angle()` and
  `run_target()` reach their encoder targets within the configured tolerance.
  `DCMotor` with `Direction.COUNTERCLOCKWISE` reverses the raw encoder direction.
- BLE: concurrent awaitable timed moves complete; a replacement through an alias
  survives closure of the old awaitable. Stopping A leaves B running, and host
  application stop releases both outputs.
- USB: `run_until_stalled()` ends with coast after the operator restrains A's
  wheel; the operator confirms that drive effort disappears.

Completed checks leave both outputs coasting with no port owner. Port connection
generations and error counters remain unchanged. These checks establish bounded
Python motor operation, not mechanical angle accuracy, controller tuning under
load, all motor models or robotics trajectories. Physical gearing, model/logging
behavior and the remaining reference examples still require acceptance.

## Paired drive control

`pybricks.robotics.DriveBase` combines two configured `Motor` objects with the
same gearing. Both shafts use the existing port worker and observers on a common
5 ms cadence. The ported PBIO distance/heading controllers and synchronized
trajectories supply the outputs; there is no Python control loop or second motor
driver.

```python
from pybricks.pupdevices import Motor
from pybricks.parameters import Direction, Port
from pybricks.robotics import DriveBase

left = Motor(Port.A, Direction.COUNTERCLOCKWISE)
right = Motor(Port.B)
robot = DriveBase(left, right, wheel_diameter=56, axle_track=112)
robot.straight(100)
robot.turn(90)
robot.stop()
```

This example moves both motors. Choose ports, motor directions and dimensions
for the actual robot. Distance is in millimetres and heading in degrees.
`straight`, `turn`, `arc` and `curve` block, support `wait=False`, or can be awaited
inside `run_task`; `turn` also accepts `absolute=True`. `drive(speed, turn_rate)`
runs until replaced or stopped. As in Pybricks, `done()` is true for this
non-waiting operation even while its controllers remain active. `stop`, `brake`
and `hold` select passive coast, electrical brake and position hold.
`distance`, `angle`, `state`, `stalled`, `reset` and `settings` expose the shared
native state. Acceleration settings accept a scalar or an acceleration/deceleration
pair.

`move_by(dx, dy, then=Stop.HOLD)` moves by a fixed-heading displacement in
millimetres, with each component limited to ±32767. It turns to the absolute
direction first and holds that heading, then drives straight for the vector's
length using `then` on completion. The native pair worker owns both phases, so
replacement or stop cancels the whole move. It blocks outside `run_task` and
returns an awaitable inside it; this method has no `wait=False` argument.

`use_gyro(True)` selects the application's shared IMU heading; it defaults to
encoder heading. Changing the selection stops the pair, while selecting its
current value leaves motion intact. In gyro mode, `reset(distance, angle)` also
resets the shared IMU heading. Direct IMU configuration is rejected while a
gyro-controlled pair is active. Acquisition failure or a changed IMU reference
epoch stops both motors and reports the cause.

An individual Motor command cancels active paired motion and coasts the other
motor; the DriveBase can be reused. Closing or reconstructing either Motor, or
losing its physical connection, invalidates the pair. Rejected geometry or
replacement motion preserves existing output. Rejected replacement also retains
the prior Python awaitable; closing a superseded awaitable cannot stop its
replacement. Application cleanup releases both associations without relying on
Python finalizers. Normal host disconnect leaves autonomous motion running.

`SpikeBase(left_motor, right_motor)` uses the same paired worker without wheel
dimensions. Its `tank_move_for_degrees`, `tank_move_for_time` and
`tank_move_forever` methods take left and right wheel speeds in degrees per
second. As in Pybricks, the left speed is negated relative to its Motor's
positive direction, while the right speed is unchanged. The corresponding `steering_move_for_degrees`,
`steering_move_for_time` and `steering_move_forever` methods take speed and
steering from −100 to 100. Angle moves use wheel degrees, with the faster wheel
covering the requested angle; finite time moves accept 0–299999 ms. Use the
dedicated `*_forever` methods for continuous motion. Finite methods default to
`then=Stop.HOLD` and `wait=True`; waiting blocks even inside `run_task`.
`SpikeBase.stop()` coasts the pair, and `.left` and `.right` reference its motors.

Both classes expose persistent, read-only `.distance_control` and
`.heading_control` objects with the same `Control` methods as `Motor.control`:
`limits`, `pid`, target and stall tolerances, `trajectory`, `done`, `load` and
`stalled`. Each controls its own paired axis. `DriveBase.settings()` shares their
default speed and acceleration settings; the speed returned by `Control.limits()`
is the maximum, not that default. Per-axis `done/load/stalled` describe one
controller, while `DriveBase.done()` describes the entire maneuver, including
both phases of `move_by`. `trajectory()` uses Motor's raw PBIO diagnostic units
rather than millimetres or heading degrees.

Each control's `.log` supports the same `start(duration, down_sample=1)`,
`stop()` and `save(path)` CSV methods as Motor. Its 13 columns include measured
and reference position/speed in millimetres for distance and degrees for
heading; SpikeBase uses shaft-angle units on both axes. Logging pauses with
passive control without discarding rows. Pair retirement stops collection and
retains rows until application cleanup or reuse of the bounded pair slot; an
old handle cannot access a new pair's log.

Native applications use `bp_drivebase_open/get/command/settings_set/close`,
`bp_drivebase_axis_get/set` and `bp_drivebase_log_start/status/read`.
Setup contains both port generations and dimensions in micrometres. Open and
queued commands expose request/completion tokens; completion acknowledges
application of the command, while `done` reports maneuver completion separately.
Settings do not replace the motion token. Close is asynchronous: `get` returns
`ENODEV` after retirement. Error, command rejection and cleanup error are separate
fields. A failed pair must be closed and reconstructed before reuse.

Numerical, native simulator and protected ARM checks cover paired scheduling,
replacement, gyro configuration/reset, cancellation, hotplug, output failure and
application cleanup with synthetic devices. Numerical checks cover `move_by`'s
two phases and cancellation; protected ARM checks cover its wait/await behavior
and SpikeBase's wheel directions, timed completion and interruption. Physical
paired motion and gyro accuracy remain unverified. Protected ARM checks also
cover shared control settings, trajectory preservation, CSV units, retained rows
and log-memory cleanup.

## Car steering and drive

`pybricks.robotics.Car(steer_motor, drive_motors, torque_limit=100)` uses the
same native Motor controllers. `drive_motors` accepts one Motor or a list/tuple
of up to five, on distinct ports from each other and the steering motor.
Construction moves the steering motor to both mechanical end stops at
300 degrees/s, resets the midpoint to zero and starts tracking that center.
It returns after accepting the center target, without waiting to reach it.
Construct the car before starting `run_task`.

`torque_limit` is a percentage of the steering motor's configured maximum
torque, not battery voltage. Each end-stop search times out after ten seconds;
an interrupted or failed search coasts its command. A steering half-range
smaller than the ten-degree margin is rejected. Use a mechanism with real
end stops and a suitable torque limit; construction actively moves it.

- `steer(percentage)` selects −100…100% of the calibrated range, leaving ten
  degrees before each end stop.
- `drive_power(power)` accepts −100…100. Values with magnitude below 10 coast;
  other values use Pybricks' friction-compensated duty mapping.
- `drive_speed(speed)` runs every drive motor at the given output degrees/s,
  respecting its direction, gearing and control limits.

The motors retain independent controllers. They do not form a synchronized
DriveBase pair. Errors detected during a Car operation coast commands still
owned by that object, preserving newer commands issued through other objects.
Between Car calls, device faults are handled per port; another drive motor may
continue running. The application supervisor releases all ports on program exit
or forced stop. `drive_power(0)` stops the drive by coasting; steering keeps its
target until replaced or the application ends.

Native programs select `BP_SERVO_UNTIL_STALLED_TORQUE` and `torque_limit` in
`bp_servo_move` for the same torque-capped search. The limit is 0…1000 permille
of configured torque, or −1 for the full configured limit. The existing
`BP_SERVO_UNTIL_STALLED` retains its battery-relative `duty_limit` semantics.

Numerical checks cover torque saturation, stall completion and restoration of
the configured limit for subsequent commands. Protected ARM checks exercise
calibration, steering targets, drive commands, interruption and application
cleanup with synthetic UART encoders. These encoders move independently of PWM;
physical steering calibration, torque and vehicle behavior remain unverified.

## Speed and position control

`CONFIG_BRICKPHYR_MOTOR_CONTROL` enables the PBIO observer, feedback controller
and acceleration-limited trajectories for the supported BOOST, Technic and SPIKE
UART motors. It requires encoder acquisition, direct motor outputs and valid
battery observations. Passive motors have no feedback and reject these commands.
The implementation is experimental: synthetic feedback/lifecycle checks and F4
compilation do not establish physical tuning or control timing.

The same USB/BLE Shell accepts:

```text
motor A run 300000 200 coast
motor A target 90000 300000 hold
observe control.A
motor A coast
motor A release
```

These commands move a motor. `run` takes signed millidegrees/second, duration in
milliseconds, and completion behavior (`coast`, `brake`, `hold`, `continue`). Zero
duration runs indefinitely until replaced or stopped; finite duration is
1–299999 ms, below PBIO's forever sentinel. `target` takes an absolute encoder
angle in millidegrees, nonnegative speed and completion behavior. Zero target
speed selects the model's rated limit. Angles use the current connection's
accumulated encoder origin; reconnect resets that origin. The maximum single
target displacement is less than `INT32_MAX / 2` millidegrees. Speeds above the
model's rated limit and numerically unsafe angles are rejected without replacing
the current command. Position commands also reject a nonzero displacement whose
speed rounds to zero in PBIO's 100 millidegrees/second trajectory units, or whose
planned duration exceeds the trajectory representation. Gear ratios and direction
remapping are not applied.

Native code calls `bp_motor_control(port, &command)` with a
`struct bp_motor_command` from the matching SDK. Set `mode` to `BP_MOTOR_SPEED`
or `BP_MOTOR_POSITION`, `finish` to a `BP_MOTOR_FINISH_*` value, and provide the
same speed, target and duration units. Unused target/duration fields must be zero.
The syscall validates and copies application memory before taking a service lock.
There is one port owner and output path for direct drive and regulation.
Coast/release, terminal disconnection and application cleanup follow the direct
drive rules above. A direct duty/brake command also cancels regulation.

Command acceptance is asynchronous. `bp_motor_get()` reports the accepted
`command`, driver request/completion identifiers, and a `control` snapshot.
Driver completion means the output operation completed, not that the shaft
reached its target. `control.done` reports trajectory completion; hold can be
both done and active. `stalled` reports PBIO's stall detection without silently
changing the chosen completion policy. Finite duration starts when the worker
applies the command. Replacing an active regulated command waits for its next
control frame; coast does not wait for that frame.

The existing port work item runs on a dedicated preemptible queue when regulation
is enabled. PBIO's Powered Up models use a 5 ms step; absolute deadlines preserve
that cadence. Losing a whole frame, including time spent applying output, reports
`-ETIMEDOUT` and coasts when service resumes. No catch-up integration fabricates
missing frames. The F4 queue follows the 1.2 ms IMU acquisition and precedes
HCI TX/RX, USB processing, management, Shell and native applications;
cooperative work and interrupts can still delay it. This is an
overrun policy, not an independent hardware watchdog or a hard real-time guarantee.

Missing angle data for the full 100 ms numerical derivative window stops
regulation before stale feedback can represent a stopped shaft indefinitely.
Invalid encoder data and battery acquisition errors stop it immediately when
observed by the worker. The last valid battery voltage converts requested mV to
duty, bounded by both the model's voltage and the available supply. The observer
predicts using the voltage corresponding to the successfully applied duty, rather
than an unclipped request. This is a calculation from battery/PWM, not measurement
of winding voltage or current. Frame timestamps/counters and missed-deadline
counts are visible in `observe control.A` through `control.F`; voltage/reference
fields describe the last successful control frame, with `active` distinguishing
ongoing regulation from historical values.

### Continuous motor observation

`motor A observe` claims a negotiated encoder motor, coasts it and starts the
same PBIO observer used by regulation. `observe control.A` then includes
`observing`, `observation_ms`, `speed_mdps`, `load_mnm` and `dc_stalled`, alongside the existing
trajectory fields. A regulated command starts observation automatically.
Subsequent duty, brake and coast commands retain it; release ends it.
Monitoring without a claim does not start the model or change an output.

Native applications use `bp_motor_observe(port, generation, &request)` and wait
for driver completion through `bp_motor_get`. Read
`bp_motor_observation_get(port, generation, window_ms, &sample)` for timestamped
shaft angle, numerical/estimated speed, model load, applied-voltage estimate,
stall state and frame count. Inspect `sample.error` before using values.
Coordinates are physical shaft millidegrees, speeds are millidegrees/second,
load is signed millinewton-metres, and voltage is millivolts. These are estimates,
not torque or winding-voltage measurements. No Python gearing or direction is
applied at this boundary.

The numerical speed window rounds to the nearest 5 ms, accepting 3..302 ms;
100 ms is the default in telemetry. Large values are rejected before the PBIO
sample-count conversion. Coasting reports zero load and no stall. During active
regulation, load/stall come from the controller; during direct drive they come
from the observer. Its stall persistence gate starts when the model detects
stall conditions, not when the drive command was issued.

One 5 ms cadence updates the model through all these modes. Idle observations do
not rewrite GPIO/PWM. Driver calls and PBIO computation remain outside the service
spinlock; a bounded publication copy makes reads independent of model mutation.
Only claimed motors with observation enabled incur this work. Release, hotplug,
invalid/stale measurements and a missed frame stop observation; an error is
reported instead of a stale zero-speed result. Restart requires an explicit new
observation or regulated command. Coast and brake observation need no battery
reading, while nonzero duty prediction requires a valid cached battery voltage.

Native simulation covers cold observer start, externally moved encoder samples,
idle PWM inactivity, direct-drive stall, release and feedback loss, alongside
regulated-command lifetime and driver failures. These checks do not establish
physical sensor accuracy or multi-motor scheduling margins. The Python `Motor`
binding uses this service; its control-settings interface remains unfinished.

The [computation adapter](../src/motor/README.md) records source provenance and
numerical boundaries. PBIO computation is reused without its device scheduler;
Zephyr port ownership, driver errors and cleanup remain authoritative.

## Detection and validity

The pin sequence and lookup table are a selected MIT-licensed port of Pybricks
[`port_dcm_pup.c`](https://github.com/alepoydes/pybricks-micropython/blob/a7b1b04ae4f8449dd46b3dfb97c420334439fd35/lib/pbio/src/port_dcm_pup.c).
Pin reset and board wiring follow `lib/pbio/drv/ioport/ioport.c` and
`lib/pbio/platform/prime_hub_f4/platform.c` at the same revision. Source notices
and the [MIT license](../LICENSES/Pybricks-MIT.txt) are retained.

`src/hub_ports.c` preserves the electrical tests, the extra RX discharge and
20 repeated matches after the initial candidate. Zephyr GPIO replaces PBIO GPIO;
one delayed work item advances all six ports with at least 2 ms between tests.
There is no PBIO scheduler, per-port thread, dynamic allocation or busy wait.
Empty and passive ports continue GPIO detection. A confirmed UART signature
hands the pins to the serial service when enabled; GPIO tests resume after
negotiation failure or serial disconnection.

| `phase` | Meaning |
| --- | --- |
| 0 | Detecting; `error=-EAGAIN`, no confirmed identity |
| 1 | Confirmed empty port |
| 2 | Confirmed passive electrical ID |
| 3 | UART signature, before negotiation or with UART support disabled |
| 4 | GPIO, UART, protocol or cleanup failure |
| 5 | Service stopped |
| 6 | Negotiating UART metadata; `error=-EAGAIN` |
| 7 | Negotiation complete; `error=-EAGAIN` until the first valid DATA |

Known passive IDs include 1 (medium motor), 2 (train motor), 8 (lights), and
5 (a contact-switch circuit, not an official released Powered Up sensor).
`pressed` is meaningful only for ID 5. Other Pybricks electrical IDs are retained
as identification results; they do not imply an implemented device API.

The first conflicting completed scan invalidates the previous identity while
the new candidate settles. `generation` changes when the published phase or
identity changes; a press on an unchanged contact switch does not change it.
`sequence` counts completed electrical scans and accepted UART DATA frames.
`sample_ms` marks the latest service update, including a failure;
`changed_ms` marks the latest phase/identity change. Observation acquisition is
independent of application and host lifetimes.

Connection errors invalidate identity and DATA, increment the saturating
`failures` counter, stop UART interrupts, release the test outputs and retry
electrical detection after 100 ms. `error` preserves the operation
failure; `cleanup` reports a later pin-release failure separately. A failed port
does not stop other ports. Cached results older than the longest ten-step
probe, including kernel tick rounding, report `-ETIMEDOUT`. A getter never
increments counters or changes the acquisition state.

When automatic detection or Powered Up communication produces a port's first
failure since boot, the bounded `diagnostics` log records its operation and cleanup
errors, probe step, timestamp and service delay. UART failures additionally report
the negotiation/parser phase, interrupt error and raw UART flags, queued bytes
and last DATA timestamp. Logging follows
interrupt shutdown and output cleanup attempts, before recovery resets the UART
state. Later failures increment the counter without flooding the log.

## UART negotiation and raw observations

The selected parser in `src/ports/lump.c` comes from Pybricks
[`port_lump/protocol.c`](https://github.com/alepoydes/pybricks-micropython/blob/a7b1b04ae4f8449dd46b3dfb97c420334439fd35/lib/pbio/src/port_lump/protocol.c)
under the same MIT license. The pinned `lib/lego/lego/lump.h` definitions are used
unchanged. Brickphyr separates byte parsing from Zephyr UART scheduling, checks
field lengths before access, requires a NAME and FORMAT for every advertised
mode, and bounds the entire negotiation. No PBIO coroutine runs underneath it.
Optional metadata is retained for [device inspection](#device-inspection),
with length and duplicate checks. A checksummed TYPE byte is accepted even when
no device-specific driver recognizes it; that does not authorize power or motion.

The hub offers 115200 baud. Devices without the fast-handshake ACK fall back to
2400 baud after 10 ms. After metadata and the hub's ACK, it waits 10 ms from the
ACK's final stop bit before adopting the advertised rate (2400–460800 baud).
An incomplete message or missing first TYPE times out after 250 ms; the entire
handshake has a 33.7-second bound derived from 16 modes, 14 INFO types, maximum
frames at 2400 baud and startup. Repeated SYNC traffic cannot hold a port forever.
DATA mode sends NACK keepalives every 100 ms; 300 ms without valid DATA restarts
detection. These waits yield to Zephyr instead of blocking a service thread.

Each snapshot includes `mode`, `num_modes`, `data_type` (0 int8, 1 int16, 2 int32,
3 float32), `num_values`, `baudrate`, `capabilities`, `data_sequence`, `data_ms`
and up to 32 `data` bytes. Multi-byte wire values are little-endian. Payload
padding is removed according to the negotiated FORMAT. `observe` prints DATA
as hexadecimal; `port A` also decodes its wire format. SMP sends a byte string,
and the existing JSONL monitor records its bytes as an integer array. All
consumers read the same cache.

`data_ms` is the last valid frame's parser completion time on the hub, not the
sensor's acquisition time or first-byte interrupt time. `data_sequence` wraps
and restarts on connection; `bad_frames` saturates and also restarts. A bad DATA
checksum increments that counter without replacing the previous sample or its
time. During initial TYPE synchronization, a peer's BREAK/framing indication is
discarded while seeking a valid TYPE packet. SPIKE motor ID 75 emits this after
the fast-baud acknowledgement. Once TYPE is known, framing errors are fatal.
Other UART errors and RX overflow are fatal throughout negotiation.
Invalid packet formats also invalidate the connection. This is a latest-sample
interface, not a lossless
recording of every UART packet. Neither an idle poll nor a keepalive makes an
old measurement fresh.

`capabilities` retains the NAME flag byte defined by LUMP. SPEED, ACK, NACK,
validated SELECT and writable-mode EXT_MODE/DATA are transmitted. The separate
LUMP `CMD_WRITE` command is not exposed.
Unknown devices retain raw observations without automatic mode selection or
additional power. Their identity alone does not imply a working device API.

## Electrical ownership and shutdown

The service owns ID1/ID2, buffered TX/RX, buffer enables, pins 1/2 and the shared device
logic supply (PA14 on F4). It initializes all ports with inputs and disabled
buffers and coasts the H-bridges before enabling that supply. A startup reset
failure keeps the shared supply off. Automatic sensor acquisition enables
additional DC power only for a recognized sensor with a validated measurement
format and matching NAME flags. An explicitly configured custom UART lease can
also request P1/P2 power after acknowledgement as described above. Pin 1
or pin 2 becomes positive while the other stays low; motor flags or contradictory
power flags reject this operation. Encoder selection ignores supply flags and
does not drive, brake or hold a motor. Motor commands require a separate explicit
request through the same owner. No raw H-bridge interface is exported.

The private output adapter in `src/ports/output.c` also implements Pybricks'
H-bridge PWM behavior through Zephyr GPIO/PWM/pinctrl. Its selected source is
[`motor_driver_hbridge_pwm.c`](https://github.com/alepoydes/pybricks-micropython/blob/a7b1b04ae4f8449dd46b3dfb97c420334439fd35/lib/pbio/drv/motor_driver/motor_driver_hbridge_pwm.c),
with the retained MIT notice. It preserves forward/reverse polarity, zero-duty
braking, coast and the F4's 12 kHz PWM with 1000 duty steps. It checks parameters
and driver results; the owning service must attempt coast after an I/O failure
and preserve any cleanup failure separately.

TIM1 serves A/B, TIM4 C/D and TIM3 E/F. Timer initialization has an empty pinctrl
state, so the boot GPIO coast remains in force. A drive transition prepares the
PWM before applying that port's single-channel pinmux; steady updates change
only the compare value. Coast disconnects the timer using GPIO and disables
available PWM channels. Re-enabling a channel latches its prepared compare on
STM32 before pinmux handoff. That timer update can also restart the shared counter;
different ports do not have an atomic or synchronized command contract. Actual
waveforms and electrical transitions still need instrumented tests.

`supply` reports sensor/custom-UART DC supply: 0 none, 1 pin 1 positive, 2 pin 2 positive.
Motor output has its separate state above. Neither is voltage feedback; after a GPIO/cleanup error, the actual
output is unknown. UART loss coasts the port when detected, within the protocol
timeout rather than instantaneously at physical removal. Failed cleanup is
reported separately and retried before electrical detection resumes.

Shutdown, reboot and DFU entry join pending service work, disable UART interrupts
and the shared supply, and release all test pins. Failure to release one pin does not skip the
other releases. Applications receive snapshots, not GPIO handles.

There are at most 500 service rounds/s before scheduler and GPIO execution cost,
with at most six finite state transitions in a round. Each UART has a static
512-byte RX ring and a 37-byte TX buffer; parsing consumes at most 128 bytes per
port per round. At 460800 baud, 8N1, the RX ring covers about 11.1 ms of continuous
traffic (44.4 ms at 115200). These bounds limit work and make overflow explicit;
they do not establish that all six ports sustain their maximum rate together.
No catch-up burst runs after a late round. CPU/ISR cost, stack peaks and physical
timing under concurrent IMU, audio and radio load remain unmeasured.

Native simulation checks six synthetic electrical circuits, debouncing, touch
state, hotplug, failure isolation, cleanup and stale observations through the
production GPIO path. ARM QEMU checks protected SDK access and actual SMP
observations. UART simulation additionally exercises upstream metadata captures,
both handshake rates, partial/corrupt messages, finite negotiation, keepalive,
hotplug, framing errors, RX overflow, SMP raw data and shutdown. Synthetic sensor
peers additionally check conversion, settling, SELECT timeout, ownership, malformed
calibration, power polarity and cleanup failure through the production service,
Shell and SMP. Writable-mode checks cover extended DATA framing and padding,
copied payloads, completion and release before/during transmission.
Encoder cases check both wrap directions, signed counter rollover,
malformed values, hotplug reset and inactive motor terminals. These are not
physical sensor measurements. Fixture provenance is in
[the test README](../tests/lump/README.md). F4 compilation does not verify
connector voltages, analogue settling, actual baud transitions, controller error
reporting under overload or identification of physical attached devices.

On one Inventor F4, two SPIKE motors reporting ID 75 on A/B complete fast-baud
negotiation and expose their metadata and encoders through Shell/SMP. Short
positive/negative direct-drive commands, timed speed control, position targets,
hold and release pass with suspended wheels. The protected native motor example
also completes and releases its port. These checks establish direction and
bounded unloaded operation, not controller tuning under load, synchronized axes,
stopping distance or third-party sensor compatibility. Color/ultrasonic coverage
is described under [sensor hardware checks](#sensor-hardware-checks); remaining
sensor conversions and electrical supply behavior need separate acceptance.

An unexpected UART reconnection was observed during a combined application and
transport sequence; its trigger is still under investigation. Reconnection resets
the encoder origin, so those transitions are not yet a verified operating mode.
