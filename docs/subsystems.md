# Subsystem ownership

The table selects code sources and integration boundaries. The experimental
Prime F4 build integrates clock/power setup, buttons, lighting, and a Zephyr USB
diagnostic shell. See [hardware](hardware.md) for physical verification.
Native loading, storage, matrix, IMU, sound, battery and electrical port detection
are implemented. Remaining device services and Python/WASM integrations below
are selected directions. This is not
a claim that every chip has a Zephyr driver.

| Subsystem | Preferred source | Brickphyr work / boundary |
| --- | --- | --- |
| Scheduling, time, synchronization, user mode, MPU | Zephyr | Service priorities, bounded ownership, application domain |
| MCU startup, clocks, interrupt/DMA support | Zephyr and its SoC support | LEGO board description and required missing support |
| GPIO, UART, SPI, I²C, PWM, ADC, USB, watchdog | Zephyr drivers | Verify each actual peripheral; add missing drivers upstream where useful |
| Hub power, charging status, shutdown, buttons, LEDs, display, sound | Pybricks board/device knowledge + Zephyr mechanisms | Preserve electrical behavior and user-facing semantics; board-specific adapters |
| LEGO port identification and device protocols | Pybricks | [Port detection and hotplug](ports.md), protocol state machines over Zephyr transport |
| Motor sensing, regulation, trajectories, DriveBase | Pybricks PBIO | Reuse algorithms; replace scheduling and low-level I/O coupling |
| IMU acquisition | Zephyr driver if suitable; otherwise reusable Pybricks driver | Verify chip, range, sample timing, filters and units |
| IMU calibration and orientation / Python semantics | Pybricks | Preserve coordinate, calibration, and persistence contracts |
| Bluetooth host, connections, GATT | Zephyr | Hub radio transport/init, service integration; see [Bluetooth](bluetooth.md) |
| Xbox and other input devices | Existing suitable stack support + Pybricks device-specific behavior | Map input consistently; retain connection/error semantics |
| Flash and storage primitives | Zephyr | Board partitioning and application/settings lifecycle; storage format undecided |
| User settings and inactivity shutdown | Zephyr Settings + board power support | Shared validation/application, explicit save, and [power policy](settings.md) |
| Native execution | Zephyr LLEXT + user mode | SDK boundary, manifest checks, loading, resource cleanup |
| Python execution | MicroPython + Pybricks C bindings and libraries | Zephyr scheduling/device integration and API compatibility |
| WASM execution | WAMR Zephyr integration | Bounded runtime configuration and shared hardware imports |
| Application selection, start/stop, fault reporting | Brickphyr | One supervisor, one application, explicit resource ownership |
| Firmware install / factory restoration | Existing hub bootloaders and Pybricks tooling | Compatible image packaging and verified address/type selection |
| Desktop upload, run, logs, later monitoring | Public Pybricks tools, Zephyr Shell, standard management mechanisms | One [host session](host.md); evaluate SMP for extensions; reuse application lifecycle |

## Sharing with Pybricks

Reuse protocol parsing, device identification, calibration, control mathematics,
and board knowledge from Pybricks. Use suitable sources unchanged; port selected
implementations into Brickphyr when their adaptation serves our execution model.
Preserve their licenses and record provenance and differences beside each port.
Keep Zephyr-specific IRQ, DMA, timer, wait, and synchronization support here or
in Zephyr, as appropriate. Use Zephyr's driver model and standard device APIs;
robotics algorithms and resource ownership remain above the device drivers.

The `pybricks.*` API is substantially implemented as MicroPython C bindings; it
cannot be ported by importing a collection of Python files alone. PBIO is a C
library whose current execution model is cooperative. Reusing its algorithms
does not require preserving that execution model in Brickphyr.

Keep dependency-fork changes independently useful to Pybricks itself. Do not
extract a universal PBIO library or add a HAL over both HALs to accommodate
Brickphyr. Extending hardware access from native Pybricks `.mpy` modules is a
separate possible project, not a prerequisite for Brickphyr's native SDK or
hardware support. The procedure is in [upstream](upstream.md).

## Minimum contract for an integrated service

Document the actual caller and owner, units, sample/command timestamps, blocking
behavior, cancellation, errors, and buffer lifetime. For a device service also
record what happens on hotplug, app termination, and loss of the host connection.
Do not invent a uniform object hierarchy to make unlike devices look identical.

For motors, expose both direct drive and the reused servo behavior where supported;
use one ownership rule so they cannot compete. For sensing, a stale sample must
not silently become a fresh measurement. Python, native, and WASM bindings must
use the same service semantics.

## Sources and unresolved integrations

- [Pybricks repository structure](https://github.com/pybricks/pybricks-micropython)
  and [PBIO README](https://raw.githubusercontent.com/pybricks/pybricks-micropython/master/lib/pbio/README.md).
- [Zephyr external modules](https://docs.zephyrproject.org/latest/develop/modules.html)
  and [example application](https://github.com/zephyrproject-rtos/example-application)
  support integration outside the Zephyr tree.
- [MicroPython Zephyr port](https://github.com/micropython/micropython/tree/master/ports/zephyr)
  is a starting point to evaluate, not proof of Pybricks compatibility.
- [WAMR's Zephyr integration](https://docs.zephyrproject.org/latest/develop/manifest/external/wamr.html)
  is preferred to a new VM or JIT implementation.

The first port must resolve radio initialization, electrical port behavior, power
control, and the scheduling dependencies of selected PBIO services. No driver is
considered integrated merely because its source compiles.
