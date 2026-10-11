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
Motor control is not implemented. Reading state does not start I/O, select a mode
or claim a port.

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
Optional units/scaling metadata is checksum-checked but not interpreted yet.

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
padding is removed according to the negotiated FORMAT. The Shell prints DATA
as hexadecimal; SMP sends a byte string and the existing JSONL monitor records
its bytes as an integer array. All consumers read the same cache.

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
keeps the motor terminals in coast, without brake or hold. No raw H-bridge
interface is exported.

The private output adapter in `src/ports/output.c` also implements Pybricks'
H-bridge PWM behavior through Zephyr GPIO/PWM/pinctrl. This is hardware support;
motor command ownership, native/Shell commands and regulation are not implemented
yet. Its selected source is
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

`supply` reports the accepted DC configuration: 0 coast, 1 pin 1 positive, 2 pin 2
positive. It is not voltage feedback; after a GPIO/cleanup error, the actual
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
