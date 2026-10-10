# Hardware support

No hub is supported by a runnable Brickphyr firmware yet. Physical availability
and successful use under LEGO/Pybricks firmware do not establish a Brickphyr port.

## Target matrix

| Hub family | Scope | Brickphyr subsystems | Physical verification |
| --- | --- | --- | --- |
| Inventor / SPIKE Prime | First target; identify exact board revision before bring-up | None yet | None |
| SPIKE Essential | Candidate after the first target | None yet | Hardware availability unconfirmed |
| Technic | Candidate subject to resource and recovery study | None yet | Hardware availability unconfirmed |
| City | Candidate subject to resource and recovery study | None yet | Hardware availability unconfirmed |
| Move | Candidate; severe memory constraints require separate evaluation | None yet | Hardware availability unconfirmed |

The family name is not a board ID. Do not assume all Prime-family hardware uses
the same MCU, radio, boot chain, or flash layout. Add revision-specific rows as
hardware is identified; do not mark an entire family verified from one board.
Other hubs are outside the current implementation target list.

For each implemented revision, extend its entry with separate results for boot /
recovery, USB, Bluetooth, ports and motors, IMU, power/UI, runtime engines, and
isolation. A build-only port and an on-device result must remain distinguishable.
Keep runtime inclusion in [builds](builds.md), not a second independent build table.

## Initial source evidence

The first hardware study can use Pybricks
[`8aa9986cd0666fb610225de05440fb6d1f99c4ab`](https://github.com/pybricks/pybricks-micropython/tree/8aa9986cd0666fb610225de05440fb6d1f99c4ab),
the v4.1.0b4 reference used by the existing babybot development environment.
This is a source reference, not a chosen Zephyr dependency set.

That revision's `lib/pbio/platform/prime_hub_f4/` describes an STM32F413 target
with 320 KiB RAM, a TI CC2564C Bluetooth device, and a W25Q256 external flash.
The external chip has 32 MiB capacity; the storage exposed by a particular
firmware is a separate policy. Its SPI attachment must not be assumed to support
direct execution of application instructions.

Exact firmware/bootloader regions and the verification still needed are in
[recovery](recovery.md). Derive pins, power sequencing, clocking, calibration
locations, and port circuitry from the platform sources and available hardware
evidence; do not fill gaps with plausible constants.

## A support claim must be reproducible

A verified row identifies board/product revision, firmware and dependency
revisions, build profile, connected devices, test procedure, and result. Store
concise findings in the repository; raw captures belong in ignored reports or
release artifacts. Link failures and restrictions as well as successes.

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
