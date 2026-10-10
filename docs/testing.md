# Validation

The short bring-up checks exercise the compiled console in Zephyr's native
simulator, inspect the linked ARM image, and open its firmware package through
Pybricks. Physical checks and their limits are listed in [hardware](hardware.md).
Simulation does not exercise the STM32 USB driver, power circuitry, or bootloaders.

The shared terminal checks run the same editing, completion, history and command
sequence through USB and BLE byte adapters. The command observes the same Shell
instance on both paths. Additional checks cover stale input/output, USB rejection
during BLE ownership, and disconnect during an accepted command. These are native
simulation checks, not evidence of Bluetooth radio behavior.

The Python protocol checks also run `brickphyrdev` USB identity and handover through
the installed pybricksdev library against the production C handler over a PTY.
USB enumeration/control is simulated. Packaging can be checked with `uv build`;
the resulting wheel contains the CLI, not firmware sources or development reports.

## Run the bring-up checks

After [workspace setup](builds.md), including the experimental
[packaging prerequisite](builds.md#experimental-firmware-package), run from this repository:

```sh
uv run --locked west build -b spike_prime_f4 -d build/prime &&
ZEPHYR_TOOLCHAIN_VARIANT=host uv run --locked west build -b native_sim/native/64 -d build/native -- -DEXTRA_CONF_FILE=tests/native_sim.conf &&
ZEPHYR_TOOLCHAIN_VARIANT=host uv run --locked west build -b native_sim/native/64 -s tests/hub -d build/hub-tests &&
ZEPHYR_TOOLCHAIN_VARIANT=host uv run --locked west build -b native_sim/native/64 -s tests/host -d build/host-tests &&
ZEPHYR_TOOLCHAIN_VARIANT=host uv run --locked west build -b native_sim/native/64 -s tests/radio -d build/radio-tests &&
ZEPHYR_TOOLCHAIN_VARIANT=host uv run --locked west build -b native_sim/native/64 -s tests/storage -d build/storage-tests &&
ZEPHYR_TOOLCHAIN_VARIANT=host uv run --locked west build -b native_sim/native/64 -s tests/management -d build/management-tests &&
uv run --locked pytest -q
```

The simulator compiles the same application and Zephyr shell. The test opens its
pseudoterminal with pyserial and checks command echo, help/identity, rejected
arguments, and an unknown command. It closes the terminal for 200 ms, reopens it,
and checks that uptime continued to advance. After a reboot it connects to the
new pseudoterminal and checks reset uptime and identity. Response failures have
bounded timeouts; serial links close and the simulator process terminates on exit.
This exercises terminal disconnection in the host serial driver, not STM32 USB
disconnection or battery power retention.

The same console test sends actual editing keys: both common Backspace codes,
cursor movement, Home/End, Delete, Tab, line clearing, and Up/Down history. It
checks the resulting command output rather than matching escape sequences.

The image test uses [the image checker](../tools/check_image.py) on the built ELF
and binary, then checks rejection of a reset into the preserved loader, a truncated
image, and corrupted boot metadata. The checked constants are the external Prime
F4 boot contract, independent of the linker configuration. No test reads source
text. Both builds must succeed before running tests on their artifacts; the
sequence above stops on a build failure. Missing artifacts cause failure rather
than a skip.

The package test checks an actual compiled F4 image through the Pybricks package
reader and rejects a corrupted image without replacing a previous package. It
does not open a USB connection. Focused tests for the dependency extension run
in its owning repository through the workspace environment:

```sh
uv run --locked --group flash python -m unittest discover -s ../pybricks-micropython/tests/tools
```

Those tests cover unchanged name customization, checksum generation without a
custom name, and both standard F4 installation paths. The unchanged writer must
leave the factory region untouched and commit the main image's initial stack
word last. The writer tests substitute the DFU-device boundary; their erase/write
sequence is simulated and is not evidence of a physical flash operation.

For interactive local use, run `build/native/zephyr/zephyr.exe`, then open its
printed pseudoterminal path in another terminal:

```sh
uv run --locked python -m serial.tools.miniterm --raw /dev/pts/N 115200
```

Replace `/dev/pts/N` with the printed path. Exit miniterm with Ctrl+], and stop
the simulator with Ctrl+C. The `reboot` command creates a new pseudoterminal;
reopen the newly printed path. This is a host simulation with simulated time
paced against the host clock, not a hub emulator.

## Hub input and lighting

