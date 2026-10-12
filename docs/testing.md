# Validation

The short bring-up checks exercise the compiled console in Zephyr's native
simulator, inspect the linked ARM image, and open its firmware package through
Pybricks. Physical checks and their limits are listed in [hardware](hardware.md).
Simulation does not exercise the STM32 USB driver, power circuitry, or bootloaders.

## Watchdog

The watchdog checks use production supervision and Zephyr Task Watchdog, with a
synthetic watchdog device and process exit at the board-reset boundary. They
check healthy progress followed by a stalled hub loop or system queue. The power
suite runs the production off loop and ADC input with watchdog enabled, including
held-button, invalid-sample and fresh-press cases.

```sh
ZEPHYR_TOOLCHAIN_VARIANT=host uv run --locked west build -b native_sim/native/64 -s tests/watchdog -d build/watchdog-tests
build/watchdog-tests/zephyr/zephyr.exe
ZEPHYR_TOOLCHAIN_VARIANT=host uv run --locked west build -b native_sim/native/64 -s tests/watchdog -d build/watchdog-work-tests -- -DSTALL_WORKQUEUE=ON
build/watchdog-work-tests/zephyr/zephyr.exe
ZEPHYR_TOOLCHAIN_VARIANT=host uv run --locked west build -b native_sim/native/64 -s tests/poweroff -d build/poweroff-tests
build/poweroff-tests/zephyr/zephyr.exe
```

On explicitly authorized hardware, a separate diagnostic build may enable
`CONFIG_BRICKPHYR_WATCHDOG_FIXTURE`. After verifying `watchdog` reports armed,
confirm no running application and inactive motor outputs. `watchdog_test service`
stalls the system workqueue once; `watchdog_test hardware` also locks interrupts.
Both commands intentionally lose the connection. Check USB recovery, reset cause,
idle application state and inactive outputs before another operation. Port
discovery may still be pending immediately after reboot; record that state instead
of treating it as a successful measurement. Neither diagnostic repeats at boot.
The normal build contains neither command.

