# Hardware support

The experimental Prime F4 image boots and provides a USB diagnostic shell on one
Robot Inventor F4 hub identified through its factory DFU loader. The PCB revision
has not been established. These results do not establish support for every
Inventor or SPIKE Prime revision, battery operation, or completed recovery.

## Target matrix

| Hub family | Scope | Brickphyr subsystems | Physical verification |
| --- | --- | --- | --- |
| Robot Inventor F4 (PCB revision unknown) | Experimental `spike_prime_f4` build | USB diagnostic shell | Installation through factory DFU, shell commands, terminal reopen, software reboot |
| SPIKE Prime F4 (STM32F413VG source target) | Experimental board build; physical boot chain unverified | Clock/power setup, motor coast pins, USB shell compile; shell logic tested on host | None |
| SPIKE Prime H5 / other Prime-family revisions | Require separate identification and port | None yet | None |
| SPIKE Essential | Candidate after the first target | None yet | Hardware availability unconfirmed |
| Technic | Candidate subject to resource and recovery study | None yet | Hardware availability unconfirmed |
| City | Candidate subject to resource and recovery study | None yet | Hardware availability unconfirmed |
| Move | Candidate; severe memory constraints require separate evaluation | None yet | Hardware availability unconfirmed |

The family name is not a board ID. Do not assume all Prime-family hardware uses
the same MCU, radio, boot chain, or flash layout. Add revision-specific rows as
hardware is identified; do not mark an entire family verified from one board.
Inventor shares the relevant upstream F4 code. The verified scope above covers
Brickphyr `0.1.0-bringup` with Zephyr `4.4.2`; Bluetooth, motor outputs, battery
power retention, and application isolation have not been physically verified.
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
the Inventor F4 above; power and motor pin behavior still require physical tests.

Exact firmware/bootloader regions and the verification still needed are in
[recovery](recovery.md). Derive pins, power sequencing, clocking, calibration
locations, and port circuitry from the platform sources and available hardware
evidence; do not fill gaps with plausible constants.

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
