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
| Robot Inventor F4 (PCB revision unknown) | Experimental `spike_prime_f4` build | USB Shell/SMP, Pybricks identification, persistent settings, buttons, lighting, DFU entry | Factory/mboot installation, shell/reopen/reboot, four button presses, central/Bluetooth colors, software DFU, Pybricks restoration and return; operator-observed battery start/retention and long-press shutdown |
| SPIKE Prime F4 (STM32F413VG source target) | Experimental board build; physical boot chain unverified | Clock/power setup, motor coast pins, USB shell, buttons and lights compile; shell and driver logic tested on host | None |
| SPIKE Prime H5 / other Prime-family revisions | Require separate identification and port | None yet | None |
| SPIKE Essential | Candidate after the first target | None yet | Hardware availability unconfirmed |
| Technic | Candidate subject to resource and recovery study | None yet | Hardware availability unconfirmed |
| City | Candidate subject to resource and recovery study | None yet | Hardware availability unconfirmed |
| Move | Candidate; severe memory constraints require separate evaluation | None yet | Hardware availability unconfirmed |

The family name is not a board ID. Do not assume all Prime-family hardware uses
the same MCU, radio, boot chain, or flash layout. Add revision-specific rows as
hardware is identified; do not mark an entire family verified from one board.
Inventor shares the relevant upstream F4 code. The verified scope above covers
Brickphyr `0.1.0-bringup` with Zephyr `4.4.2`; Bluetooth controller input, motor outputs, and
application isolation have not been physically verified. Battery retention and
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
the Inventor F4 above; motor pin behavior still requires physical tests.

Exact firmware/bootloader regions and the verification still needed are in
[recovery](recovery.md). Derive pins, power sequencing, clocking, calibration
locations, and port circuitry from the platform sources and available hardware
evidence; do not fill gaps with plausible constants.

## F4 buttons, lighting, and power

The four buttons use two ADC resistor ladders: PC4/ADC14 contains the center
button together with non-button inputs; PA1/ADC1 contains left, right, and
Bluetooth. The decoder and nominal resistor-ladder cutoffs come from Pybricks.
With charging disabled, the measured center-button level on the Inventor F4
was 3141–3147 ADC counts, straddling Pybricks' loaded-ladder cutoff of 3142.
This build uses the nominal ladder cutoffs for both ladders; adding charger
control requires rechecking these levels. The Zephyr Input adapter
samples every 10 ms after startup settling and requires two matching samples.
An invalid sample releases reported keys so it cannot sustain a shutdown hold.

The TLC5955 drives both central RGB LEDs and the Bluetooth RGB LED over SPI1,
with a TIM12 clock and PA15 latch. The driver reuses Pybricks control/grayscale
encoding over Zephyr SPI/PWM/GPIO and exposes Zephyr LED operations. Its initial
3.2 mA setting matches Pybricks. The shared charger-control channel remains at
the Pybricks disabled value; this step does not implement charging or the matrix.

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
| Waiting for a connection | Full blue flash for 100 ms, once per second |
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
