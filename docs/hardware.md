# Hardware support

The experimental Prime F4 image boots and provides a USB diagnostic shell on one
Robot Inventor F4 hub identified through its factory DFU loader. The PCB revision
has not been established. These results do not establish support for every
Inventor or SPIKE Prime revision, LEGO restoration, or interrupted-update recovery.

The new [USB interface selector](host.md) and minimal Pybricks endpoint are
checked in simulation and over Linux USB on that Inventor F4. The round trip
preserves uptime; occupied-session rejection, terminal editing/history and
partial-input isolation are verified. The local pybricksdev CDC client identifies
the hub, receives status, reconnects, and returns to Shell. Standard SMP clients
exercise fragmented echo and Settings read/write/save. Shell and SMP settings
persist across software reboot, including saves that trigger file compaction.
USB and battery inactivity shutdown and startup are checked; see
[power verification](settings.md#inactivity-shutdown). Other host operating systems
remain unverified.

The optional [Bluetooth controller profile](bluetooth.md#experimental-f4-controller)
initializes the CC2564C through Zephyr and advertises as `Brickphyr`. Two
initialization/advertising cycles, including controller disable between them,
pass on the same Inventor F4 with Linux discovery. The Pybricks GATT endpoint
also passes Linux pybricksdev identification/status, reconnect, return to Shell
and USB/BLE exclusion checks. Standard SMP BLE clients pass discovery, echo,
fragmented packets, settings read/write/save and incomplete-packet isolation.
An active BLE owner inhibits idle shutdown on USB and battery power. USB/battery
source changes preserve the session and uptime.

## Target matrix

| Hub family | Scope | Brickphyr subsystems | Physical verification |
| --- | --- | --- | --- |
| Robot Inventor F4 (PCB revision unknown) | Experimental `spike_prime_f4` build | USB/BLE Shell/SMP, Pybricks identification, native applications, settings, power/UI and hardware APIs below | Installation/restoration, connection and power cycles; bounded native matrix/IMU/audio, A/B encoder and unloaded motor checks; subsystem limits below |
| SPIKE Prime F4 (STM32F413VG source target) | Experimental board build; physical boot chain unverified | Clock/power setup, motor coast pins, USB shell, buttons and lights compile; shell and driver logic tested on host | None |
| SPIKE Prime H5 / other Prime-family revisions | Require separate identification and port | None yet | None |
| SPIKE Essential | Candidate after the first target | None yet | Hardware availability unconfirmed |
| Technic 88012 (STM32L431RC source target) | Experimental `technic_hub` base, BLE and RAM-native builds | Power/button, timer RGB, watchdog and factory-loader package; optional CC2640 management, IMU and electrical port detection have synthetic checks; shared native loader has compact ARM checks | No board available; boot, BLE, applications, IMU, recovery and electrical behavior unverified |
| City | Candidate subject to resource and recovery study | None yet | Hardware availability unconfirmed |
| Move | Candidate; severe memory constraints require separate evaluation | None yet | Hardware availability unconfirmed |

The family name is not a board ID. Do not assume all Prime-family hardware uses
the same MCU, radio, boot chain, or flash layout. Add revision-specific rows as
hardware is identified; do not mark an entire family verified from one board.
Inventor shares the relevant upstream F4 code. Native application checks are
described in [applications](applications.md#experimental-native-profile).
Bounded motor checks are described under [external ports](#external-ports-and-sensors). Battery retention and
center-button start/shutdown have operator
observations on this hub, not current-consumption measurements.
Other hubs are outside the current implementation target list.

For each implemented revision, extend its entry with separate results for boot /
recovery, USB, Bluetooth, ports and motors, IMU, power/UI, runtime engines, and
isolation. A build-only port and an on-device result must remain distinguishable.
Keep runtime inclusion in [builds](builds.md), not a second independent build table.

## Initial source evidence

The board implementation references Pybricks v4.1.0b4,
[`8aa9986cd0666fb610225de05440fb6d1f99c4ab`](https://github.com/pybricks/pybricks-micropython/tree/8aa9986cd0666fb610225de05440fb6d1f99c4ab),
as a source reference, not a chosen Zephyr dependency set.

That revision's `lib/pbio/platform/prime_hub_f4/` describes an STM32F413 target
with 320 KiB RAM, a TI CC2564C Bluetooth device, and a W25Q256 external flash.
The external chip has 32 MiB capacity; the storage exposed by a particular
firmware is a separate policy. Its SPI attachment must not be assumed to support
direct execution of application instructions.

The experimental board uses the `STM32F413VG` identity from the reference's
[`mpconfigport.h`](https://github.com/pybricks/pybricks-micropython/blob/8aa9986cd0666fb610225de05440fb6d1f99c4ab/bricks/primehub_f4/mpconfigport.h).
Its 16 MHz HSE, 96 MHz system clock, 48 MHz USB clock, PA13 power hold, PA11/PA12
USB pins, and motor bridge pins come from
[`platform.c`](https://github.com/pybricks/pybricks-micropython/blob/8aa9986cd0666fb610225de05440fb6d1f99c4ab/lib/pbio/platform/prime_hub_f4/platform.c).
The both-inputs-low coast state follows
[`motor_driver_hbridge_pwm.c`](https://github.com/pybricks/pybricks-micropython/blob/8aa9986cd0666fb610225de05440fb6d1f99c4ab/lib/pbio/drv/motor_driver/motor_driver_hbridge_pwm.c).
Zephyr supplies the SoC/drivers; Brickphyr supplies the devicetree, early power
hold, and loader metadata. USB boot and shell behavior have been exercised on
the Inventor F4 above; electrical waveforms still require instrumented tests.

Exact firmware/bootloader regions and the verification still needed are in
[recovery](recovery.md). Derive pins, power sequencing, clocking, calibration
locations, and port circuitry from the platform sources and available hardware
evidence; do not fill gaps with plausible constants.

## Experimental Technic base

The four-port Control+/Technic Hub has a build-only port with optional BLE and
RAM-native profiles. Its STM32L431RC, 80 MHz MSI/PLL clock, PC12 power hold, PC14 active-low
button and RGB pin assignments follow the pinned Pybricks
[`technic_hub/platform.c`](https://github.com/pybricks/pybricks-micropython/blob/a7b1b04ae4f8449dd46b3dfb97c420334439fd35/lib/pbio/platform/technic_hub/platform.c)
and [`mpconfigport.h`](https://github.com/pybricks/pybricks-micropython/blob/a7b1b04ae4f8449dd46b3dfb97c420334439fd35/bricks/technichub/mpconfigport.h).
Zephyr supplies the clock, GPIO keys, timer PWM, MPU and independent watchdog.
The shared Brickphyr hub loop, power policy and lighting service handle the
button's two-second shutdown hold and native-mode indication. Four motor bridges
start in coast. The sensor supply stays off until the optional port service starts.

Red PB15/TIM15 CH2 and green PA11/TIM1 CH4 use 10 kHz hardware PWM, sharing the
reference motor period; blue PA6/TIM16 CH1 uses 1 kHz. This avoids Pybricks'
software GPIO PWM interrupt while retaining one shared animation/ownership
implementation. The initial RGB weights follow the reference's relative channel
factors; brightness, hue and waveforms have not been checked physically. There is
no separate Bluetooth button/light; requests for that light return `-ENODEV`.

The base reserves the first 1 KiB of SRAM for the factory loader handoff and
builds in the remaining 63 KiB with the same ARMv7E-M hard-float calling convention
as F4. The optional native profile reuses the protected loader and shared
application lifecycle with smaller memory budgets and enables the shared IMU
and telemetry services; the base has no application engine or IMU acquisition.
The native profile also enables the shared electrical detector on A–D, using
the reference GPIO wiring and PB12 sensor supply. It reports empty/passive
connections and the UART electrical signature. It also enables the shared
direct motor service for passive medium/train motors on A–D: signed duty,
brake, coast, duration expiry and ownership cleanup. Digital negotiation,
UART sensor measurements, regulated motors and persistent storage remain
unimplemented on this board. Four-port synthetic GPIO checks cover discovery,
hotplug, first-error preservation and shutdown; physical wiring and timing are
unverified. All Technic profiles enable the shared
[battery observations and protection](#hub-battery), with synthetic ADC/GPIO
checks and no physical battery evidence. The CC2640 is held in reset; no host terminal is available in this
base image. Its factory-programmed TI controller owns GAP/GATT and needs a
different adapter from F4's HCI controller. Do not substitute a generic SPI HCI
driver or reflash that controller to bypass the integration work.

Each motor uses the main and complementary outputs of one timer channel:
TIM15 CH1 for A, TIM1 CH2 for B, CH1 for C and CH3 for D. Normal outputs use
inverted polarity; complementary outputs retain normal polarity, matching the
reference CCxP/CCxNP setup. Both legs start as low GPIOs. The shared output
adapter disconnects and disables both PWM outputs before changing direction,
then muxes only the driven leg; the other leg is high GPIO. All use the same
10 kHz period as the LEDs sharing TIM1/TIM15. The Technic package builds with
these mappings; a synthetic PWM check covers shared-channel selection, flags
and coast on reversal. Physical pin behavior, waveforms and motor operation
remain unverified.

The optional [CC2640 transport profile](builds.md#experimental-technic-base)
adds a Zephyr SPI/GPIO port of Pybricks' NPI framing, ready handshake and reset
sequence. It checks framed event lengths and checksums, bounds handshakes and
latches the first failure until explicit reset. Synthetic SPI/GPIO checks cover
full-size packets, a rapid ready-line release/reassertion and fault cleanup.
The optional initialization layer retains the reference command order, factory
address, 0 dBm output and 27-byte link-layer packet limit. It checks address
readback and command replies, bounds missing replies and preserves the first
failure through reset cleanup. Synthetic checks exercise these behaviors through
the production controller code; the NPI device is simulated.
The GATT adapter registers Generic Attribute/Access, Device Information, Pybricks,
legacy NUS and native Shell services. It validates allocated handle ranges and
publishes the database only after all registrations succeed. Peripheral GAP
initialization follows the reference ordering between Generic Access and Device
Information registration, with pairing disabled and the validated public factory
address. It does not erase controller bonds. Its ATT request
handler supports discovery, reads, subscriptions and shared Pybricks/Shell writes
at the default MTU of 23 bytes. Synthetic wire checks cover discovery boundaries,
rejected requests, cold Shell/SMP use and reconnect.
The base and NPI-only builds keep the controller in reset at boot. The optional
[BLE build](builds.md#experimental-technic-base) initializes it and adds an
advertising/event worker using the shared Shell, standard SMP BLE service and
Pybricks identity session. Its production-path synthetic fixture covers cold
startup, notification backpressure, fragmented SMP packets, disconnect/reconnect
and old-output isolation. This remains build-only support: no physical Technic BLE
connection, boot or recovery has been checked.

[Builds](builds.md#experimental-technic-base) gives the build command;
[recovery](recovery.md#technic-factory-loader-boundary) owns the checksum, SRAM
selector and installation limits. Compilation and synthetic button/PWM checks
do not verify startup, watchdog expiry, restoration or power consumption on a
physical Technic Hub.

## F4 buttons, lighting, and power

The four buttons use two ADC resistor ladders: PC4/ADC14 contains the center
button together with non-button inputs; PA1/ADC1 contains left, right, and
Bluetooth. The decoder and nominal resistor-ladder cutoffs come from Pybricks.
With charging disabled, the measured center-button level on the Inventor F4
was 3141–3147 ADC counts, straddling Pybricks' loaded-ladder cutoff of 3142.
This build uses the nominal ladder cutoffs for both ladders. With charging
enabled, the released center/CHG level and a short center press are checked on
the Inventor F4; all combined ladder inputs still need characterization. The Zephyr Input adapter
samples every 10 ms after startup settling and requires two matching samples.
An invalid sample releases reported keys so it cannot sustain a shutdown hold.

The TLC5955 drives both central RGB LEDs and the Bluetooth RGB LED over SPI1,
with a TIM12 clock and PA15 latch. The driver reuses Pybricks control/grayscale
encoding over Zephyr SPI/PWM/GPIO and exposes Zephyr LED operations. Its initial
3.2 mA setting matches Pybricks. The shared charger-control channel starts at
the Pybricks disabled value; [charging](#hub-charger) owns subsequent changes.
Ordinary LED writes cannot change that reserved channel. The matrix uses the
same controller and frame; see [hub matrix](#hub-matrix).

Both F4 SPI controllers use Zephyr's STM32 DMA driver to avoid per-byte
interrupt service: SPI1 uses DMA2 streams 2/3 for the TLC5955, and SPI2 uses DMA1
streams 3/4 for external flash. The assignments follow the Pybricks F4 platform;
the speaker retains its separate DMA1 stream 5. Completion interrupts retain the
SPI priority below the byte-fed UARTs. Calls still wait for completion before
latching a frame or finishing a flash transaction, preserving buffer lifetimes
and the existing driver locks.

Readiness lighting uses a driver extension for 16-bit RGB writes, latching both
central LEDs and the Bluetooth LED in one transfer. The experimental reactor animation has a saturated
cyan base for Pybricks (40% dominant-channel duty) and yellow for Brickphyr (95%).
Small independent pulses add 2–5% each, starting every 60–100 ms. Their
60–80 ms attack and 80–100 ms decay overlap into continuous irregular motion.
A separate 10–20% surge starts every 2.5–5 seconds, rises over 80–120 ms and
falls over 60–100 ms. All envelopes sum before individual channel clipping;
clipping can shift the hue without adding another color channel. Startup fades
in over 1.2 seconds, and interface changes blend over 300 ms.
Frames follow an absolute 20 ms schedule in the existing hub thread; delayed
frames are skipped. Three small-pulse slots cover the maximum 180 ms lifetime
at a minimum 60 ms onset spacing. The surge has its own slot; no active envelope
is overwritten. Each update evaluates at most four envelopes and schedules at
most one event of each kind. A local libc PRNG needs no hardware entropy or
allocation. Both buttons share the pulse envelopes and animation code, with
independent palettes. The periodic SPI traffic remains one 97-byte transfer per
frame (4,850 bytes/s at 50 Hz); menu and manual changes also write immediately.
Manual color and shutdown operations serialize with animation writes; shutdown
prevents relighting. See [shell usage](builds.md#diagnostic-shell).

The central button shows the selected USB interface. In the Bluetooth build, the
Bluetooth button follows the actual BLE connection independently:

| BLE state | Bluetooth button |
| --- | --- |
| Waiting for a connection | Full blue flash for 100 ms, once every three seconds |
| Connected, no service used yet | Steady blue |
| Pybricks service in use | Cyan reactor animation |
| Native terminal or SMP in use | Yellow reactor animation |
| Radio disabled, starting, stopping or failed | Off |

The connection menu overrides this indication with white; a rejected selection
briefly blanks it. Closing the menu restores the connection indication. Manual
Bluetooth color lasts until a BLE state, interface or menu change, or reboot;
manual central color does not stop automatic Bluetooth indication. Without the
Bluetooth build option, that button remains off outside the menu unless set
manually. These connection-driven colors have native simulation checks; their
brightness and transitions still need a physical hub check.

The F4 board configuration holds provisional channel weights `255:47:64`, selected
visually using RGB pairs on the central button of the tested Inventor F4. These
are hardware gains, not palette colors: yellow is logical RGB `255 255 0`.
RGB commands and lighting apply gamma 2 followed by these gains. Animation then
normalizes the strongest channel and applies its brightness envelope; menu white
uses full-range corrected duty. Direct `led pwm` commands bypass conversion and
accept the full 16-bit duty range. Perceived colors, flare character, and menu white still
need operator acceptance; no photometric calibration has been performed.

Four individual button presses and green central/blue Bluetooth lighting have
been checked on the Inventor F4 above. Software DFU entry, standard Pybricks
reinstallation through mboot, and the returning USB console have also been
checked. Return to official Pybricks 4.1.0b4 and 4.1.0b6 verified USB identification,
STOP/status, and reconnection. Its automatic DFU entry then allowed Brickphyr
reinstallation with a working console. See [recovery](recovery.md) for the limits
of this normal installation cycle. The operator observed battery-only startup
and operation after USB removal, and the lights extinguishing after a two-second center hold with USB
and on battery. Subsequent center-button startup and `shutdown` were also checked;
the latter removed the hub from USB enumeration. These are functional observations,
not measurements of residual current or proof of wake initiated by the USB host.
Button restart with USB left attached is checked after both command shutdown and
a long center hold. See [power behavior](settings.md#inactivity-shutdown) and
[testing](testing.md#hub-input-and-lighting).

### Application button lighting

`CONFIG_BRICKPHYR_LIGHT` exposes the existing button-light service through
`bp_light_write()` and `bp_light_get()`. A write copies three logical RGB bytes
and claims the selected `BP_LIGHT_CENTER` or `BP_LIGHT_BLUETOOTH` until application
cleanup. It performs no allocation, driver I/O or wait; another write while one is
pending returns `EBUSY`. The existing hub loop applies the color on its next
scheduled frame, using the same gamma and board gains as Shell RGB commands.

`bp_light_animate()` repeats a copied sequence of 1–256 RGB frames, each with a
duration of 1–2147483647 ms. It allocates at most 2 KiB per light from the existing
system heap; replacing either of two maximum sequences temporarily requires a
third allocation, for 6 KiB plus allocator overhead. Submission does not wait
for memory or perform device I/O. Invalid input, a pending write or allocation
failure preserves the preceding operation. Static writes cancel animation.

The existing hub loop services animation deadlines alongside readiness lighting.
It advances against monotonic time and skips expired frames after a delay;
there is at most one shared SPI latch per loop iteration. Short intervals can
increase traffic above the readiness animation's 50 Hz. A 97-byte frame requires
32.4 microseconds of wire time at the F4's configured 24 MHz, before GPIO,
locking, driver and scheduling overhead. This is a transfer-size calculation,
not a measured timing guarantee. Admission validates at most 256 cells before
the service lock; the critical section contains a nonwaiting allocation, a copy
of at most 2 KiB and replacement/free. Updates scan at most 256 cells per light,
without allocation, logging or a catch-up loop over missed cycles.

Snapshots distinguish requested RGB, pending work, ownership, visible override,
last write validity/time and I/O errors. A successful controller write does not
establish physical light output. System connection menus temporarily hide both
application colors, preserving their claims; shutdown rejects further writes and
extinguishes the LEDs. Manual Shell writes to a claimed light report `EBUSY`.
Animation snapshots additionally report its active state and frame index/count.
Menus hide output without pausing the sequence. A failed SPI write stops its
advancement; explicit replacement is required to start it again.

Return, fault, stop and Python soft reset restore system indication only for the
lights the application claimed. A failed restoration retains ownership and
reports the cleanup error. A host disconnection leaves the application colors
alone. Cleanup also frees copied sequences. Simulator SPI checks cover these
priorities, failures, allocator exhaustion, delayed frames and independent light
ownership; ARM checks exercise the same service from protected Python, including
a busy loop without cooperative yielding and invalid animation-buffer pointers.
Physical acceptance of application lighting remains pending.

## Hub matrix

The F4 image implements a 5 by 5 monochrome display through the existing TLC5955
driver. Its row-major wiring comes from Pybricks `prime_hub_f4`'s
`LED_ARRAY_DEV_0_LIGHT_MATRIX`. Brightness is 0–100 with Pybricks' gamma-2
conversion to 16-bit PWM. Coordinates start at the top left in the upright hub
orientation. Matrix and button writes serialize through the same persistent
controller frame, preserving each other's channels and the charger-control output.

The [Shell commands](builds.md#diagnostic-shell) and native SDK use one display
service. The first accepted terminal or application write acquires the matrix;
the other owner receives `-EBUSY`. The idle program selector owns a
system preview that an application or terminal can replace immediately, including
while a preview transfer is pending. A terminal clear or disconnect
blanks its display and releases it. Host disconnection leaves an application's
display alone. Application return, fault or stop blanks it before unload. A
failed blank retains ownership and reports the error; an explicit cleanup can
retry it. Shutdown closes admission and requests a blank.

While no application, REPL or management session is active, the F4 matrix shows
the selected program. Pybricks USB mode shows slot 0–4 when slot execution is
included in the build; `!` indicates that it is unavailable. In Brickphyr USB mode,
left/right browse visible installed named programs in lexical order with wraparound;
names beginning with an underscore are skipped. The last selection remains in RAM
after the program completes and after returning from Pybricks mode; a removed
selection advances to the next visible name. `-` means there are none. The selected
name appears one letter at a time; each letter stays bright, fades out, then the
next appears after a brief dark pause. After one pass, the first letter remains.
A left/right press sends a short light sweep in that direction; repeated presses
replace the sweep, and the selected name appears when browsing finishes. A short
center press flashes the whole matrix and fades it before launching the selection;
the flash adds about 115 ms before execution. `X` briefly reports an empty slot or
failed launch. The two-second center hold still shuts down.
Holding the wake button at boot does not launch a program. The system preview
releases the matrix before a button-launched program runs and returns when it
stops and the hub is idle. A host-launched program can take the display on its
first write. Named-program selection and short-center launch are
operator-confirmed on the Inventor F4 with a motorless native example. Pybricks
slot digits, right-button selection and short-center launch of a motorless
Python example are also confirmed there. The same board retained a motorless
Python selection after button launch and skipped an underscore-prefixed installed
program while browsing. USB Shell removal of both test programs left the
pre-existing applications intact.

`bp_matrix_write(first, values, count)` accepts a contiguous portion of the
25-pixel row-major frame, including a single pixel or the whole display. It
copies the values before returning and does not wait or allocate. One frame can
be pending; another write receives `-EBUSY`. A system work item performs the SPI
operation, so aborting the application cannot leave a driver mutex locked or
invalidate the driver's buffer. Each attempted frame adds one 97-byte transfer
to the existing button animation traffic. There is no periodic display transfer
when the image is unchanged.

`bp_matrix_animate(frames, count, interval_ms)` repeats 1–255 frames independently
of application execution. It copies 25 bytes per frame into service-owned heap
memory: up to 6375 bytes, or 12750 bytes during replacement, plus allocator
overhead. Validation and nonwaiting allocation precede replacement. Allocation,
copy and publication share one bounded critical section so forced application
stop cannot leak an unpublished buffer. No allocation occurs while playing.
A delayed system work item selects the frame from absolute elapsed time, skipping
expired frames rather than replaying them. Intervals are 1–2147483647 ms; driver
and system-queue latency can delay a frame. Each update uses one controller write.
An I/O failure stops advancement and releases the sequence; ownership remains
until explicit release. Static writes stop animation; a partial write then starts
from a blank frame. Invalid replacement leaves the current sequence intact.

`bp_matrix_orientation(rotation)` selects 0–3 clockwise quarter-turns for future
application writes and animation frames. It does not redraw the current image
or acquire the display. Application cleanup resets the orientation. Terminal
coordinates and observation arrays always use the physical upright layout.

`bp_matrix_get()` distinguishes requested values from the last successful write,
with its monotonic completion time, pending/animation flags, owner, latest error and failure
count. Successful writes establish controller output, not measured light.
An I/O error marks the output uncertain until another write succeeds. Rejected
SPI frames are not retained for transmission by an unrelated RGB update.

Native simulation checks cover wiring endpoints, brightness, shared-channel
preservation, ownership, input validation, in-flight disconnect and SPI failure,
copied sequences, orientation changes, delayed frames and exhausted allocation.
ARM QEMU additionally exercises user-memory validation and application stop/fault
cleanup through the actual native syscalls and loader. On the Inventor F4,
Shell writes and the separately uploaded native matrix example complete without
driver errors over USB/BLE; stopping the application clears and releases the
display. Physical matrix orientation, brightness and perceived output still
await operator verification.

## Hub IMU

The F4 build includes LSM6DS3TR-C acquisition and calibrated orientation with
`CONFIG_BRICKPHYR_IMU`. It reuses the pinned ST/Pybricks register functions through
Zephyr I²C/GPIO, without the PBIO scheduler or STM32 HAL backend. Wiring and
mounting signs come from Pybricks' Prime F4 platform: I²C2 at 400 kHz, PB10/PB3,
INT1 on PB4, sensor-to-hub signs −X/+Y/−Z. Both channels run at 833 Hz with ±8 g
and ±2000°/s ranges. Startup waits for sensor boot and gyro settling; block data
update and the filter-settling data-ready mask are enabled.

The Technic native profile uses the same acquisition, calibration, orientation,
settings and native API implementations. Its reference wiring is I²C1 on PB8/PB9,
INT1 on PC9 and sensor-to-hub signs −X/−Y/+Z. The I²C timing preset
`0x00000404` at a 5 MHz peripheral clock follows the pinned
[Pybricks driver](https://github.com/pybricks/pybricks-micropython/blob/a7b1b04ae4f8449dd46b3dfb97c420334439fd35/lib/pbio/drv/imu/imu_lsm6ds3tr_c_stm32.c);
its bus rate is slightly below 400 kHz after internal and electrical delays.
Transfers use Zephyr interrupts with a 10 ms timeout and one standard bus-recovery
attempt after a failed initial identity read. Calibration changes apply in RAM;
this profile has no persistent settings backend. Register-peer simulation checks
Technic's mounting, calibrated readings and lifecycle. Physical bus timing,
axes, bias and orientation remain unverified on Technic.

Read a snapshot in either terminal, or select it in the host monitor:

```sh
uv run brickphyrdev monitor --source imu
uv run brickphyrdev monitor --ble Brickphyr --source imu.calibrated --log imu.jsonl
```

The Shell commands are `observe imu`, `observe imu.calibrated` and
`observe imu.orientation`. Native applications call `bp_imu_get()` for raw data
or `bp_imu_get_state()` for a coherent calibrated snapshot, including its raw
input. The example `imu.elf` prints calibrated acceleration and angular velocity.
All paths copy the latest published sample, without I²C access,
allocation, ownership changes or sensor reconfiguration. Read errors preserve the
last valid values and acquisition time. The API returns `-ENOTSUP` when excluded;
otherwise inspect `sample.error`, including startup (`-EINPROGRESS`), waiting or
resynchronization (`-EAGAIN`), stale data (`-ETIMEDOUT`) and stopped acquisition
(`-ESHUTDOWN`). Units and counters are described in [observations](telemetry.md).

Calibration and quaternion fusion are ported from Pybricks with recorded
[provenance](../src/imu/README.md). Accelerometer values include gravity. The
orientation matrix maps hub coordinates to an inertial frame initialized from
gravity; no magnetometer supplies absolute heading. Integrated axis rotations
and clockwise-positive 1D/3D headings are in degrees. The 3D heading projects the
front axis onto the horizontal plane and is undefined when that axis is vertical.

The acquisition worker also performs processing, using actual accepted sample
intervals. Missing, failed or stale acquisition starts a new orientation reference
on the next usable frame, increments `epoch`, and retains learned bias. Calibration
changes likewise start a new reference; compare angles only within one epoch.
Readers copy the coherent snapshot under a short lock; no application can hold
the processing lock. No extra sample queue or processing thread is introduced.
Native [sample-triggered control](applications.md#native-control-and-background-computation)
can sleep on publication of a distinct fresh sample. It uses one coalescing signal
and preserves the exact triggering observation; ordinary snapshot readers do not
consume that signal or change acquisition.

Stationary detection and bias averaging retain Pybricks' bounded sample windows.
`stationary` means low variation, and `ready` means bias was learned within the
last ten minutes. Constant rotation can resemble bias; neither flag proves
absolute rest. Learning continues during applications. Set either stationary
threshold to zero to disable learning when necessary. Motor drive inhibits
learning until the output is confirmed passive and a new stationary window is
observed. Initial bias, scale and calibration are changed
through [Settings](settings.md#imu-calibration); learning never writes flash.

`sample_us` timestamps the observed gyro-ready edge on the monotonic clock.
Acceleration accompanies it in the same 12-byte burst. This is not a simultaneous
hardware latch of six axes, nor the physical sampling instant before filter
delay. When the synchronous I²C read returns, the driver rejects frames crossed
by another observed gyro-ready edge. A later edge during conversion or publication
belongs to the next frame and does not invalidate bytes already copied. After an I²C failure,
the driver drains a complete frame before accepting new values.
The chip's BDU protection covers each two-byte word; see
[ST AN5130, sections 3.9 and 4](https://www.st.com/resource/en/application_note/dm00472670-lsm6ds3tr-c-always-on-3d-accelerometer-and-3d-gyroscope-stmicroelectronics.pdf).

One preemptible worker has a 1536-byte stack and a binary wakeup; it performs no
periodic polling and no sample queue grows behind a slow consumer. The 400 kHz
bus transaction has a nominal wire time
of about 338 µs within a 1201 µs period, excluding IRQ/software overhead. F4 I²C
transactions time out after 10 ms; reset polling is bounded. Observation performs
no background transmission. Shutdown joins acquisition and powers down both
sensors. The F4 acquisition thread precedes the 5 ms motor service and host
processing. The main UI/power loop targets a 20 ms cadence and has lower
priority than acquisition and port service, allowing them to preempt normal UI
work after I/O completion.
The USB core and dedicated CDC queue are preemptible at priority 8, with the
[upstream prerequisites](upstream.md#usb-acquisition-scheduling-prerequisite),
so acquisition can preempt both control requests and CDC callbacks. USB event
processing shares the context mutex with configuration and teardown operations.

Native simulation uses the actual worker and register library with a synthetic
I²C/GPIO peer. It checks cold initialization, signed limits and mounting axes,
units, preserved timestamps, stale samples, transfer failures, overlapping reads,
shutdown and first-error preservation when initialization and cleanup both fail.
It also checks calibration persistence, stationary learning, synthetic rotation,
reference discontinuities and actual SMP observations. ARM numerical checks cover
gravity poses, complete and tilted rotations, heading, calibration and rejected
settings without mutation. Physical raw/calibrated observations and the native
IMU example run over USB/BLE on the Inventor F4. With preemptible USB processing,
a bounded ten-second check returned 255 valid telemetry snapshots without missing
samples or acquisition failures. Both counters remained zero after a finite
Python BLE output stream and a USB reader-pause/STOP check, at 154 seconds uptime.
This does not establish a worst-case latency guarantee: startup/session
transitions and all peripheral combinations still require characterization.
Mounting verification, calibrated accuracy, CPU use, stack margin and residual
current remain open.

Acquisition reliability under sustained combined load remains unverified.
Diagnostic phase timing distinguishes late acquisition starts from delayed
worker wakeups after the I²C interrupt
has already completed the read. The latter can cause conservative rejection
of an intact frame; the synchronous API does not report the interrupt's
completion timestamp. Cooperative work and interrupt processing still need
bounded scheduling; thread priorities alone do not establish acquisition reliability.
Native Python code preparation has its own preemptible worker: initializing its
16 KiB arena must not delay acquisition on the cooperative system workqueue.
Bounded Inventor F4 checks cover a 32 KiB RAM upload, dynamic C module import,
idle HubNetwork construction and subsequent BLE output without IMU gaps.
HubNetwork inbox allocation avoids clearing user-sized buffers with interrupts
masked. Inventor F4 creation/stop checks with 1 KiB and 8 KiB per sender preserve
IMU counters and both idle motor UART links. Sustained-load acceptance remains open.
The UART overrun observed during the installer's initial connection was traced
to Shell thread teardown. The [retained Shell worker](host.md#one-management-session)
and [application-thread cleanup correction](ports.md#electrical-ownership-and-shutdown)
address the reproduced teardown failures. Bounded reconnect, application-stop
and software-reboot checks retain both ID-75 links without acquisition errors.
Physical cold power-on and sustained combined-load acceptance remain open.

## Hub sound

`CONFIG_BRICKPHYR_SOUND` provides one speaker service for the shared USB/BLE Shell
and native/Python applications. The F4 backend ports Pybricks' DAC1 channel 1 output on
PA4, PC10 amplifier enable, TIM6 trigger and DMA1 stream 5/channel 7 to Zephyr.
Nothing plays at startup. [Source provenance](../src/sound/README.md) records the
reused implementation and its changes.

In either terminal:

```text
sound tone 440 200 10
sound status
observe sound
sound stop
```

Tone arguments are frequency in Hz (20–24000, or zero for silence), duration in milliseconds (zero
continues until stopped), and volume (0–100). Tones are square waves at a fixed
48 kHz sample clock, with Pybricks' logarithmic volume law. Edges are quantized to
one sample; no claim of high-fidelity tone synthesis is made.

Native applications use `bp_sound_tone()` or `bp_sound_stream()` with
`bp_sound_write()` and `bp_sound_finish()`. PCM is signed 16-bit mono at 8000–48000
samples/s. The DAC uses the upper 12 bits. The timer chooses the nearest divider;
status reports requested and effective integer Hz. No implicit resampling occurs.
The [native example](../examples/native/sound.c) plays a short tone followed by
a generated PCM triangle, without a file or allocation.

Commands do not block. Playback acquires the free speaker for the application or
terminal; a competing start returns `-EBUSY`. Each write copies 1–256 samples into
a firmware queue and accepts either all or none (`-EAGAIN` when full). Two complete
chunks prime the DMA buffer; finishing also permits a shorter stream. Finish
marks the end of input and drains it, including silence after the last conversion.
Keep the application alive until idle to hear the complete stream: return, fault
and forced stop quiesce DMA before unload. Terminal disconnection stops only its
own playback. Completion or successful stop releases ownership; a failed stop
retains it and can be retried. Shutdown, reboot and DFU quiesce sound as well.

`bp_sound_reserve()` optionally claims the free speaker across several tones,
streams and pauses. With a reservation, completion leaves it idle but owned;
the same application can start the next tone or stream. Explicit stop or
application cleanup releases it. The Python speaker uses this to prevent a
terminal command from interrupting a melody between notes. An error remains
observable while reserved; a failed cleanup still retains the owner.

Python provides `hub.speaker.volume()`, `beep()` and `play_notes()` through this
same service. Calls wait interruptibly, or return awaitables inside `run_task`.
The [speaker example](../examples/python/speaker.py) combines a quiet melody with
display text and checks cancellation. Notes retain the Pybricks octave range,
sharps/flats, dotted duration, slurs and rests. A note iterator is consumed lazily;
malformed notes or iterator exceptions stop playback. Zero tempo and zero note
divisors raise `ValueError`. Negative beep duration continues until replaced or
the application ends; zero duration stops an existing tone. Finite tones stop in
the service even while Python is busy. Advancing the melody requires Python to
run; it does not add another background interpreter task.

Nonzero beep frequencies clamp to 64–24000 Hz, as in Pybricks' sampled driver;
negative frequencies use the lower limit instead of unsigned wraparound. The
volume defaults to 100 and applies to subsequent tones. Hub objects share one
speaker and its volume until interpreter reset. New playback cancels the previous
awaitable; closing that obsolete awaitable cannot stop its replacement. Sound
completion includes DMA draining, so note boundaries include buffer/workqueue
latency and are not a sample-accurate musical sequencer.

DMA has two 256-sample halves; the queue holds another 1024 samples. These buffers
occupy 3072 bytes, independently of stream length. At 48 kHz, a half lasts about
5.33 ms, there are 187.5 callbacks/s, and the full queue represents another 21.33 ms
of producer time. Each callback fills at most 256 samples with integer operations;
there is no allocation, floating-point calculation or logging in the refill path.
Start and stop run on the existing system workqueue. Reads and writes copy bounded
state under a spinlock; user applications cannot hold a device mutex or lend DMA
their own memory.

On F4, byte-fed UART reception takes priority over speaker interrupts. Tone
generation keeps oscillator state in registers, and priming releases the IRQ
mask between the two halves. At 115200 baud a UART byte arrives every 86.8 µs;
the audio half-buffer deadline alone is not a sufficient interrupt-latency budget.

Missing PCM, a DMA/DAC failure or a detected late refill stops playback and
preserves the first error. Old audio is not intentionally replayed to hide an
underrun. `sound status` distinguishes queued and completed samples, playback
state, cleanup error and cumulative failure/underrun counts. Completion counts
DMA consumption, not measured acoustic output. Actual sampling, refill deadline
margin, interruption clicks, amplifier behavior and sound quality still require
physical verification under combined IMU/radio/application load.

Native simulation checks the production service, waveform and Shell through a
synthetic DMA sink. ARM QEMU additionally exercises the SDK calls and cleanup
after protected application stop/fault. On the Inventor F4 the native tone/PCM
example completes over USB/BLE, including 8192 PCM samples at 16 kHz, with no
reported underrun or cleanup failure. Bounded repeated-launch checks preserve
hub responsiveness and both attached ID-75 encoder links, using the required
[ARM MPU and thread-cleanup corrections](upstream.md#native-loader-prerequisites).
These checks do not establish worst-case timing with six active ports. Acoustic
quality and actual sample timing still require listening and instrumented checks.

Python speaker checks use the same service with synthetic DMA in protected ARM
execution. On the Inventor F4, the speaker example completes through native
USB/BLE management and the actual Pybricks USB/BLE clients. Interruption and REPL
reset release playback, with no reported underrun or cleanup failure. These
checks confirm execution and driver completion, not acoustic quality.

## External ports and sensors

The F4 build has experimental electrical detection and LUMP UART negotiation on
ports A–F, shared by the native SDK, Shell and SMP. [External ports](ports.md)
describes identification, raw packets, converted sensor/encoder measurements,
explicit mode selection, direct and regulated motor drive, hotplug and cleanup. Supported SPIKE, BOOST and WeDo
sensors share one service, including validated additional sensor power. Synthetic
GPIO/UART checks cover conversions, encoder turn counting and ownership; ARM
checks cover the protected API boundary. On one Inventor F4, motors on A/B
reporting ID 75 negotiate at 115200 baud, expose their modes and provide moving
encoders. Positive/negative 150 ms direct commands and 300 ms speed commands
complete and coast; short position targets and hold also pass with suspended
wheels. Protected native motor commands complete and release their port over
both USB and BLE. Original LEGO ID-61 color and ID-62 ultrasonic sensors also
negotiate and provide default measurements; synchronous/awaitable Python reads,
light commands and cleanup pass through native USB/BLE management. See
[sensor hardware coverage](ports.md#sensor-hardware-checks) for limits: calibrated
accuracy, supply electrical measurements and other device types remain unverified.
Recognized motors stay in coast until explicitly commanded.
Python `PUPDevice` reset also renegotiates an idle ID-75 motor over USB/BLE,
followed by synchronous and awaitable CALIB reads and port release. Force, Tilt
and Infrared Python classes have synthetic ARM coverage only.
Experimental PBIO speed/position control, trajectories and hold use
the same port owner and output. Synthetic UART/ADC/PWM checks cover stall,
completion, missing feedback and battery errors, missed deadlines and concurrent
release. Numerical ARM checks cover closed feedback on an independent synthetic
plant. Physical tuning, period jitter and simultaneous peripheral load remain
unverified.

## Hub battery

`CONFIG_BRICKPHYR_BATTERY` provides chemistry, voltage, load current and temperature through
`bp_battery_get()`, `observe battery` and the same USB/BLE monitor:

```sh
uv run brickphyrdev monitor --source battery
uv run brickphyrdev monitor --ble Brickphyr --source battery --log battery.jsonl
```

Python uses the same cached observations:

```python
from pybricks.hubs import PrimeHub
hub = PrimeHub()
print(hub.battery.type())         # "Li-ion" on F4
print(hub.battery.temperature())  # signed millidegrees Celsius
```

On F4, PC0/ADC1 channel 10 measures current, PC1/channel 11 measures voltage,
and PB0/channel 8 measures the battery thermistor. The board declares Li-ion
chemistry; this is a connector property, not an ADC estimate.
The 12-bit conversion and coefficients retain Pybricks' implementation at
[`a7b1b04ae`](https://github.com/alepoydes/pybricks-micropython/tree/a7b1b04ae4f8449dd46b3dfb97c420334439fd35):
`lib/pbio/drv/battery/battery_adc.c` and the Prime F4 platform/configuration.
The port in `src/hub_battery.c` retains its MIT notice. Board coefficients live
in `battery.dtsi`: 4096 counts correspond to 7300 mA or 9900 mV; the voltage
correction is the integer current times 3/16 Ω. Integer rounding order is retained.
Current-offset variation of tens of milliamps is known in the source; these are
not calibrated laboratory measurements. Current is not signed charging current,
and no state-of-charge percentage is inferred. Charging control remains separate.

The build-only Technic port uses the same service with PC1/ADC1 channel 2 for
voltage and PC2/channel 3 for current. Its reference coefficients are 9618 mV
and 4178 mA per 4096 counts, a 20-count current offset added before scaling, and
3/4 Ω voltage correction. PA12 is sampled at boot without a pull: high selects
alkaline thresholds, low selects Li-ion. A failed connector read prevents valid
battery observations and low-voltage protection instead of guessing a chemistry.
Its 20 MHz synchronous ADC
clock and 640.5-cycle sampling time follow the reference; Zephyr encodes that
sample-time choice as 641 ticks.
Technic has no battery thermistor or charger in this port. Temperature reports
`-ENOTSUP` without incrementing a failure counter; the internal MCU temperature
channel is not used as battery temperature. The reference warns of nonlinear
current readings below roughly 100 mA and voltage overestimation under high B/D
port load, since the voltage correction models the A/C measurement path.
Synthetic ADC/GPIO checks cover alkaline selection, scaling, offset, absent
temperature and alkaline thresholds. Physical classification, accuracy and
shutdown under battery load remain unverified on Technic.

Temperature reuses the reference 103AT conversion: a 2.4 kΩ series and 7.5 kΩ
parallel network, 10 kΩ at 25 °C and beta 3435 K. The cached result is signed
millidegrees Celsius. Zero ADC input and readings implying nonpositive thermistor
resistance return `-ERANGE`. A thermistor failure retains its last valid value
and timestamp, with its own error and saturating failure count; it does not
invalidate voltage/current observations or stop a motor using those observations.

The existing system workqueue takes single-channel conversions for current,
voltage and, when present, temperature, without DMA or a new thread. This avoids depending on CPU service of
back-to-back results at the ADC's full conversion rate. A new pair is requested
10 ms after the preceding attempt, matching the reference's nominal polling
period; scheduling can lengthen it. There is no sample history or allocation.
The first second remains `-EINPROGRESS` for startup settling; no minimum voltage
is substituted. Observations copy the cache without ADC access or changing power
policy. A pair is published only when both reads succeed within 40 ms. Its
`sample_ms` is the completion time of that accepted pair, not a simultaneous
hardware latch of both channels. `completed_ms` records the last attempt's end.
Read errors preserve values and acquisition time; a cached pair older than 40 ms
reports `-ETIMEDOUT`. Temperature uses its own completion timestamp and the same
freshness bound, independently of the pair. Chemistry remains available during
settling and acquisition failures. Shutdown joins this work without disabling the button ADC.

F4 conversions use the [local Zephyr timeout correction](upstream.md#adc-timeout-prerequisite).
At the board's 24 MHz ADC clock, one 12-bit conversion with 15-cycle acquisition
takes 27 cycles, nominally 1.125 µs. A 2 ms failure deadline bounds missing
completion interrupts; this is a recovery budget, not measured normal latency.
Stopping the conversion and its interrupts precedes returning an error, preserving
buffer lifetime. Three ADC interrupts per battery acquisition add at most 300/s
to the two button ladders' nominal 200/s. Each conversion arms and cancels a kernel
timer. The thermistor conversion adds one `logf` per successful acquisition;
cached readers perform neither conversion nor floating-point calculation.
Combined CPU/IRQ cost and measurement accuracy still require physical testing.

`average_mv`, `low` and `critical` expose the filtered voltage and hysteretic
protection state. The moving average and thresholds are adapted from Pybricks
`lib/pbio/src/battery.c` and `lib/pbio/sys/battery.c` at the revision above.
The filter uses 64 samples at the nominal 10 ms acquisition period, retaining
the reference's nominal 640 ms time constant. Acquisition failures retain an
established condition; they do not turn an invalid reading into a healthy battery.
See [critical-battery shutdown](settings.md#inactivity-shutdown).

Synthetic ADC checks exercise startup, scaling boundaries, first-error preservation,
partial-pair rejection, the 25 °C reference point, negative temperatures, thermistor
fault isolation, stale snapshots, shutdown and continued button sampling.
ARM QEMU checks protected SDK access and SMP snapshots. On the Inventor F4,
USB/BLE observations provide changing voltage/current readings, including during
the short motor checks. Python and Shell/SMP checks also return Li-ion chemistry
and fresh thermistor readings, without reported ADC failures. Absolute accuracy and
the hardware timeout path have not been verified with instruments or fault injection.

## Hub charger

`CONFIG_BRICKPHYR_CHARGER` controls the F4 MP2639A charger using Zephyr drivers.
Its implementation is adapted from Pybricks
[`charger_mp2639a.c`](https://github.com/alepoydes/pybricks-micropython/blob/a7b1b04ae4f8449dd46b3dfb97c420334439fd35/lib/pbio/drv/charger/charger_mp2639a.c),
with its MIT notice preserved. TLC5955 channel 14 controls MODE, TIM5/PA0 sets
ISET, PA3/ADC3 reads IB, and bit 2 of the center-button ladder reads CHG.
The charger chip controls the charge phases and its analogue protections.

With `CONFIG_BRICKPHYR_USB_BC12` on F4, the STM32 USB controller classifies the
source before the existing CDC device attaches. Detection uses a BC1.2 child of
the same UDC register owner and Pybricks' detection deadlines; it adds no thread.
The hub loop handles insertion and removal, including while USB keeps the hub
powered after shutdown. CDC initialization remains automatic, but USB enable waits
for classification. A source or USB attach failure is reported until the cable
is removed and reinserted. A failed USB detach leaves the PHY state uncertain
and requires a restart before another detection can begin.

VBUS alone grants no charging budget. An awake, configured USB host permits
the reference standard-port ISET setting (15% PWM); a detected charging downstream
port (CDP) or dedicated charging port (DCP) permits the reference charging-port
setting (100%), including before enumeration and while USB is suspended.
A standard downstream port (SDP) or unknown/nonstandard source requires host
configuration and wakefulness. The source allowance is 500 mA for the configured
USB device or 1500 mA for CDP/DCP; it covers the entire USB input, not just charging.

These are the Prime F4 settings from the reference driver, not a calibrated
conversion from PWM to USB milliamps. ISET controls charge current; the chip's
input-current and input-voltage protections are separate. Actual total input
current and the IB conversion still need instrumented verification. Source
changes update ISET without toggling MODE or resetting charge/fault history.
A failed setting change attempts to disable MODE and zero ISET, preserving the
first error. An output error means the physical outputs cannot be assumed known.
No open terminal or keep-alive application is required. Charging continues after
[shutdown](settings.md#inactivity-shutdown) while its host or detected-source
budget remains valid.

Read the same cached observations in the terminal or host monitor:

```text
charger
observe charger
observe battery
```

```sh
uv run brickphyrdev monitor --source charger --source battery --log charging.jsonl
```

Native applications read `bp_charger_get()`; Python uses the same snapshot:

```python
from pybricks.hubs import PrimeHub
hub = PrimeHub()
print(hub.charger.current())  # empirical charger current, mA
print(hub.charger.status())   # 0 discharging, 1 charging, 2 complete, 3 fault
print(hub.charger.connected())  # 0 none, 1 nonstandard, 2 SDP, 3 CDP, 4 DCP
```

`current()` and `status()` raise `OSError` for unavailable, failed or stale
observations. `connected()` reports the independently cached BC1.2 source class
or raises `OSError` for a detection/USB failure or stale snapshot; an ADC error
alone does not invalidate its result. Reading any observation does not change
charging policy.

`charger` prints the source, allowance, applied ISET, policy reason, chip status
and fault/restart counters. `enabled` and `iset_percent` describe the last
successfully applied outputs; `input_budget_ma` is the source allowance, including
during a periodic charge pause. It is neither a measured current nor a calibrated
hardware limit. `current_ma` is the reference's empirical IB conversion,
clamped at zero instead of wrapping below its offset. `status` is 0 discharging,
1 charging, 2 complete/suspended by the chip, or 3 fault. It is not a state-of-charge
percentage. Inspect the observation error before trusting these fields: an I/O
failure can leave the actual output unknown. The existing Python status values
are preserved; in particular, an inactive CHG signal does not alone prove a full
battery. Status is sampled every 250 ms; a complete seven-sample CHG window
identifies the chip's blinking fault indication. Stable output does not cause
repeated SPI writes. After one hour enabled, charging pauses for 30 seconds and
restarts, as in Pybricks. The full pause starts only after outputs are confirmed
disabled; failed attempts cannot shorten it. A detected hardware fault inhibits these automatic
restarts until USB removal; MODE stays enabled so the chip's fault remains
observable and its own protections control charging. `faults` counts observed
fault episodes and `restarts` counts completed periodic restarts since boot.
Removal revokes charging at the next hub-loop update; suspend also revokes an
ordinary host's budget. CDP/DCP retains its charging-port setting.

`reason` is 0 no USB, 1 waiting for USB configuration, 2 USB suspended,
3 enabled, 4 periodic restart pause, 5 hardware fault, or 6 I/O error. It explains
charger policy, not net battery charge: simultaneous hub consumption can exceed
available input power. The readings do not provide calibrated net battery current
or a state-of-charge percentage. A suitably powered idle hub should gain charge;
that property requires physical measurements, not just an enabled flag.

Initial charging and recovery of battery voltage, lighting and the center button
are checked on the Inventor F4. The source-aware charging-port setting also has
a bounded three-minute USB check on that board with connected peripherals and
motor outputs off: battery voltage rose, temperature remained stable, and no
charger acquisition or chip fault was reported. Shutdown then left USB attached
with the console closed. This is evidence of charging under that load, not a
measurement of battery capacity, total USB current or a complete charge cycle.
Python current/status readings through the
protected SDK are checked with synthetic ADC input on ARM and over USB/BLE on
that F4. Full-charge termination, all temperature/fault
conditions, physical BC1.2 classification, adapter charging and instrumented
current accuracy remain open. Synthetic checks cover detector timing,
source changes, suspended/detached budgets, failed output changes, fault detection
and a full restart pause after failed disable. These checks do not establish
electrical source classification or current accuracy on a hub.

## A support claim must be reproducible

A verified row identifies board/product revision, firmware and dependency
revisions, build profile, test procedure, and supported behavior. Keep experiment
reports and device/session details in ignored `reports/`. Public documentation
states supported configurations, reproducible procedures, and known limitations.

Validation on a limited number of available hubs is expected. Reuse upstream
knowledge to prepare other ports, then accept community test reports tied to
identified hardware. Do not require ownership of every hub before developing the
first port, and do not describe untested ports as verified.

## Adding a board

First establish its restoration path, protected storage, actual memory budget,
and minimum power/clock/console setup. Reuse common devices through devicetree and
existing drivers. Introduce a fork only for changes that cannot live out of tree.
Use [testing](testing.md) for incremental device checks and
[recovery](recovery.md) before any firmware write.
