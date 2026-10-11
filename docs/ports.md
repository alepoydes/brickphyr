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
Device-specific sensor units, mode selection and motor control are not yet
implemented. Reading state does not start I/O, select a mode or claim a port.

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

`capabilities` retains the NAME flag byte defined by LUMP; it does not authorize
outputs. Only SPEED, ACK and NACK are transmitted. The service does not issue
SELECT/WRITE commands or supply device-specific power on pins 1/2. A device
requiring those operations may negotiate but cannot yet provide useful data.

## Electrical ownership and shutdown

The service owns ID1/ID2, buffered TX/RX, buffer enables and the shared device
logic supply (PA14 on F4). It initializes all ports with inputs and disabled
buffers before enabling that supply. A startup reset failure keeps the shared
supply off. The H-bridge inputs remain in their existing coast configuration;
this service never drives pins 1/2, including brake or hold.

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
hotplug, framing errors, RX overflow, SMP raw data and shutdown. Fixture provenance
is in [the test README](../tests/lump/README.md). F4 compilation does not verify
connector voltages, analogue settling, actual baud transitions, controller error
reporting under overload or identification of physical attached devices.