The short `tests/hub` Zephyr test uses the production input/LED drivers and
Pybricks helpers over Zephyr's ADC, SPI, GPIO emulators and a fake PWM timer.
It uses the F4 button configuration and exercises the observed 3141–3147 center
voltage range, all resistor-ladder button combinations, invalid voltage and ADC
failure release, TLC5955 initialization, RGB channel ordering, invalid LED
requests, SPI errors, and preservation of the charger-disable channel. These
checks use synthetic inputs; they do not verify electrical behavior or a bootloader.

The same SPI path checks the readiness animation with synthetic time: startup
fade, visible steady levels, continuous small fluctuations, rare bounded surges,
and cyan/yellow channel exclusion. Green keeps increasing after red saturates.
It also checks menu lighting, simultaneous central LED updates, skipped late
frames, persistent manual central color, independent Bluetooth color, and prevention of relighting after shutdown. RGB checks cover
board gains, gamma response, and exact raw PWM including zero and full duty.
These checks establish output behavior, not perceived color or visual quality.
The menu checks exercise entry, preview, cancellation, release confirmation, and
retention after rejected selection through the production transition handler.

On F4, visually check the fade-in and cyan/yellow reactor flares, each button and
combinations with `buttons`, and both lighting commands. Confirm that setting the
central color stops animation, while a Bluetooth color command does not interrupt
it. Open the menu, preview both palettes, cancel, then select the other interface;
interface/menu changes restore automatic lighting. Check busy rejection with a
terminal open. Then check `shutdown` and the two-second center hold
with USB and with battery power separately. Check `reboot dfu`, a standard
Pybricks reinstall, and the returning console. Keep independent factory DFU entry
available throughout; follow [recovery](recovery.md). Do not infer power removal
from the disappearance of a serial port alone.

## Local checks

`tests/management` runs the production Shell/SMP transport and LittleFS/Settings
stack behind a native UART pseudoterminal. Unmodified `smpclient` and `smpmgr`
exercise negotiation, multi-line echo, RAM changes, explicit save, invalid values,
Shell reads, and persistence across process restart. The fixture waits for the
Shell prompt before opening a client and supplies a permanent management session
because the native UART has no USB DTR. The separate `tests/host` UART-emulator
checks exercise actual session transitions, including disconnect during an
accepted SMP operation and isolation of incomplete commands and old responses.

`tests/storage` runs the actual LittleFS/Settings stack over simulated flash.
It checks explicit save versus RAM-only changes, reload, file compaction,
incompatible values and truncated records. A second boot starts with foreign
contents and checks that rejected storage remains byte-for-byte unchanged.
The backing files are temporary; no test opens a physical hub.

The host simulation also exercises runtime power settings through the production
Shell/Settings path: rejected values preserve state, source timers are independent,
zero disables expiry, activity restarts the deadline, unknown source and an active
USB owner inhibit expiry, and an expiry claim excludes later ownership. Time is
synthetic; these checks do not prove physical power removal or VBUS wiring.

Use short upstream tests and focused Brickphyr behavior tests in the default
check set. Suitable cases include format/ABI rejection, loader failure cleanup,
buffer lifetime, resource ownership, state transitions, bounded queues, timing
arithmetic, and device-protocol boundaries. Test cancellation and secondary errors
through actual entry paths, not only helper functions.

Use Zephyr's existing test tooling and simulation facilities where applicable.
Host simulation can check a state machine; it cannot demonstrate MCU instruction
timing, peripheral DMA, radio behavior, or physical stopping. A scripted clock must
be shared by all participants, not independently incremented by each task's wait.

Numerical code reused from Pybricks should retain its meaningful upstream tests.
Validate finite precision and boundary cases when a port changes representation,
units, compiler options, or arithmetic. Test the actual optimized path against an
independent reference; do not validate a dead scalar fallback instead.

## Minimal hardware fixture

Use an identified hub attached over USB with a known set of motors and sensors.
Secure or raise driven parts for motion tests and record wiring/directions. The
fixture must match the behavior being checked.

Bring up small applications, each proving a specific contract:

| Check | Evidence it must collect |
| --- | --- |
| Boot, console, identity | Correct revision identification and recovery entry |
| Sensors | Units, validity, sample times, quantization/filter behavior |
| Motors | Bounded commands, encoder response, cancellation and confirmed stop |
| App lifecycle | Repeated load/start/stop/unload; resource and heap recovery |
| Isolation | Invalid access ends the application while system services remain usable |
| Input and communication | Disconnect/reconnect, cancellation, negotiated payload limits, slow receiver |
| Combined load | Sensor/actuator latency under USB/BLE/input traffic and runtime activity |
| Restoration | The supported transitions and interrupted transfer in [recovery](recovery.md) |

