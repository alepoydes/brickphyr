# Subsystem ownership

The table selects code sources and integration boundaries. Every Brickphyr
integration below is currently unimplemented. It does not claim that a matching
Zephyr driver exists for every chip in every hub.

| Subsystem | Preferred source | Brickphyr work / boundary |
| --- | --- | --- |
| Scheduling, time, synchronization, user mode, MPU | Zephyr | Service priorities, bounded ownership, application domain |
| MCU startup, clocks, interrupt/DMA support | Zephyr and its SoC support | LEGO board description and required missing support |
| GPIO, UART, SPI, I²C, PWM, ADC, USB, watchdog | Zephyr drivers | Verify each actual peripheral; add missing drivers upstream where useful |
| Hub power, charging status, shutdown, buttons, LEDs, display, sound | Pybricks board/device knowledge + Zephyr mechanisms | Preserve electrical behavior and user-facing semantics; board-specific adapters |
| LEGO port identification and device protocols | Pybricks | Port detection, hotplug, protocol state machines over Zephyr transport |
| Motor sensing, regulation, trajectories, DriveBase | Pybricks PBIO | Reuse algorithms; replace scheduling and low-level I/O coupling |
| IMU acquisition | Zephyr driver if suitable; otherwise reusable Pybricks driver | Verify chip, range, sample timing, filters and units |
| IMU calibration and orientation / Python semantics | Pybricks | Preserve coordinate, calibration, and persistence contracts |
| Bluetooth host, connections, GATT | Zephyr | Hub radio transport/init, service integration; see [Bluetooth](bluetooth.md) |
| Xbox and other input devices | Existing suitable stack support + Pybricks device-specific behavior | Map input consistently; retain connection/error semantics |
| Flash and storage primitives | Zephyr | Board partitioning and application/settings lifecycle; storage format undecided |
| Native execution | Zephyr LLEXT + user mode | SDK boundary, manifest checks, loading, resource cleanup |
| Python execution | MicroPython + Pybricks C bindings and libraries | Zephyr scheduling/device integration and API compatibility |
| WASM execution | WAMR Zephyr integration | Bounded runtime configuration and shared hardware imports |
| Application selection, start/stop, fault reporting | Brickphyr | One supervisor, one application, explicit resource ownership |
| Firmware install / factory restoration | Existing hub bootloaders and Pybricks tooling | Compatible image packaging and verified address/type selection |
| Desktop upload, run, logs, later monitoring | Public pybricksdev functionality where compatible | Additional protocol/SDK tooling only where existing contracts cannot serve |

## Sharing with Pybricks

Share protocol parsing, device identification, calibration, control mathematics,
and other code independent of the operating system. Keep Zephyr-specific IRQ,
DMA, timer, wait, and synchronization adapters here or in Zephyr, as appropriate.
An algorithm may remain byte-identical while its I/O adapter differs.

The `pybricks.*` API is substantially implemented as MicroPython C bindings; it
cannot be ported by importing a collection of Python files alone. PBIO is a C
library whose current execution model is cooperative. Reusing its algorithms
does not require preserving that execution model in Brickphyr.

Start from the existing repositories. Do not extract a new universal PBIO library
or add a HAL over both HALs before an actual integration establishes the required
boundary. Make portable refactorings reviewable by Pybricks independently of our
choice of RTOS. The procedure is in [upstream](upstream.md).

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
