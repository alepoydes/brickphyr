# MicroPython integration

The engine uses the `micropython` submodule selected by the pinned Pybricks
checkout: `740cbf265cda4d404fef11848e3bdf32fcd24420`. Its parent baseline is
MicroPython `0fd6c573ea815774668bbb16b8e197c8822368b2` (version 1.29.0).
The five subsequent Pybricks commits make math modules extensible, add `%i`
formatting, guard standard streams, select the Pybricks STM32 library and adjust
upstream CI tests. Brickphyr compiles core and selected shared runtime files;
the STM32 library and upstream hardware ports are not included.

`CMakeLists.txt` reuses MicroPython's source list and generated qstr/module/root
tables. The `mpy_cross` target builds the compiler from that same checkout into
the build directory. Plain bytecode uses format 6; native modules use format 6.3
and additionally depend on architecture and runtime ABI. The tested producer
target is `armv7m` with software floating point and MicroPython single precision.

`runtime.c` adapts initialization and execution from
`ports/embed/port/embed_util.c` at that revision, preserving its MIT attribution.
It returns execution failures to the application caller, bounds bytecode reads,
and uses caller-owned heap and stack budgets. GC and its architecture-specific
register/stack tracing remain upstream implementations. The
[MicroPython notice](../../LICENSES/MicroPython-MIT.txt) accompanies Python images.
Every interpreter also initializes the shared readline editor through its public
API; the required [reset correction](../../docs/upstream.md#python-runtime-prerequisites)
prevents newline state from leaking across application lifetimes.

The library's mutable state joins Zephyr's C-library application-memory partition
when present, leaving MPU regions for the GC arena and native code. Otherwise it
has its own partition. Partition descriptors remain in privileged firmware memory;
granting the VM write access to those descriptors would weaken its isolation.
The engine neither allocates a separate permanent heap nor creates its own system
thread. The application supervisor supplies its memory domain, lifecycle and output,
reusing the same execution arena for Python and native applications. Its protected
thread supplies the actual stack lower bound for MicroPython's stack checks.

The maintained `MP_PLAT_COMMIT_EXEC` hook publishes native code through a bounded
supervisor operation. The application relocates a temporary GC buffer, then blocks
while the system workqueue copies it into a reserved code region. That worker has
its own MPU mapping; the application always sees native code as read/execute and
its GC arena as non-executable. The lifecycle lock serializes publication with
stop/unload. Code is retained until application exit; upstream root tracking keeps
native BSS/rodata alive across GC. Current targets have no instruction/data caches;
a cache-enabled target requires explicit integration.

The current execution checks cover source, bytecode from both compiler builds,
GC, exceptions, exhaustion and truncated bytecode. RAM launch, stop and output now
use the shared supervisor and SMP handlers, checked through ARM QEMU and USB/BLE
on the Inventor F4. Native/Viper source and bytecode, and a separately compiled C
module with GC also execute over both transports. ARM tests cover code writes,
heap execution, protected-memory access, native loops, allocation exhaustion and
subsequent successful execution.

`files.c` supplies the port's `mp_import_stat`, lexer and bytecode-reader hooks
over an application-owned CBOR file map. The same zcbor library used by SMP
validates its bounded namespace without a second index allocation. Import lookup
references the retained bytes; module resolution, relative packages and native
loading remain upstream code. Installed programs snapshot the entire map so
updates cannot change later imports in a running instance. See
[program format and limits](../../docs/applications.md#named-python-programs).
Source/bytecode/native imports and installed launches have ARM and F4 USB/BLE
coverage, including reboot persistence. Writable file I/O and remaining Pybricks hardware bindings
remain in the [implementation plan](../../docs/python.md).

`files.c` adapts read-only stream operations from `extmod/vfs_rom_file.c` at the
same revision to that existing namespace. `streams.c` adapts
`shared/runtime/sys_stdio_mphal.c`, preserving separate input/output directions
for `.buffer` and polling and adding flush without a second buffering layer.
Both retain the original MIT attribution. Readline, JSON, random and select reuse
upstream implementations. Private application syscalls supply the bounded input
queue and event wait. A host interrupt sets only an atomic signal; VM/event hooks
schedule the exception in the interpreter's own thread.

The port explicitly enables the reference F4 build's memoryviews, builtin help,
memory diagnostics, GC finalizers and extended/reverse class operators on top of
MicroPython's basic feature set. Standard modules remain upstream code; their
[integration coverage](../../docs/applications.md#standard-python-modules) runs
through the protected application lifecycle.

`shared/runtime/pyexec.c` supplies friendly/raw REPL without a second interpreter
or parser. Its stdio-mode hooks distinguish command editing from execution for
interrupt and session-boundary handling. Soft reset returns to the supervisor,
which joins/releases the application before rebuilding VM state; executable
allocations cannot survive merely because the host keeps its connection open.
Core REPL and the common CLI terminal have ARM/PTY checks. Plain Shell entry and
reconnection use the same VM through the shared dispatcher, checked on ARM QEMU
with simulated session boundaries. SMP REPL editing, history/completion, interrupt,
soft reset and raw protocol also pass over USB/BLE on the Inventor F4, as do plain
Shell entry/return, soft reset and reconnection with preserved globals.

The optional Pybricks library compiles its selected parameters, matrix, stopwatch,
awaitable/task and tools sources directly from the pinned dependency checkout.
Its common `qstrdefs.h` supplies strings with punctuation, such as `Li-ion`, that
automatic identifier collection cannot reconstruct.
MicroPython's builtin-subpackage support keeps application imports on the same
standard importer; no Pybricks firmware scheduler or driver is linked. The PBIO
clock hook delegates to the VM clock, while `mp_hal_delay_ms` waits interruptibly
on the application's existing event channel against a monotonic deadline.
The unchanged tools `hub_menu` calls small button/stop-mask and matrix bridges.
Its legacy PBIO matrix handle is unused: rendering goes through the same rooted
`PrimeHub.display` object and protected matrix service. Button changes wake the
existing VM event channel; unchanged hardware polls do not. No second input loop
or matrix renderer is linked.
Each fresh VM resets Color's GC-rooted dictionary and the tools' async state.
Portable integer, error, time-comparison and COBS helpers are compiled once for
both firmware services and bindings. Current [library coverage](../../docs/compatibility.md#implemented-library-subset)
and its [dependency corrections](../../docs/upstream.md#python-library-prerequisites)
remain distinct from physical hardware acceptance.

`pupdevice.c` adapts the MIT-licensed
[`pb_type_iodevices_pupdevice.c`](https://github.com/alepoydes/pybricks-micropython/blob/a7b1b04ae4f8449dd46b3dfb97c420334439fd35/pybricks/iodevices/pb_type_iodevices_pupdevice.c)
metadata/read/write/reset binding to the protected port API. It converts copied wire
values without unaligned accesses; the common UART service owns SELECT, output,
settling and connection generations. Blocking I/O uses interruptible timed VM waits because
there is no PBIO cooperative device scheduler. No second parser or driver is
linked. Explicit reset records the resulting connection generation; a later
hotplug cannot silently rebind an old object.

`uartdevice.c` adapts `pybricks/iodevices/pb_type_uart_device.c` at the same
revision. The existing port worker owns direct UART configuration, copied TX,
RX and cleanup instead of PBIO's UART process. Python owns its rooted immutable
buffers and exact-length/pattern waits; prefix matching handles overlapping
patterns without consuming trailing bytes. The existing Pybricks awaitable serves
both directions. P1/P2 supply and the reference acknowledgement prompt use the
same protected UART lease and explicitly saved configuration; see
[UART semantics and limitations](../../docs/ports.md#custom-serial-devices).

`pupsensors.c` adapts the ForceSensor, TiltSensor, InfraredSensor, ColorSensor,
UltrasonicSensor and ColorDistanceSensor bindings from `pybricks/pupdevices` at
the same revision, plus palette parsing from `util_pb/pb_color_map.c` and
LightArray from `common/pb_type_lightarray.c`. The external ColorLight wrapper
compiles unchanged. The common native sensor service retains calibration,
conversion, palettes and light commands; Python maps units and results and
supplies the infrared object's counter offset. PBIO's color matcher and distance
helpers compile unchanged, without PBIO port instances. Generation-bound
selection rejects stale objects before changing device modes. These classes have
protected ARM coverage with synthetic UART peers; their physical acceptance
remains pending.

`pupaccessories.c` adapts `pybricks/pupdevices/pb_type_pupdevices_light.c` and
`pb_type_pupdevices_colorlightmatrix.c` from that same revision. Light uses the
shared port PWM queue; the matrix uses the shared UART writer. Both bind commands
to the detected connection generation. Matrix HSV-to-wire conversion belongs to
the native service, so it is also available without Python. The normal port
cleanup owns release; Python has no driver handles or output buffers in flight.

`pupsensors.c` also adapts `pb_type_pupdevices_pfmotor.c`. Each PF object retains
its ColorDistanceSensor and reuses that sensor's awaitable. The native service
encodes and queues IR commands, then sends coast to remembered outputs during
application cleanup. Transmission and receiver acknowledgement are different;
see [the port contract](../../docs/ports.md#power-functions-infrared-motors).

`motor.c` adapts `pybricks/common/pb_type_motor.c` at the same revision. DCMotor
and Motor share DC methods. Gear-list parsing stays at the language boundary;
direction, ratio, zero origin, profile, observations and motion live in the native
port/controller service. The existing Pybricks async implementation handles
awaitables, with request tokens protecting newer motions from older cancellation.
See [encoded motors](../../docs/ports.md#python-encoded-motors) for the implemented
methods and remaining compatibility limits. `Motor.control` adapts the reference
`pb_type_control.c` through native settings/trajectory snapshots, without exporting
a privileged PBIO pointer. Updates and application waits share the port worker.
`Motor.model`, `Motor.log` and `Motor.control.log` adapt `pb_type_motor_model.c` and `pb_type_logger.c`
from the same revision. Model changes cross that shared settings boundary;
both independent logger buffers belong to the firmware, and their shared CSV
export implementation uses application stdout.

`car.c` adapts `pybricks/robotics/pb_type_car.c` at the same revision. The
reference end-stop calibration, steering margin and drive-power mapping use the
protected motor commands, with a torque-percentage variant of the existing stall
objective. Request tokens limit error cleanup to this object's commands; there
is no second servo loop or hardware driver.

`messaging.c` adapts `pybricks/messaging/pb_type_app_data.c` at that revision;
the module table and legacy alias compile directly from Pybricks. The binding
retains mode layout, setup/configuration messages and the maintained awaitable
implementation. Supervisor-owned copied buffers replace PBIO's Python-object
callback and pending-data pointer. Lease/request tokens bind cleanup and sends
to their object and operation; forced stop requires no Python finalizer.
See [AppData limits and evidence](../../docs/host.md#application-data).

`ble_radio.c` ports the receive codec from
`pybricks/messaging/pb_type_ble_radio.c` at the same revision, retaining MIT
attribution. It validates field lengths before decoding a copied sample. The
shared Zephyr radio owner replaces the PBIO scan callbacks and retains the
reference channel format, RSSI filter and one-second expiry. No driver retains
Python memory; closing or stopping the application retires its observer before
asynchronous HCI cleanup. `version()` reads validated TI service-pack metadata
cached by the radio owner after initialization, without another HCI command.
Broadcasting remains unfinished.
See [observation support](../../docs/bluetooth.md#application-broadcast-observation).

`hub.c`, `light.c`, `matrix.c`, `speaker.c`, `system.c` and `imu.c` adapt the hub,
battery, charger observations, internal color-light, light-matrix, speaker, system
and IMU bindings from Pybricks
v4.1.0b6 (`a7b1b04ae`). They retain the upstream
MIT attribution. The keypad wrapper, fonts and color conversion compile directly
from that checkout; the font's BBC notice is retained in the distribution notice.
The adaptation replaces PBIO device access with the protected C SDK and common
ownership/cleanup, without linking another driver scheduler. Static display calls
wait interruptibly for the shared service's completion. Color calls do likewise;
system menus can temporarily hide their output without revoking ownership.
Blink/color sequences are validated and converted to RGB once, then copied into
the shared light service. It owns their timing and lifetime independently of
Python, with the same cancellation/cleanup path as native applications.
Matrix orientation belongs to the hub display and is shared by hub objects,
resetting with a fresh constructor or interpreter as in the reference.
The matrix service owns coordinate rotation and copied icon animations as well.
`display.animate()` accepts the reference's 2–255 icons and a positive signed
32-bit millisecond interval. Static display operations stop it. Brightness and
duration validation completes before replacing existing output.

`display.text()` retains its Python string and renders one reference-font
character at a time through that same frame service. It accepts printable ASCII
and nonnegative signed-32-bit on/off durations, including zero. The final
character is followed by a blank; empty text leaves the display unchanged.
Ordinary calls wait interruptibly against the VM's clock. Inside `run_task`, the
maintained Pybricks awaitable drives the same rendering state machine. Replacing
the display exhausts its previous text awaitable; cancelling that obsolete
awaitable cannot blank the replacement. Cancellation of an active text operation
blanks it. Application termination and REPL reset use the common service cleanup.

The speaker retains the reference note grammar, pitch table, articulation and
logarithmic volume behavior. Note parsing bounds every read, consumes the whole
token and rejects zero divisors. The binding retains a lazy iterator on the GC
heap and uses the common service's reservation across notes and rests. It never
owns a DAC buffer or generates samples. That service stops finite tones without
Python cooperation. Ordinary calls poll its bounded status at millisecond
intervals; `run_task` drives the same state machine through the maintained
awaitable. Iterator, driver and interruption errors release playback while
preserving the original exception. Superseded awaitables cannot cancel a newer
tone, including indefinite beeps. See [sound semantics](../../docs/hardware.md#hub-sound).

System information comes from protected snapshots of the launch metadata,
cached reset cause and live management link. The supervisor owns stop-button
selection and delivers a preallocated, GC-rooted `SystemExit` through the VM's
existing event poll. Menu button events remain with the hub UI; forced stop and
long-press shutdown remain independent. The shutdown syscall requests the normal
power policy and waits for supervisor cleanup without running user finalizers.
`system.storage()` copies the common 512-byte user-data region through protected
SDK calls. Flash access belongs to firmware shutdown or explicit Shell save;
aborting the VM cannot strand a filesystem lock. This region is distinct from
runtime settings and read-only bundled resources. `reset_storage()` remains open.
