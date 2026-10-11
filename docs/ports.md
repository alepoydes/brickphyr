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
`CONFIG_BRICKPHYR_PORT_UART` adds LUMP negotiation and raw binary input, using
Zephyr UART drivers. Both options default on for F4. ID 14 is the electrical
UART signature; successful negotiation provides the device's actual ID.
Known sensors and motor encoders also provide converted measurements and explicit
mode selection.
Direct motor drive is available with `CONFIG_BRICKPHYR_MOTORS`; experimental
speed/position regulation adds `CONFIG_BRICKPHYR_MOTOR_CONTROL`. Reading state does not start I/O, select a mode or
claim a port.

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
modes, claim a port or actuate a device. Any later explicit mode selection must
use the existing port ownership and validation rules.

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
bounded strings and payloads. Publication occurs after negotiation, and reset
invalidates readers first. No heap allocation or device I/O occurs in a getter.
Shell range/float32 output enables Zephyr's floating-point formatter.

Synthetic checks cover recognized and unknown UART peers, all four wire formats,
optional and malformed metadata, maximum mode/payload sizes, reconnects and
unchanged ownership/outputs. Physical third-party compatibility and USB/BLE
inspection on the hub remain unverified. First inspect what a device advertises;
a matching ID alone does not establish compatibility.

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
has no application-specific zero offset. Color names, sensor-light commands,
infrared transmission and other device APIs are not implemented.

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
PBIO RGB/HSV converter is compiled unchanged. Brickphyr adds explicit units,
format validation, rejected malformed calibration and RGB saturation before
narrowing, preventing unusually bright BOOST input from wrapping. Acquisition,
ownership and scheduling use the existing Zephyr port service.
Encoder adaptation validates the known ID, negotiated format and absolute-angle
capability. It extends the BOOST counter without signed overflow, rejects invalid
absolute values, and uses shortest-angle increments instead of PBIO's quadrant
crossing test. These are measurement semantics, not a motor controller.

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
under physical hub load remain unverified. DriveBase is subsequent work.

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
the current command. Gear ratios and direction remapping are not applied.

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
missing frames. The F4 queue priority precedes HCI TX/RX, IMU, Shell and native
applications; cooperative work and interrupts can still delay it. This is an
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
time. Invalid framing/format, UART errors reported by the driver and RX overflow
invalidate the connection. This is a latest-sample interface, not a lossless
recording of every UART packet. Neither an idle poll nor a keepalive makes an
old measurement fresh.

`capabilities` retains the NAME flag byte defined by LUMP. SPEED, ACK, NACK and
validated measurement SELECT commands are transmitted; WRITE is not exposed.
Unknown devices retain raw observations without automatic mode selection or
additional power. Their identity alone does not imply a working device API.

## Electrical ownership and shutdown

The service owns ID1/ID2, buffered TX/RX, buffer enables, pins 1/2 and the shared device
logic supply (PA14 on F4). It initializes all ports with inputs and disabled
buffers and coasts the H-bridges before enabling that supply. A startup reset
failure keeps the shared supply off. Only a recognized sensor with a validated
measurement format can request additional DC power using its NAME flags. Pin 1
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
waveforms, transitions, stopping and motor behavior still need physical tests.

`supply` reports sensor DC supply: 0 none, 1 pin 1 positive, 2 pin 2 positive.
Motor output has its separate state above. Neither is voltage feedback; after a GPIO/cleanup error, the actual
output is unknown. UART loss coasts the port when detected, within the protocol
timeout rather than instantaneously at physical removal. Failed cleanup is
reported separately and retried before electrical detection resumes.

Shutdown, reboot and DFU entry join pending service work, disable UART interrupts
and the shared supply, and release all test pins. Failure to release one pin does not skip the
other releases. Applications receive snapshots, not GPIO handles.

There are at most 500 service rounds/s before scheduler and GPIO execution cost,
with at most six finite state transitions in a round. Each UART has a static
512-byte RX ring and a 34-byte TX buffer; parsing consumes at most 128 bytes per
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
Shell and SMP. Encoder cases check both wrap directions, signed counter rollover,
malformed values, hotplug reset and inactive motor terminals. These are not
physical sensor measurements. Fixture provenance is in
[the test README](../tests/lump/README.md). F4 compilation does not verify
connector voltages, analogue settling, actual baud transitions, controller error
reporting under overload or identification of physical attached devices.