The [power document](settings.md#system-watchdog) records physical coverage and
the distinction between hardware reset, software reset and still-pending power
checks. Watchdog-enabled builds require the [Zephyr fixes](upstream.md#watchdog-prerequisites).

## Python core

These checks run the production VM with captured output and a caller-owned heap.
They cover source, bytecode from the matching compiler and pybricksdev, GC,
exceptions, fresh state, memory exhaustion and every incomplete prefix of a
compiled fixture. ARM checks execute in Zephyr user mode with separate VM state
and test-buffer partitions. They do not exercise upload, storage, REPL, Pybricks
bindings or physical hardware.

```sh
ZEPHYR_TOOLCHAIN_VARIANT=host uv run --locked west build -p always -b native_sim/native/64 -s tests/python -d build/python-tests
build/python-tests/zephyr/zephyr.exe
uv run --locked west build -p always -b mps2/an385 -s tests/python -d build/python-arm-tests -- -DEXTRA_CONF_FILE=arm.conf
uv run --locked pytest -q tests/test_native.py -k python
```

The build initializes no dependency automatically: populate the manifest's
MicroPython submodule first. The `mpy_cross` build target uses that same source
revision. Use a pristine build when changing MicroPython feature selection;
its current upstream registration generator can otherwise retain disabled
modules. Compiler checks require local subprocesses and `asyncio` socket pairs.

### Python application integration

The ARM SMP fixture runs the production supervisor, protected VM, upload handlers
and host CLI through QEMU UART/PTY. These checks cover errors, memory/stack limits,
output overflow, maximum source length, forced stop and repeated Python/native
transitions after an MPU fault. The UART fixture has no physical USB/BLE detach
signal; it checks client reconnection, not radio or USB session cleanup.

```sh
uv run --locked west build -b mps2/an385 -s tests/native_smp -d build/python-smp-tests -- -DCONFIG_BRICKPHYR_PYTHON=y
uv run --locked pytest -q tests/test_python.py tests/test_native_mpy.py tests/test_python_files.py tests/test_python_io.py tests/test_python_repl.py tests/test_python_modules.py tests/test_pybricks_tools.py tests/test_python_hub.py
```

The same build produces native `.mpy` fixtures in `python-fixtures` using the
matching compiler and upstream `dynruntime.mk` with the `armv7m` target.
`test_native_mpy.py` checks native/Viper calculations, dynamic C initialization
and GC, code/data/kernel memory protection, forced stop, code exhaustion, malformed
input and another successful run after each failure. These are protected ARM
checks, not physical fault injection.

`test_python_hub.py` exercises the Pybricks hub bindings through that same
protected engine. Synthetic button/ADC inputs and an observed SPI peer check
button sets, battery units, static drawing, rotation, color channels, ownership,
I/O failures and cleanup on return, exception, forced stop and REPL reset.
Light sequences are copied before caller mutation/GC, invalid replacements
preserve the preceding pattern, and both lights animate during a non-yielding
Python loop. Static and animated cleanup use the same production resource owner.
These checks do not establish physical display or light appearance. NaN and
malformed images must fail without altering the display.
The native ARM fixture also supplies invalid RGB/animation pointers and misaligned
animation/snapshot buffers through the SDK. Each must end only the application,
restore its claimed light, recover its allocation and leave the next application usable.

`test_python_files.py` exercises source/bytecode/native imports, relative packages,
GC and recovery from a truncated native import. It also covers malformed maps,
namespace escape, duplicate paths, atomic replacement, removal during repeated
imports and directory-based CLI runs through the actual application store. An
explicit entry is checked with an ordinary module named `main` and all 64 files;
invalid or duplicate entry metadata is rejected. The
existing native storage tests cover LittleFS capacity failures through the shared
publication path.

`test_python_io.py` covers standard text/binary streams, readline editing,
input polling, bounded backpressure, catchable interrupts during execution and
blocked input, and a subsequent application. Resource cases check reads, seek,
close, GC, namespace boundaries and rejected writes. The same production SMP
handlers are used by the installed input CLI. These are ARM user-mode checks;
QEMU UART reconnection does not establish physical USB/BLE detach behavior.

`test_python_modules.py` runs selected upstream cases unchanged through the
protected application path. It compares portable cases with CPython and uses
the dependency's expected output for MicroPython-only allocation locks. Cases
exercise buffers, reverse operators, binary structures, IOBase, GC, JSON and
random ranges. A focused diagnostic case checks numeric tolerances, exception
formatting, memory information and module help. No source-text assertions or
second implementation of these modules is used.

`test_pybricks_tools.py` runs the dependency's color, multitasking and COBS cases
unchanged, including its local cancellation regression. Additional cases check
geometry, GC roots, fresh library state after forced stop, stopwatch semantics,
concurrent waits, input and interruption. Time assertions allow scheduling delay;
the upstream virtual-hub wait test's exact synthetic-clock assumptions are not
used as physical timing requirements. These checks do not access a hub.

`test_python_repl.py` uses the maintained REPL through the same protected VM and
SMP handlers. It covers editing/history/completion, exceptions, blocked input,
busy-loop interruption, native allocation cleanup on soft reset, friendly/raw
mode switching, busy admission and reattachment. A synthetic disconnect
notification checks unfinished-line cancellation without resetting globals.
Real CLI/PTY cases check monitoring within that connection, detach, forced exit
under input backpressure, complete long output before/after soft reset, final
output draining, return to Shell and TTY restoration. SMP stream checks hold a
producer behind a full output queue, then drain, interrupt, stop or detach it.
A REPL-to-script case checks independent newline state using the
[MicroPython reset correction](upstream.md#python-runtime-prerequisites).
These checks do not establish physical BLE delivery or radio timing.

The plain Shell variant uses the production host dispatcher instead of Zephyr's
serial Shell backend. It requires the local
[CMSDK UART correction](upstream.md#cmsdk-uart-test-prerequisite). Its synthetic
DTR notification exercises real teardown and reattachment; UART device closure
alone does not provide that notification in QEMU.

```sh
uv run --locked west build -b mps2/an385 -s ../zephyr/tests/drivers/uart/cmsdk_apb -d build/uart-cmsdk-tests
uv run --locked pytest -q tests/test_native.py -k uart-cmsdk
uv run --locked west build -b mps2/an385 -s tests/native_smp -d build/python-host-tests -- -DCONFIG_BRICKPHYR_PYTHON=y -DEXTRA_CONF_FILE=host.conf
uv run --locked pytest -q tests/test_python_shell.py
```

These cases cover cold Shell entry, early Ctrl-C, binary output, soft reset,
return without replaying queued keystrokes, state retention across a session
boundary and rejection of raw REPL entry on an active SMP connection. The actual
pybricksdev serial client also identifies and reconnects through this dispatcher;
that case detects drivers invoking TX callbacks synchronously under a caller's
lock. Program transfer and execution use the separate cases below.

### Pybricks execution adapter

`test_pybricks_execution.py` uses the same dispatcher build and the actual
pybricksdev serial client/compiler. It checks multi-module uploads, a `main`
module separate from the entry, stdin/stdout, running status, busy-loop stop,
repeat/reconnect and recovery after invalid module records. Transport framing
and malformed-file checks use the production handlers, not a second parser.
The long-output case checks a complete 4 KiB line through the client's ordinary
run/output path, exceeding the application's bounded queue.
The native case checks capability-based ABI selection, client-compiled native/Viper
code, precompiled imports with a transitive dependency, a dynamic C module and GC.
A mismatched native header fails before download; the installed program still runs.
The client also compiles and executes the reference tools cases and the public
concurrent-timer example through the same protocol, including repeated fresh VMs.
The required local client changes are listed in
[upstream integration](upstream.md#python-client-prerequisites).

```sh
uv run --locked pytest -q tests/test_pybricks_execution.py
uv run --locked west build -b mps2/an385 -s tests/python_ble -d build/python-ble-tests
uv run --locked pytest -q tests/test_native.py -k python-ble
uv run --locked west build -b mps2/an385 -s tests/python_ble -d build/python-slots-tests -- \
  '-DEXTRA_CONF_FILE=../native_smp/storage.conf;slots.conf' \
  '-DDTC_OVERLAY_FILE=app.overlay;../native_smp/storage.overlay'
uv run --locked pytest -q tests/test_native.py -k python-slots
```

The BLE fixture uses the real GATT adapter, common host owner and protected
interpreter with synthetic controller calls and notifications at ATT MTU 23.
The native Shell REPL checks complete long output and explicit exit while
notifications are blocked. Standard SMP BLE requests also exercise the real
Zephyr transport and application handlers: status polling preserves an attached
output consumer and interruption remains available while its queue is full.
It checks input across disconnect, retained and incomplete uploads, repeated
execution, native/Viper execution with GC, busy-loop stop and lossless notification retry after transient
resource exhaustion. With output blocked it checks forced stop, catchable
interruption and autonomous completion after disconnect or native-profile
selection. An emulated UART also checks STOP through the real USB dispatcher
with its transmit FIFO unread. The normal lifecycle poll runs independently of
status reads; UART0 carries test results. This does not emulate USB hardware or
the complete HCI/ATT receive stack and radio.

The slot variant uses that same fixture with the existing 64 KiB simulated
LittleFS store. It checks multiple slots, selected-slot admission, interrupted
and malformed replacement, reserved-name protection, and start across BLE/USB.
A compiler-produced 24 KiB constant fills storage until a replacement fails;
the previous program and an ordinary named program must still run. These are
synthetic storage checks, not a physical power-cycle claim. The actual serial
client also checks selection through the hub Shell and repeat after rejected
replacement. The RAM-only fixture remains a separate configuration.
The storage worker retains 1808 bytes of unused stack in this ARM fixture after
the capacity checks. This guides its 4 KiB budget; real SPI flash response latency,
Bluetooth receive behavior during synchronous completion and F4 stack high-water
checks remain separate acceptance work.

Python builds give the Bluetooth RX thread 2 KiB. The fixture's callback path
uses about 1 KiB; the audited F4 compiler's HCI/L2CAP/ATT frames add 696 bytes.
Package normalization and admission run sequentially, and Python launch does
not reserve the ELF loader's buffer. These measurements guide the stack budget;
physical high-water checks under combined device load remain required.

On a named F4 running the Python profile, separately check source and client
bytecode through both USB and BLE, then a bounded non-yielding program followed by
explicit stop and another successful run. Observe application power holds and
incomplete-upload cleanup during cross-transport reconnection. Keep motor outputs
inactive; these calculations need no actuator operation. The watchdog recovery
gate applies before such unattended execution. Leave no running diagnostic at
handoff and distinguish these bounded checks from a long-duration workload.

## Existing subsystem checks

The shared terminal checks run the same editing, completion, history and command
sequence through USB and BLE byte adapters. The command observes the same Shell
instance on both paths. Additional checks cover stale input/output, USB rejection
during BLE ownership, and disconnect during an accepted command. These are native
simulation checks, not evidence of Bluetooth radio behavior.

The IMU fixtures use the production acquisition thread and pinned ST/Pybricks
register code with a synthetic I²C/GPIO device. Checks cover cold configuration,
physical units and signed limits, read-only Shell snapshots, stale timestamps,
I/O failure recovery, overlapping updates and shutdown. A separate startup-failure
fixture checks that a failed register read cannot cause a write and that cleanup
does not replace the first error. These do not measure F4 sensor accuracy or timing.

`tests/imu_processing` checks the [ported calculation](../src/imu/README.md) on
ARM QEMU: six gravity poses, known turns, user axes, calibration, rejected settings,
stationary bias learning and restart after missing measurements. Synthetic input
sequences check the mathematics, not sensor accuracy. The acquisition fixture
also exercises processing, Settings and explicit persistence through actual
I²C/GPIO emulation and the LittleFS backend on simulated flash. It calls the real
SMP observation handler with the transport's payload capacity, including a build
where telemetry alone supplies the required CBOR nesting configuration.

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
ZEPHYR_TOOLCHAIN_VARIANT=host uv run --locked west build -b native_sim/native/64 -s tests/imu -d build/imu-tests &&
ZEPHYR_TOOLCHAIN_VARIANT=host uv run --locked west build -b native_sim/native/64 -s tests/imu -d build/imu-failure-tests -- -DIMU_INIT_FAILURE=ON &&
ZEPHYR_TOOLCHAIN_VARIANT=host uv run --locked west build -b native_sim/native/64 -s tests/poweroff -d build/poweroff-tests &&
ZEPHYR_TOOLCHAIN_VARIANT=host uv run --locked west build -b native_sim/native/64 -s tests/sound -d build/sound-tests &&
ZEPHYR_TOOLCHAIN_VARIANT=host uv run --locked west build -b native_sim/native/64 -s tests/battery -d build/battery-tests &&
ZEPHYR_TOOLCHAIN_VARIANT=host uv run --locked west build -b native_sim/native/64 -s tests/ports -d build/ports-tests &&
ZEPHYR_TOOLCHAIN_VARIANT=host uv run --locked west build -b native_sim/native/64 -s tests/lump -d build/lump-tests &&
ZEPHYR_TOOLCHAIN_VARIANT=host uv run --locked west build -b native_sim/native/64 -s tests/motor_output -d build/motor-output-tests &&
ZEPHYR_TOOLCHAIN_VARIANT=host uv run --locked west build -b native_sim/native/64 -s tests/motors -d build/motor-tests &&
ZEPHYR_TOOLCHAIN_VARIANT=host uv run --locked west build -b native_sim/native/64 -s tests/regulated_motors -d build/regulated-motor-tests &&
ZEPHYR_TOOLCHAIN_VARIANT=host uv run --locked west build -b native_sim/native/64 -s tests/host -d build/host-tests &&
ZEPHYR_TOOLCHAIN_VARIANT=host uv run --locked west build -b native_sim/native/64 -s tests/radio -d build/radio-tests &&
ZEPHYR_TOOLCHAIN_VARIANT=host uv run --locked west build -b native_sim/native/64 -s tests/storage -d build/storage-tests &&
ZEPHYR_TOOLCHAIN_VARIANT=host uv run --locked west build -b native_sim/native/64 -s tests/management -d build/management-tests &&
uv run --locked west build -b mps2/an385 -s tests/native -d build/native-app-tests -- -DDTC_OVERLAY_FILE=app.overlay &&
uv run --locked west build -b mps2/an385 -s tests/imu_processing -d build/imu-processing-tests &&
uv run --locked west build -b mps2/an385 -s tests/motor_control -d build/motor-control-arm-tests &&
uv run --locked west build -b mps2/an385 -s tests/python -d build/python-arm-tests -- -DEXTRA_CONF_FILE=arm.conf &&
uv run --locked west build -b mps2/an385 -s tests/native_smp -d build/python-smp-tests -- -DCONFIG_BRICKPHYR_PYTHON=y &&
uv run --locked west build -b mps2/an385 -s tests/native_smp -d build/python-host-tests -- -DCONFIG_BRICKPHYR_PYTHON=y -DEXTRA_CONF_FILE=host.conf &&
uv run --locked west build -b mps2/an385 -s ../zephyr/tests/drivers/uart/cmsdk_apb -d build/uart-cmsdk-tests &&
uv run --locked west build -b mps2/an385 -s tests/native_smp -d build/native-smp-tests -t llext-edk &&
mkdir -p build/native-smp-sdk &&
tar -xf build/native-smp-tests/zephyr/llext-edk.tar.xz -C build/native-smp-sdk &&
uv run --locked cmake -S examples/native -B build/native-smp-examples -G Ninja \
  -DLLEXT_EDK_INSTALL_DIR="$PWD/build/native-smp-sdk/llext-edk" &&
uv run --locked cmake --build build/native-smp-examples &&
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
Bluetooth lighting checks exercise the three-second blue flash, steady unclassified
connection, shared reactor output, independent USB/BLE palettes, menu priority and
busy blanking, manual overrides, disconnect, radio off and shutdown. All three
LEDs must latch in one frame without changing the charger-disable channel.
These checks establish output behavior, not perceived color or visual quality.
Matrix checks use the same SPI driver and production service: brightness and
corner wiring, preservation of RGB/charger channels, owner exclusion, malformed
Shell arguments, disconnect during an unfinished write, and failed-write isolation.
The menu checks exercise entry, preview, cancellation, release confirmation, and
retention after rejected selection through the production transition handler.

On F4, visually check the fade-in and cyan/yellow reactor flares, each button and
combinations with `buttons`, and both lighting commands. Confirm that setting the
central color stops animation, while a Bluetooth color command does not interrupt
it. Open the menu, preview both palettes, cancel, then select the other interface;
interface/menu changes restore automatic lighting. Check busy rejection with a
terminal open. In the Bluetooth build, close USB and observe waiting flashes,
then connect through Pybricks and the native terminal/SMP in turn. Check cyan and
yellow, return to flashing after disconnect, menu priority during a connection,
and darkness after `bluetooth off`. Then check `shutdown` and the two-second center hold
with USB and with battery power separately. Check `reboot dfu`, a standard
Pybricks reinstall, and the returning console. Keep independent factory DFU entry
available throughout; follow [recovery](recovery.md). Do not infer power removal
from the disappearance of a serial port alone.

`tests/poweroff` runs the production hub loop, power policy and button driver with
an emulated ADC. It checks that explicit shutdown rejects new ownership and
releases the power latch once. A held button, ADC errors, contact bounce and other
keys cannot trigger restart; a valid center release followed by a new press does.
The board outputs and reset are substituted; this does not prove physical power
removal or USB restart. On hardware, check command and long-press shutdown followed
by a fresh center press with USB left attached, then repeat on battery power.

## Local checks

`tests/motor_output` exercises the private H-bridge adapter through Zephyr GPIO
and the fake PWM driver: direction, smallest/full duty, steady updates without
GPIO changes, brake, coast, sensor supply, rejected commands and cleanup after
injected errors. The fixture records timer counts; it does not emulate STM32
pinmux, inverted waveforms, shared-timer phase or physical motor behavior.
`tests/motors` adds the production port service, ownership, Shell commands,
duration expiry, hotplug, cleanup failures and release during blocked output.
The LUMP fixture checks drive alongside encoder acquisition and coast on invalid
data or UART loss. ARM applications exercise motor syscalls, return, forced stop
and an invalid buffer fault while driving; the IMU fixture checks inhibited bias
learning during drive.

`tests/motor_control` runs the PBIO computation on ARM with a deliberately simple
independent synthetic plant. It checks motion in both directions, long encoder
origins, position hold, timed completion, time wrapping and rejected requests.
`tests/regulated_motors` runs the production UART/ADC/controller/PWM path with
synthetic peers: command ownership, stall, Shell/SMP observations, completion,
missing encoder input, battery failure, lost frames before/during I/O and release
during an unfinished PWM operation. Neither fixture measures physical tuning,
motor stopping or service jitter on the hub.

An optional numerical check enables Zephyr's undefined-behavior sanitizer on ARM:

```sh
uv run --locked west build -b mps2/an385 -s tests/motor_control -d build/motor-control-arm-ubsan-tests -- -DCONFIG_UBSAN=y -DCONFIG_UBSAN_TRAP=y
uv run --locked west build -d build/motor-control-arm-ubsan-tests -t run
```

The QEMU runner remains open after the result; exit after the suite finishes.

`tests/battery` runs production acquisition, Shell observations and both button
ladders on one emulated ADC. It checks cold settling without invented values,
physical-unit scaling and limits, rejected partial/late pairs, preserved samples
on either-channel failure, stale cache reads, shutdown joining and continued button
operation. The converter can deliberately wait; observations must not start more
ADC work. The ARM native fixture also exercises the real SDK and a writable but
misaligned result buffer: rejection must precede the snapshot lock, and subsequent
privileged reads must work. The SMP fixture reads the same service through group 66. These tests do
not emulate STM32 registers, validate physical readings or prove its IRQ timeout.
The separate Zephyr F4 timeout fixture masks the ADC IRQ and checks timeout,
late-interrupt buffer lifetime, retry and asynchronous reads; its image is built
but has not yet run on physical hardware. See [the prerequisite](upstream.md#adc-timeout-prerequisite).

`tests/sound` exercises the production speaker service, waveform and Shell with
a synthetic DMA sink and IRQ callbacks. It checks tone periods/levels/duration,
PCM conversion and buffering, caller-buffer independence, queue backpressure,
partial final blocks, drain, empty streams and ownership. Injected underrun,
start/DMA/cleanup failures, stop retry, disconnect during start and shutdown
check that samples are not replayed and the first error survives cleanup.
`tests/native` uses the same service through the actual ARM SDK and user-memory
checks, with a synthetic tick-driven sink. The sound example must finish tone
and PCM, and stop/fault must release its speaker before unload. `tests/native_smp`
also uploads the sound image through SMP and reads its completed sample count
through the production observations handler. These fixtures
do not execute STM32 DAC registers or measure sound quality or interrupt budgets.

`tests/native` executes the production loader on an ARM MPU model, checking
matrix syscalls, repeated invalid calls, invalid result buffers and release after
stop/fault. Its synthetic SPI peer can delay a write until the test allows it to
finish; application unload must wait for the final blank. It also checks
resource recovery, unavailable memory, interrupted staging, ABI/format rejection,
busy-loop termination at terminal priority and recovery after a prohibited RAM
write. `tests/native_smp` adds the standard serial SMP stack. Its host check uploads
separately built EDK images and invokes the installed CLI through a QEMU PTY;
it checks useful output, stop and the same rejection/fault paths. The simulated
buttons are explicitly synthetic. Actual USB/BLE ownership and disconnect cleanup
remain separate board checks; QEMU has no USB DTR or radio. A synthetic 64 KiB
LittleFS store additionally checks standard SMP file transfer, path restrictions,
atomic replacement, partial and incompatible updates, capacity failure, and
file-loader allocation parity with RAM. The physical F4 uses 224 KiB.

Autonomous ARM checks use Zephyr's `run` build target, including the board's
configured instruction-count clock. This avoids treating host scheduling or QEMU
translation delays as guest execution time. The interactive SMP fixture instead
paces guest time against the host, so a transfer cannot expire while Python waits
to be scheduled. Neither clock supplies physical hub timing measurements.

`tests/management` runs the production Shell/SMP transport and LittleFS/Settings
stack behind a native UART pseudoterminal. Unmodified `smpclient` and `smpmgr`
exercise negotiation, multi-line echo, RAM changes, explicit save, invalid values,
Shell reads, and persistence across process restart. The fixture waits for the
Shell prompt before opening a client and supplies a permanent management session
because the native UART has no USB DTR. The separate `tests/host` UART-emulator
checks exercise actual session transitions, including disconnect during an
accepted SMP operation and isolation of incomplete commands and old responses.

Observation checks use the same SMP handlers and Shell commands: cold unavailable
power input, source discovery, rejected writes, preserved acquisition times and
25-byte matrix frames. A synthetic SPI peer provides matrix transport completion;
physical pixel orientation remains a board check. Native QEMU verifies that
observation neither consumes output nor unloads a completed application, and
returns busy during blocked cleanup. The existing ADC and power-policy fixtures
also check capture timestamps and errors through the production snapshot accessors.

`tests/test_monitor.py` checks the installed monitor CLI and interactive terminal
against that same Shell/SMP peer. It interleaves editing/history with fragmented
SMP, tests the shared stream over a PTY and explicitly synthetic BLE delivery,
and verifies JSONL records, refused overwrites, slow-output scheduling, cancellation
and TTY restoration. It does not emulate a second Shell or claim radio coverage.
`tests/test_terminal.py` stalls a serial PTY behind the actual CLI, directly and
through `uv run`. It checks that input resumes without loss, control keys reach
the peer unchanged, Ctrl+] cancels a blocked send, and local TTY settings are
restored. These checks use no firmware or physical Bluetooth connection.

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
Deferred UART delivery also checks that old driver FIFO input cannot cross a DTR
boundary. Accepted USB commands retain the power hold during teardown, and unread
USB output cannot prevent a subsequent BLE terminal session. The UART emulator
runs below the dispatcher and Shell priorities to expose delayed input; this is a scheduling
test, not a measured USB timing result.

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
idempotent, and the first error prevents reinitialization until reboot. Synthetic
GATT output failure and rejected connections check that disconnect waits leave
the system queue responsive, retain the connection reference and management
ownership correctly, and preserve the first lifecycle error. GATT reads and
subscriptions also check BLE profile indication: shared identity and rejected
operations do not select a profile, native Shell retains its indication, and a
fresh connection forgets the previous selection. This does
not simulate RF or the HCI controller. The standard pytest run executes this suite;
it can also run directly as `build/radio-tests/zephyr/zephyr.exe`;
its clock is accelerated by default. On the Inventor F4, automatic boot discovery,
Pybricks identification, subsequent SMP connection and repeated on/off are checked
without opening USB before the first BLE connection.

On an authorized F4 hub, follow the [USB round trip](host.md#usage). Also attempt
selection while the terminal is open, close it with a partial command, and verify
that the partial command never executes after reopening. Close all clients at
handoff. USB-driver access, physical packet completion and unchanged uptime must
be measured separately from simulation.
