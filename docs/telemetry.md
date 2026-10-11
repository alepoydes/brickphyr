# Observations

`observe` or `observe all` reads current observations in the shared USB/BLE Shell.
Use `observe power`, `observe buttons.center`, `observe buttons.side`,
`observe application`, `observe matrix`, `observe imu`, `observe imu.calibrated`
`observe imu.orientation`, `observe sound`, `observe battery` or `observe port.A`
(through `port.F`) for one source. Unsupported sources
report an error; `all` includes the sources compiled into the image.

Observation is read-only. It does not start ADC/SPI transactions, change a sensor
mode, claim a device, consume application output, expire uploads or unload an
application. Device services supply snapshots under their existing locks. Native
lifecycle work can return `-EBUSY`; observation does not wait for it. Application
state is the supervisor's latest state, which its normal polling updates.

No observation thread, history buffer or periodic transmission runs on the hub.
A client requests the latest snapshot at its chosen display rate. Samples acquired
between requests are not a recorded history. Slow readers cannot accumulate an
unbounded telemetry queue. A management connection retains the existing
[power hold](settings.md#inactivity-shutdown); closing it leaves an autonomous
application running.

## Text monitor and recording

From the repository, use the same installed `brickphyrdev` tools as for applications:

```sh
uv run brickphyrdev monitor
uv run brickphyrdev monitor --ble Brickphyr
uv run brickphyrdev monitor --source power --rate 2 --log telemetry.jsonl
uv run brickphyrdev monitor --ble Brickphyr --source buttons.side --count 10
```

USB is the monitor's default transport and selects Shell automatically. Choose a
particular hub with `--serial NUMBER`, or an already selected port with
`--port /dev/ttyACM0`. Close another management client before connecting.

The monitor shows all advertised sources, or the sources selected with repeated
`--source NAME`. One selected source shows detailed fields and matrix frames;
several sources show a compact overview. A TTY gets a refreshed screen; redirected
stdout gets plain text snapshots. Errors and sample/read ages remain visible.
`Ctrl+C` closes the standalone monitor. `--count N` exits after N refresh cycles.

`--rate` is a maximum display rate, default 2 Hz, up to 20 Hz. It does not change
the devices' acquisition rates. Only one SMP request is outstanding. The scheduler
counts missed display periods and skips them, including time spent formatting
and recording; a slow connection or disk does not produce catch-up bursts.

To keep a terminal and monitoring in one connection:

```sh
uv run brickphyrdev terminal --usb --monitor --log telemetry.jsonl
uv run brickphyrdev terminal --monitor --source power
```

The terminal retains its existing default BLE target `Brickphyr`; `--usb` selects
USB. In this combined mode, `Ctrl+T` switches between the original Shell screen
and the monitor; `Ctrl+]` closes the connection. Monitoring and optional recording
continue in either view. Keyboard editing, history and completion still run in
the same Zephyr Shell on the hub. No host-side command parser is added. If new
Shell output arrives while the monitor is visible, the client returns to the
Shell so that output is not hidden or discarded. While viewing the monitor,
ordinary keyboard input is ignored; switch back before typing commands.

The combined client uses standard serial SMP framing inside the existing native
USB/BLE console stream, with one writer and connection owner. BLE does not open
the separate SMP GATT service at the same time. A cancelled or timed-out exchange
ends that console session so a late response cannot belong to another request.

`--log FILE` creates a new JSONL file on the computer and refuses to overwrite an
existing file. Nothing is logged to hub flash. The first record identifies the
schema (`brickphyr.telemetry/1`), session, firmware/build, board, selected sources
and requested rate. Each snapshot records hub time, acquisition/completion times,
errors and values, together with host UTC and monotonic reception time. Matrix
byte strings become integer arrays. Complete lines are flushed to the operating
system; this is not an fsync guarantee. The last record reports completion,
cancellation or failure, record counts and missed periods. An abrupt process or
disk failure can leave an incomplete final line or omit the ending record.

The combined terminal currently requires a POSIX TTY and is checked on Linux
pseudoterminals. New host monitoring and recording have simulation checks;
verification with a physical USB/BLE hub remains pending.

## Time and validity

Hub times use monotonic uptime: milliseconds for `_ms` fields and microseconds
for `_us` fields. `read_ms` is when capture
finished, not when a sensor measurement happened. `sample_ms` belongs to the last
acquisition attempt; repeated reads preserve it. `applied_ms` belongs to the last
successful matrix write. The two button ladders have separate snapshots and times;
reading all sources is not an atomic robot-wide measurement.

`error` is zero for available data or a negative firmware errno. An unavailable
or unstable reading is not a zero measurement. Fields can still describe policy,
raw input or previous output while `error` is nonzero. Staleness is visible from
the acquisition time; a client must not replace it with its reception time.

Matrix observations distinguish the requested frame, the last successfully
applied frame, pending work and validity. Applied means the controller accepted
the write, not that light was physically measured. See [matrix ownership](hardware.md#hub-matrix).

## Experimental SMP API 1

Group **66** uses standard SMP framing and CBOR over the existing
[management transports](host.md). Command **0**, read, returns `api`, `firmware`,
`build`, `board` and the available `sources` names. Command **1**, read, takes
`{"source": "power"}` and returns `api`, `source`, `read_ms`, `error` and `values`.
Writes are unsupported. An unknown name is a protocol error; a known source
excluded from a build returns an empty snapshot with `-ENOTSUP`.

One request reads one source. Responses fit the existing 384-byte SMP buffers;
the client combines several responses for a display. No separate telemetry wire
format, connection owner or Bluetooth service is introduced.

| Source | Values and units |
| --- | --- |
| `power` | `sample_ms`, `source` (0 battery, 1 USB, negative errno unknown), `idle_since_ms`, `battery_idle_s`, `usb_idle_s`, `connected`, `application`, `shutdown` |
| `buttons.center` | `sample_ms`, `raw` (ADC counts), `failures`, `stable`, `center` |
| `buttons.side` | `sample_ms`, `raw` (ADC counts), `failures`, `stable`, `left`, `right`, `bluetooth` |
| `application` | `state`, `result`, `fault`, `cleanup`, `image`, `heap`, `loaded`, `stack`, `unused`, `dropped`, `queued`, `received`, `total`, `ready` |
| `matrix` | `applied_ms`, `failures`, `owner`, `pending`, `valid`, `requested`, `applied` |
| `imu` | `sample_us`, `completed_us`, `sequence`, `missed`, `failures`, `ax_um_s2`, `ay_um_s2`, `az_um_s2`, `gx_mdps`, `gy_mdps`, `gz_mdps` |
| `imu.calibrated` | `sample_us`, `sequence`, `epoch`, `ready`, `stationary`, calibrated `ax_um_s2`, `ay_um_s2`, `az_um_s2`, `gx_mdps`, `gy_mdps`, `gz_mdps`, current bias `bx_mdps`, `by_mdps`, `bz_mdps` |
| `imu.orientation` | `sample_us`, `sequence`, `epoch`, `ready`, rotation matrix `m11`…`m33` multiplied by 1000000, `heading1_mdeg`, `heading3_mdeg` |
| `sound` | `changed_ms`, `owner`, `phase`, `mode`, `requested_hz`, `actual_hz`, `queued`, `completed`, `frequency`, `duration_ms`, `volume`, `eof`, `underruns`, `failures`, `cleanup` |
| `battery` | `sample_ms`, `completed_ms`, `sequence`, `failures`, `voltage_mv`, `current_ma`, `voltage_raw`, `current_raw` |
| `port.A` … `port.F` | `sample_ms`, `changed_ms`, `sequence`, `generation`, `failures`, `phase`, `type_id`, `cleanup`; passive `pressed` or UART format, timestamps and raw `data` as defined by [external ports](ports.md) |

IMU times are microseconds on the hub's monotonic clock. `sample_us` is the last
accepted gyro data-ready edge; `completed_us` is the last read attempt's end.
Acceleration is µm/s² and angular velocity is millidegrees/s, in the uncalibrated
Pybricks hub frame. An acquisition error retains the previous values and sample
time. A sample older than four nominal periods reports `-ETIMEDOUT`; it is never
relabeled with the time of a request. `missed` counts observed edges not published
as samples, including discarded reads; it cannot count interrupts the MCU never
observed. See [IMU acquisition](hardware.md#hub-imu) for synchronization limits.

Calibrated and orientation sources use the same processed snapshot. Separate
requests can see different frames; compare their `sequence` and `epoch` when
combining them. Acceleration includes gravity. The row-major matrix maps hub
axes to the inertial frame, and headings are clockwise-positive millidegrees.
Missing data or a calibration change starts a new reference with a different
`epoch`. `ready` means a bias estimate from the last ten minutes, not proven
physical rest or absolute heading. Errors retain previous computed values and
must be checked before using them. See [calibration](settings.md#imu-calibration).

Scalars are CBOR integers. Flags are 0/1. Button values are 0 released, 1 pressed,
or negative errno when the captured sample is unreadable or not yet debounced.
Application fields retain the meanings of [application group 65](host.md#native-applications):
sizes and output counts are bytes; state is 0 idle, 1 running, 2 returned, 3 stopped,
4 fault. Observation never drains `queued` bytes. A completed thread may still
show running until the supervisor processes its completion.

Matrix owner is 0 free, 1 application or 2 terminal. The two frames are CBOR byte
strings of 25 row-major brightness values, each 0–100. Bytes represent brightness,
not encoded text. Native SDK calls and Shell commands share the same matrix
service; telemetry adds no alternate control path.

Sound owner is 0 free, 1 application or 2 terminal. Phase is 0 idle, 1 priming,
2 playing (including accepted device startup), 3 stopping or 4 failed cleanup;
mode is 0 tone or 1 PCM. `queued` counts samples waiting outside DMA; `completed`
counts consumed payload samples, excluding silence padding. `actual_hz` is zero
until device startup completes. `changed_ms` belongs to the latest playback
transition; observing sound does not start, drain or stop it. See
[speaker ownership and timing](hardware.md#hub-sound).

Battery values use millivolts, milliamps and raw 12-bit ADC counts. Both sequential
reads must succeed before publishing a pair. Errors retain its previous values,
`sequence` and `sample_ms`; `completed_ms` advances with failed attempts as well.
Startup settling, stale data and stopped acquisition remain explicit errors.
Neither voltage nor current implies battery percentage or charging status. See
[battery acquisition and scaling](hardware.md#hub-battery).

Native simulation checks Shell/SMP reads, matrix payloads, unsupported sources,
write rejection and acquisition timestamps. Host tests use the real Shell/SMP
peer with fragmented, explicitly synthetic BLE delivery; they cover simultaneous
editing and management, view switching, TTY restoration, JSONL output and slow
consumers. ARM QEMU checks read-only native
observation, including busy cleanup and preserved output/allocation. These checks
do not establish physical sensor accuracy or radio timing.
