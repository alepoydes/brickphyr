# Brickphyr

An open application platform for LEGO hubs, built on Zephyr RTOS.

Brickphyr is being designed to run native applications, MicroPython programs
using the Pybricks API, and WebAssembly applications on LEGO hubs. One user
application runs at a time. Hardware services remain responsive independently
of the application's interpreter, garbage collection, or willingness to yield.

**Status: experimental board bring-up.** The Prime F4 target builds a Zephyr
image with a USB diagnostic shell. The same shell runs in Zephyr's native
simulator. No hub has been flashed with Brickphyr; USB, power, and recovery on
physical hardware remain unverified. There is no application loader, SDK, or
supported installation procedure yet. See [builds](docs/builds.md) for commands.

The first target is the Inventor / SPIKE Prime family, starting with an identified
physical board revision. Other hubs follow when their memory, peripherals, and
recovery paths are understood. See [hardware support](docs/hardware.md).

## Design

- Zephyr owns scheduling, isolation, and general-purpose hardware services.
- Pybricks supplies LEGO-specific knowledge, reusable device/control algorithms,
  and the Python API whose behavior we intend to preserve.
- Native applications access hardware without Python through a small firmware
  interface. Applications are separate from the firmware and its source tree.
- Installed runtime engines determine which application formats a firmware can
  load. The uploaded application's format selects its engine.
- Existing bootloader and restoration mechanisms are preserved. Available flash
  is used subject to actual hardware and recovery constraints, without the
  inherited fixed division into five small program slots.

The balancing robot project, babybot, remains an independent application and an
integration workload. Its controller, learning algorithms, and operating modes
do not belong in this firmware.

## Read by task

| Task | Document |
| --- | --- |
| Understand the system boundaries | [Architecture](docs/architecture.md) |
| Find what comes from which project | [Subsystems](docs/subsystems.md) |
| Check hub and feature availability | [Hardware](docs/hardware.md) |
| Understand workspace, builds, and dependencies | [Builds](docs/builds.md) |
| Write or load an application | [Applications](docs/applications.md) |
| Check Pybricks compatibility | [Compatibility](docs/compatibility.md) |
| Work on Bluetooth | [Bluetooth](docs/bluetooth.md) |
| Install or restore firmware | [Recovery design](docs/recovery.md) — no install commands yet |
| Validate software or hardware | [Testing](docs/testing.md) |
| Contribute and exchange upstream changes | [Contributing](CONTRIBUTING.md), [upstream workflow](docs/upstream.md) |
| Understand licensing | [Licensing](docs/licensing.md) |
| Orient a contributor or agent | [Project orientation](AI/context.md), [agent instructions](AGENTS.md) |

## Start contributing

Read the architecture and the document owning the subsystem you want to change.
The next hardware step is identifying a Prime board revision and verifying
installation, the USB shell, and restoration to Pybricks. An isolated native
application using real sensor and motor services follows that bring-up.
Subsystem contracts and implementation boundaries are linked in
[the project orientation](AI/context.md).

Build dependencies are separate west checkouts; host Python tools are locked with
uv. [Builds](docs/builds.md) describes setup and [testing](docs/testing.md) describes
the short checks of executable behavior and the linked image.

## License

New Brickphyr code and documentation are licensed under [Apache-2.0](LICENSE).
Reused components retain their own licenses; contributions intended for Pybricks
remain suitable for its MIT-licensed code. See [licensing](docs/licensing.md).

Brickphyr is an independent project, not an official LEGO, Pybricks, or Zephyr
release.