Zero-output tests must gate actuation at the device boundary, including servo and
position commands. A test that sends only `dc(0)` is insufficient if another code
path can invoke a position servo. Do not read or overwrite user calibration/model
storage as a side effect of timing diagnostics.

Reuse one host connection owner for a diagnostic session. Confirm STOP before
replacement. Keeping a host connected may prevent an existing firmware's idle
shutdown; charging alone and an off hub are different cases. Recheck the actual
transport rather than relying on a product name or an old process handle.

Inspect interface descriptors and select a compatible public transport. Product
names alone do not distinguish USB CDC ACM from vendor-specific USB interfaces.

## Performance evidence

Measure an uninstrumented baseline and the observer's own cost. Account for input,
acquisition, computation, command issue, recording, transport continuations,
interrupts, scheduling, and waiting. Include background display work; stop animated
spinners when measuring the application's workload and state what remains active.

Record distributions and maxima for the tested workload, not only means. Report
read-to-command age separately from period and wake-up lateness. Test complete
episodes so that allocations, queues, and resource retention become visible.
Heap scans and collection can themselves take time; separate their measurement
from ordinary operation and include their cost when they really execute there.

Do not claim an observed maximum is a mathematical worst-case bound. Compare the
whole intended workload, including combined radio/controller load, with its
deadline. A longer delay or a disabled guard is not an optimization.

## Scope of test results

Every report identifies commit/dependency versions, board/revision, configuration,
toolchain, attached devices, workload, instrumentation, commands, and observed
result. Separate source inspection, build success, simulation, and hardware data.
List untested parts instead of promoting a narrow result into platform support.

Keep technical reports, session notes, and experiment results in ignored
`reports/`; generated builds and captures also remain outside Git. Public documents
describe support, reproducible procedures, and limitations. Small curated regression
fixtures may be versioned with their source and interpretation. Long stress, soak, and
robot mechanics tests run explicitly rather than slowing every documentation edit.

Python compatibility programs compare Brickphyr with the selected Pybricks
reference. Native/WASM examples exercise the released hardware interface. An
application such as babybot validates a useful integration, but balancing success
alone cannot establish recovery, isolation, or compatibility on other hubs.

## USB interface selection

`tests/host` runs the production dispatcher and Zephyr Shell through the UART
emulator. It checks Shell → Pybricks → Shell without reboot, real editing/history,
unknown selections, occupied-session rejection, output draining and partial-line
isolation, including rapid reopen. This does not exercise a physical USB driver.

The Python protocol checks compile the production C handler with the selected
Pybricks sources and exchange frames using the unchanged upstream Python client.
They check real identification, zero application capabilities, tagged errors,
subscription status and malformed/oversized frame recovery. A host C compiler is
required. No check inspects source contents.

The host test first acquires BLE ownership without preceding USB status requests.
It also drives the production BLE ownership entry points: USB/BLE
exclusion, power inhibition, rejected USB input and fresh-open requirements. This
simulates ownership transitions, not radio traffic. The optional Bluetooth profile
is checked on the Inventor F4 with pybricksdev discovery, identity, status,
unsupported-command rejection, reconnection and exit to Shell. Both directions
of USB/BLE exclusion are exercised with actual host connections. A synthetic slow
SMP operation checks admission, held ownership/power during disconnect, completion
and rejection of a queued stale packet through the production processor. On BLE,
standard SMP clients exercise fragmented echo, RAM changes versus saved values,
incomplete-packet isolation and inhibition of a one-second USB-power timeout; see
[Bluetooth usage](bluetooth.md#experimental-f4-controller).

`tests/radio` exercises the production radio workqueue and Shell commands with
synthetic Bluetooth lifecycle operations. It checks that stalled initialization
leaves status and the system queue responsive, off during startup remains off,
recycled connection notifications cannot undo off, repeated commands are
idempotent, and the first error prevents reinitialization until reboot. This does
not simulate RF or the HCI controller. Run `build/radio-tests/zephyr/zephyr.exe`;
its clock is accelerated by default. On the Inventor F4, automatic boot discovery,
Pybricks identification, subsequent SMP connection and repeated on/off are checked
without opening USB before the first BLE connection.

On an authorized F4 hub, follow the [USB round trip](host.md#usage). Also attempt
selection while the terminal is open, close it with a partial command, and verify
that the partial command never executes after reopening. Close all clients at
handoff. USB-driver access, physical packet completion and unchanged uptime must
be measured separately from simulation.
