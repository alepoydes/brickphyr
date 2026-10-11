# Observations

`observe` or `observe all` reads current observations in the shared USB/BLE Shell.
Use `observe power`, `observe buttons.center`, `observe buttons.side`,
`observe application` or `observe matrix` for one source. Unsupported sources
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

All hub times use monotonic uptime in milliseconds. `read_ms` is when capture
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

Native simulation checks Shell/SMP reads, matrix payloads, unsupported sources,
write rejection and acquisition timestamps. Host tests use the real Shell/SMP
peer with fragmented, explicitly synthetic BLE delivery; they cover simultaneous
editing and management, view switching, TTY restoration, JSONL output and slow
consumers. ARM QEMU checks read-only native
observation, including busy cleanup and preserved output/allocation. These checks
do not establish physical sensor accuracy or radio timing.
