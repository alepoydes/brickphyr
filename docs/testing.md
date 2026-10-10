# Validation

The short bring-up checks exercise the compiled console in Zephyr's native
simulator and inspect the linked ARM image. There are no hardware results yet.
Simulation does not exercise the STM32 USB driver, power circuitry, or bootloaders.

## Run the bring-up checks

After [workspace setup](builds.md), run from this repository:

```sh
uv run --locked west build -b spike_prime_f4 -d build/prime &&
ZEPHYR_TOOLCHAIN_VARIANT=host uv run --locked west build -b native_sim/native/64 -d build/native -- -DEXTRA_CONF_FILE=tests/native_sim.conf &&
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

The image test uses [the image checker](../tools/check_image.py) on the built ELF
and binary, then checks rejection of a reset into the preserved loader, a truncated
image, and corrupted boot metadata. The checked constants are the external Prime
F4 boot contract, independent of the linker configuration. No test reads source
text. Both builds must succeed before running tests on their artifacts; the
sequence above stops on a build failure. Missing artifacts cause failure rather
than a skip.

For interactive local use, run `build/native/zephyr/zephyr.exe`, then open its
printed pseudoterminal path in another terminal:

```sh
uv run --locked python -m serial.tools.miniterm /dev/pts/N 115200
```

Replace `/dev/pts/N` with the printed path. Exit miniterm with Ctrl+], and stop
the simulator with Ctrl+C. The `reboot` command creates a new pseudoterminal;
reopen the newly printed path. This is a host simulation with simulated time
paced against the host clock, not a hub emulator.

## Local checks

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
