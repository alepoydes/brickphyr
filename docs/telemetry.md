# Observations

`observe` or `observe all` reads current observations in the shared USB/BLE Shell.
Use `observe power`, `observe buttons.center`, `observe buttons.side`,
`observe application`, `observe matrix`, `observe imu`, `observe imu.calibrated`
`observe imu.orientation`, `observe sound`, `observe battery`, `observe charger`, `observe port.A`
(through `port.F`) or `observe sensor.A` (through `sensor.F`) for one source. Unsupported sources
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

The built-in capture below records selected observations at hub-local deadlines.
The text monitor provides client-requested snapshots. Application control-step
records remain available through the same [binary recorder](applications.md#application-recording).
[Grouped motor commands](ports.md#grouped-raw-motor-commands) share this Shell;
[finite schedules](#finite-motor-schedules) add hub-local timing and command records.
The F4 timer adapter's physical drive/timing acceptance remains
[planned work](roadmap.md#measurement-bench).

## Finite RAM capture

With `CONFIG_BRICKPHYR_CAPTURE`, the shared USB/BLE Shell can record observations
without uploading an application. Connect with `uv run brickphyrdev terminal`;
the default target is `Brickphyr` over BLE. These commands run in the hub Shell:

```text
capture sources
capture prepare 20 1000 imu battery encoder.A encoder.B motor.A motor.B
capture start
capture status
```

`prepare` takes a period in milliseconds, a duration in milliseconds or `full`,
then one or more source names, or `all` for every available source. It validates
the plan and reports the record size,
capacity, recorder bytes, worker/stack reservations and current free heap without
allocating or starting capture. `start` allocates those resources before sampling;
fragmentation or intervening allocations can still cause `ENOMEM`. The current
record/configuration budget is 32 KiB, shared with the application recorder.
An oversized duration fails before starting and reports the available window.
`full` stops when the allocated buffer fills. No old samples are overwritten.

Sources are raw `imu`, `imu.calibrated`, `battery`, and `encoder.A`/`motor.A`
through the board's last port. Encoder and motor blocks describe separate input
and output validity, including connection generations. Empty ports and devices
without an encoder produce explicit errors. Selection neither changes sensor
modes nor claims or energizes motors. Other sensor quantities are not yet exposed
by this capture format; they remain readable with `observe sensor.A` and peers.

Select individual values by appending a colon and comma-separated field names:

```text
capture prepare 20 1000 imu.calibrated:ay,az,gx battery:voltage encoder.A:angle motor.A:duty
```

| Source | Selectable fields |
| --- | --- |
| `imu`, `imu.calibrated` | `ax`, `ay`, `az`, `gx`, `gy`, `gz` |
| `battery` | `voltage`, `current`, `average` |
| `encoder.A` through `encoder.F` | `angle` |
| `motor.A` through `motor.F` | `duty`, `requested`, `request`, `completed`, `request_generation` |

A source without a suffix includes all its fields. `capture sources` lists the
available selections and their sizes; `status` shows the prepared selections.
Source times, sequences, connection/calibration identity, errors and validity are
always retained. Selection preserves integer precision and changes neither device
acquisition nor its rate. Unknown fields, duplicate sources/fields and empty lists
are rejected without replacing the prepared plan. Reducing fields extends the
available recording window and reduces the required payload rate; it does not
guarantee that BLE can sustain that rate.

The worker copies existing service snapshots at absolute deadlines; it does not
perform device transactions or allocate per sample. Missed releases are skipped,
not executed in a catch-up burst. Record times distinguish the scheduled release,
copy start/end and each source's acquisition/completion times. Repeated source
sequence numbers mean no new device sample. A periodic snapshot is not a
simultaneous physical measurement or a guarantee of fresh data at every period.

`capture stop` joins the worker and retains its records. The worker stack and
scratch buffer are reclaimed after completion; retained records remain allocated.
The capture exclusively owns the recorder until `capture clear`: application
start and `app record-clear` return `EBUSY` during that ownership, including after
sampling. Starting capture refuses an existing application recording with
`EEXIST`; collect it and explicitly use `app record-clear` first. Preparation and
rejected starts do not discard it. Capture is currently an idle-hub diagnostic;
concurrent application recording uses the application's existing SDK interface.

To record while keeping the same Shell available, open the terminal with a new
binary output file before preparing and starting capture:

```sh
uv run brickphyrdev terminal --record capture.cbor
```

`--record` collects one current recording, or waits for the next if none exists.
It uses binary SMP blocks on the same BLE connection as the Shell; add `--usb`
for USB. `Ctrl+T` shows collection progress and returns to the Shell without
losing a partially edited command. The terminal remains open after download
completion. This status describes collection, not measurement success: inspect
the recording outcome, source errors and skipped releases separately.
`--monitor` may share that connection too; its display rate does not set the
capture period. File encoding/writing runs in bounded batches outside the input
event loop. Slow storage delays collection without blocking keyboard handling
or hub sampling. No unbounded host queue accumulates.

The output file is CBOR regardless of its filename; using `.cbor` lets offline
tools detect it automatically. Existing files are never overwritten. Closing
with `Ctrl+]` detaches collection and retains an incomplete file for resumption;
it does not stop this passive capture or clear the hub buffer. Before any
recording appears, the created file stays empty. After a completed download,
`capture clear` releases the hub buffer. To collect successive experiments in one
terminal, use a directory instead of `--record`:

```sh
uv run brickphyrdev terminal --record-dir measurements
```

Each recording gets a separate `capture-<unique-id>.cbor` file. `Ctrl+T` shows its
path, progress and the number of completed downloads. After download completes,
use `capture clear` and `capture start` to repeat the prepared experiment. The
collector waits for a new recording generation and never clears data or starts
another experiment itself. Files are not overwritten. An interrupted file can
be resumed by its displayed path with the command below; reconnecting with
`--record-dir` starts a fresh collection file instead of resuming one implicitly.

For separate collection or resumption, close the terminal and use:

```sh
uv run brickphyrdev app --ble Brickphyr record capture.cbor
uv run brickphyrdev app --ble Brickphyr record capture.cbor --resume
uv run brickphyrdev record-info capture.cbor
```

Use `--resume` only for an interrupted download. Collection can follow a running
capture and finishes when it freezes. The binary file contains the original
packed bytes, without duplicating decoded values; the offline `Recording` reader
provides named values in `Frame.values`. The schema is `brickphyr.capture/2`;
older `/1` files remain readable. IMU values use individual names such as
`ay_um_s2` and `gx_mdps`; unselected values are absent. The mandatory calibrated
IMU `flags` contains ready/stationary in bits 0/1; battery `flags` contains
low/critical in bits 0/1. Both include their source errors independently.

The final `recording-end` event distinguishes `full`, `duration`, `cancelled` and
`error` from an application's result. A duration outcome includes the total
missed releases, including an unobserved tail. Successful downloading does not
turn a cancelled or failed capture into a completed experiment. Source errors
and skipped sample releases remain separate from transfer gaps.

Reconnect to the terminal and use `capture clear` after collecting the data.
Until then, a recording power hold prevents inactivity shutdown even without a
host. Explicit shutdown, reboot and critical-battery protection still take
precedence; RAM data cannot survive power loss or reset. Downloading is
non-consuming and does not automatically release that hold.

The Shell/lifecycle, finite buffer and binary collection path have protected
ARM/QEMU checks with synthetic devices. A bounded BLE check on one Robot Inventor
F4 (PCB revision unknown), powered through USB, also captures every supported
source group, retains data across reconnection, decodes the binary file and
releases the recording hold. The observed motor duties remain zero. This does
not establish sustained sampling latency or electrical output timing. Battery-only
checks are described under the BLE transfer budget below.

The combined `terminal --record` path has real CLI/PTY checks on ARM/QEMU,
including command input during capture, collection completion without closing
the Shell and terminal-state restoration. A bounded Inventor F4 check also runs
that CLI over one BLE connection while capturing IMU, battery and two ports,
then validates the binary file and explicitly clears the hub buffer. USB supplies
power during that check; motor outputs are not energized.

Selected-field packing has ARM/SMP checks for actual battery values, synthetic
motor duties, rejected-plan preservation and complete binary decoding. A bounded
Inventor F4 BLE check also retains selected raw/calibrated IMU, battery, encoder
and inactive-output fields, with matching IMU sequence identity, complete
collection and responsive Shell input. The host reader accepts older schemas;
these checks do not establish sustained throughput.

### BLE transfer budget

F4 native and Python profiles use four 1024-byte SMP buffers, allowing up to
912 packed recording bytes per response. This retains the request/response pool
capacity and costs 2048 bytes more static RAM than 512-byte buffers; the shared
heap budgets account for that cost. The client uses the advertised block size.

On one USB-powered Inventor F4, a retained 13,800-byte payload took 4.05 seconds
to collect over BLE at ATT MTU 130 and a 7.5 ms connection interval, versus
4.58 seconds with 400-byte payload blocks. These are bounded measurements after
connection setup, not a guaranteed rate. Initial connection-parameter negotiation
can make the first download slower; the computer controls the accepted interval.

A passive BLE check also collects sixty 276-byte observations over six seconds
while retaining Shell input (one command completed in 125 ms), resumes an
interrupted download with bytes identical to a complete reread, and retrieves
all 118 observations after the RAM buffer fills. The SMP worker retains 2032
unused bytes of its 4096-byte stack in that workload. USB supplied power.

The same live, interrupted/resumed and full-buffer paths also pass on battery
power with USB disconnected. A six-second live capture retains sixty observations
and finishes collection in 6.12 seconds; a concurrent Shell command completes in
141 ms. With a temporary three-second battery idle timeout, the hub retains the
recording across eight seconds without a BLE connection, then resumes collection
byte-for-byte identically to a complete reread. Cleanup returns heap usage to its
initial value. These are bounded passive checks on the same Inventor F4;
sustained combined load and instrumented electrical timing remain separate checks.

## Finite motor schedules

A prepared capture can include a finite sequence of grouped motor commands.
Enter these commands in the hub Shell; `at` appends an action without moving a
motor, and only `start` begins the experiment:

```text
capture prepare 20 1500 imu encoder.A encoder.B
capture at 200 300 A=200 B=200
capture at 700 300 A=-200 B=-200
capture check
capture start
```

`at` takes an offset from capture start, a command duration (both milliseconds),
then the same `A=duty|brake|coast` members as
[`motor group`](ports.md#grouped-raw-motor-commands). Duty is signed permille,
from -1000 to 1000. Actions must have increasing offsets, the same port set,
and fit wholly within the capture window. Drive and brake require a positive
duration; all-coast permits zero. A later action can replace the earlier one
before its deadline. Preparing a new capture discards its previous prepared
schedule; rejected edits preserve it. `status` shows the complete plan.
`check` revalidates device identities and availability without claiming ports
or changing outputs. Reconnecting a scheduled device requires preparing again.

`start` allocates the recording and worker, then reserves all selected idle
motors and the shared group-command history. Competing writes to those motors
and competing groups return `EBUSY`; ordinary read-only observations remain
available. The reservation spans gaps between actions and ends after output
cleanup. A live management connection is required for scheduled drive. The
existing exclusive recorder/application admission rules still apply.

Recording and actions share one monotonic origin aligned to the next millisecond
boundary, matching the motor service's absolute deadlines. Queue delay consumes
the command's validity rather than extending it. Missed actions are not replayed
in a burst: if the next action is already due, the preceding output is still
pending, or a command expires before all its outputs apply, the experiment fails
and coasts. Requested and observed driver times remain distinct; see
[F4 output timing](ports.md#f4-output-timing) for the limits
of electrical synchronization claims.

`capture stop`, a detected management-link loss or an execution error coasts the
scheduled group and retains observations. Each active command also has its own
finite deadline, independent of the capture thread and connection-loss detection.
The transport signals cancellation immediately on its disconnect notification;
the shared motor worker checks it again before applying output. This diagnostic
policy does not change autonomous applications or passive capture on disconnect.
`stop` joins cleanup before returning. Cleanup errors retain affected claims;
`capture clear` retries release before discarding the records. Successful clear
retains the prepared plan for another run.

Scheduled captures use `brickphyr.bench/3`; older `/1` and `/2` files remain readable.
The immutable configuration includes the source/field selection, period, device
generations and complete action list. Records
contain a begin/origin event, ordinary observation snapshots, action results and a
final summary. An action can produce separate application and cleanup results,
including its request identity, times, masks and first operation/cleanup errors.
The final event records attempted actions, planned/written samples and missed
releases. Missing sensor data remains a source error, distinct from schedule
failure. The offline `Recording` reader exposes these as `kind` values `begin`,
`sample`, `group` and `end`.

Records have their exact payload length: a 12-byte begin event, a 36-byte end
event, a four-byte kind plus the selected observation fields, or a 56-byte group
header plus 40 bytes per selected port. There is no padding to the largest
record. RAM holds packed payloads in small preallocated blocks and a two-byte
offset index; no allocation occurs per observation. The 32 KiB scheduled-capture
budget includes configuration, the index and block pointers, excluding allocator
overhead. Existing application recordings keep their fixed-size format.

Admission reserves up to two results per action plus begin and end events before
admitting samples. `check` shows the maximum record size, retained RAM budget,
sample payload rate and available window. The rate includes the two-byte length
prefix used for block transfer, but excludes action events and SMP/BLE overhead;
it is a required data rate, not a measured transport capability. Appending actions
can reduce a `full` capture's window or reject an explicit duration. Unused event
reservations and skipped releases can leave fewer records than the capacity when
that window ends; the final event reports the actual sample and action counts.
Collection uses the same `terminal --record` and resumable binary download as
passive capture. The offline reader also accepts earlier padded `/1` files.

Protected ARM/QEMU checks exercise the shared Shell, recorder and synthetic motor
services through preparation, opposite duties, expiry, result collection and
explicit stop and cancellation before the first action. The shared host-dispatcher
fixture also checks DTR loss and collection after reconnection. Native simulation
checks cancellation between grouped driver writes and preservation of output
ownership. These checks do not establish physical motion, BLE timing or electrical
synchronization.

On one battery-powered Robot Inventor F4 (PCB revision unknown), BLE schedules
drive two suspended ID-75 motors on B/F through duty changes, reversal, brake
and coast. A 1.8-second experiment retains all 36 observations at 50 ms intervals,
including encoder motion and both members' applied results. Explicit stop and
actual BLE disconnect cancel active drive, release both ports and retain complete
binary records for collection after reconnection. These checks have no missed
samples or cleanup errors. Separate 350 ms pulses with USB supplying power
verify both directions and automatic deadline expiry.

In the disconnect check, a 20% command with a four-second limit coasts after
about 2.49 seconds, before that limit. Closing the host BLE connection itself
takes 2.17–2.69 seconds across the bounded disconnect checks.
Link-loss handling is therefore not an instantaneous stop command. Keep finite
output deadlines; use `capture stop` when the connection is available. These
bounded checks establish functional actuation and cleanup, not electrical edge
alignment, sensor accuracy or timing under sustained combined load.

### Command files

Save ordinary Shell commands in a UTF-8 text file and execute it before keyboard
input begins:

```sh
uv run brickphyrdev terminal --commands experiment.txt --record run.cbor
```

For example, this passive experiment does not energize outputs:

```text
# Period and duration are in milliseconds.
capture prepare 20 1000 imu encoder.A encoder.B
capture check
capture start
```

The file may contain the same `capture at` commands as an interactive schedule.
Timing belongs in the hub's schedule, not in host sleeps between file lines.
Blank lines and whole-line `#` comments are ignored; LF and CRLF are accepted.
Each remaining line goes unchanged to Zephyr's standard SMP Shell handler, using
the same parser, command handlers and length limit as interactive Shell. The
client checks the real command return code before sending another line. An error
reports the file and line and closes the session; an ambiguous transport failure
does not retry the command. Closing the connection cancels diagnostic drive under
the existing schedule policy, while ordinary autonomous applications keep running.

The file is validated as text before connecting. Commands themselves are
validated by the hub when executed; the file is not a transaction and successful
earlier commands are not undone. `--repl` cannot be combined with command files.
`Ctrl+]` remains available to detach during execution; queued keyboard input is
sent after the file finishes. `--record-dir` can replace `--record` for subsequent
interactive experiments on the same connection.

The standard SMP response has bounded text output. `[output truncated]` means
the command's output was incomplete, not that it failed; the actual return code
still controls continuation. Inspect long listings interactively. The execution
context uses the existing management caller's stack, with no additional Shell
thread, stack or history. It does not overwrite a partially edited interactive
command. ARM/QEMU checks cover file errors, output truncation, preserved input,
capture collection and successive experiments through the real CLI and Shell;
a bounded Inventor F4 check also executes a passive command file and collects two
experiments in separate files through one BLE terminal session, preserving command
input and explicit buffer cleanup. This workflow passes with USB power and with
USB disconnected on battery power; the latter check also restores terminal state
on exit and validates both complete binary files. Standard SMP Shell commands
also work over native USB there. Energized schedule timing remains unverified.

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

The combined client has one connection owner. USB uses standard serial SMP
framing inside the console stream. BLE uses the standard binary SMP GATT
characteristic alongside the native Shell characteristics on that same link;
machine responses never pass through the terminal's text channel. A cancelled
or timed-out exchange ends the connection so a late response cannot belong to
another request.

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
acquisition or service update as specified for each source; repeated reads preserve
it. External sensor `sample_ms` marks the last accepted measurement, including
mode settling. `applied_ms` belongs to the last
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

One request reads one source; the client combines several responses for a display.
Builds with motor regulation default to 512-byte SMP buffers because full source
discovery exceeds 384 bytes. The client negotiates the server's capacity. Motor
output and controller details remain separate bounded observations of the same
service state. No separate telemetry wire
format, connection owner or Bluetooth service is introduced.

| Source | Values and units |
| --- | --- |
| `power` | `sample_ms`, `source` (0 battery, 1 USB, negative errno unknown), `idle_since_ms`, `battery_idle_s`, `usb_idle_s`, `connected`, `application`, `shutdown` |
| `buttons.center` | `center`; resistor-ladder boards also supply `sample_ms`, `raw` (ADC counts), `failures` and `stable` |
| `buttons.side` | `sample_ms`, `raw` (ADC counts), `failures`, `stable`, `left`, `right`, `bluetooth` |
| `application` | `state`, `result`, `fault`, `cleanup`, `image`, `heap`, `loaded`, `stack`, `unused`, `dropped`, `queued`, `received`, `total`, `ready` |
| `matrix` | `applied_ms`, `failures`, `owner`, `pending`, `valid`, `requested`, `applied` |
| `imu` | `sample_us`, `completed_us`, `sequence`, `missed`, `failures`, `ax_um_s2`, `ay_um_s2`, `az_um_s2`, `gx_mdps`, `gy_mdps`, `gz_mdps` |
| `imu.calibrated` | `sample_us`, `sequence`, `epoch`, `ready`, `stationary`, calibrated `ax_um_s2`, `ay_um_s2`, `az_um_s2`, `gx_mdps`, `gy_mdps`, `gz_mdps`, current bias `bx_mdps`, `by_mdps`, `bz_mdps` |
| `imu.orientation` | `sample_us`, `sequence`, `epoch`, `ready`, rotation matrix `m11`…`m33` multiplied by 1000000, `heading1_mdeg`, `heading3_mdeg` |
| `sound` | `changed_ms`, `owner`, `phase`, `mode`, `requested_hz`, `actual_hz`, `queued`, `completed`, `frequency`, `duration_ms`, `volume`, `eof`, `underruns`, `failures`, `cleanup` |
| `battery` | `sample_ms`, `completed_ms`, `sequence`, `failures`, `voltage_mv`, `current_ma`, `voltage_raw`, `current_raw`, `type`, `temperature_mdegc`, `temperature_error`, `temperature_ms`, `temperature_failures`, `temperature_raw` |
| `port.A` … `port.F` | `sample_ms`, `changed_ms`, `sequence`, `generation`, `failures`, `phase`, `type_id`, `cleanup`, `supply`; passive `pressed` or UART format, timestamps and raw `data` as defined by [external ports](ports.md) |
| `sensor.A` … `sensor.F` | `sample_ms`, `sequence`, `generation`, `type_id`, `mode`, `measurement`, `available`, `owner`, `fields`; applicable distance, light, HSV, force, displacement, tilt, count or presence values in [sensor units](ports.md#sensor-measurements) |
| `motor.A` … `motor.F` | `requested_ms`, `applied_ms`, `deadline_ms`, `generation`, `request`, `completed`, `mode`, `duty_permille`, `requested_mode`, `requested_duty`, `owner`, `pending`, `releasing`, `valid`, `cleanup`, `failures`; [drive and ownership semantics](ports.md#direct-motor-drive) |
| `control.A` … `control.F` | `sample_ms`, `generation`, `completed`, `active`, `done`, `stalled`, `frames`, `overruns`, `reference_mdeg`, `reference_mdps`, `estimated_mdps`, `requested_mv`, `applied_mv`; [regulation semantics](ports.md#speed-and-position-control) |

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
Heading conversion rounds to the nearest integer, with ties away from zero,
without first rounding the scaled value to `float32`. An unrepresentable heading
is omitted and reports `-ERANGE` unless the snapshot already has an error.
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
Battery `type` is 0 unknown, 1 alkaline or 2 Li-ion. The top-level error applies
to voltage/current; inspect `temperature_error` separately before using the
signed temperature. `temperature_ms` is its last valid acquisition timestamp.
Chemistry is a board property and remains available when acquisition fails.

Native simulation checks Shell/SMP reads, matrix payloads, unsupported sources,
write rejection and acquisition timestamps. Host tests use the real Shell/SMP
peer with fragmented, explicitly synthetic BLE delivery; they cover simultaneous
editing and management, view switching, TTY restoration, JSONL output and slow
consumers. ARM QEMU checks read-only native
observation, including busy cleanup and preserved output/allocation. These checks
do not establish physical sensor accuracy or radio timing.
