# Brickphyr

An open application platform for LEGO hubs, built on Zephyr RTOS.

Brickphyr is being designed to run native applications, MicroPython programs
using the Pybricks API, and WebAssembly applications on LEGO hubs. One user
application runs at a time. Hardware services remain responsive independently
of the application's interpreter, garbage collection, or willingness to yield.

**Status: architecture and repository foundation.** There is no Brickphyr firmware
image, board port, SDK, or supported installation procedure yet. No hub has been
flashed with Brickphyr. The documents distinguish selected designs from tested
implementations; upstream support alone is not a Brickphyr hardware result.

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
| Resume development in a new session | [Current development context](AI/context.md), [agent instructions](AGENTS.md) |

## Start contributing

Read the architecture and the document owning the subsystem you want to change.
The next work is a reproducible Zephyr workspace and a recovery-preserving board
bring-up, followed by an isolated native application using real sensor and motor
services. Implementation steps and unresolved choices are in
[the development context](AI/context.md).

The repository currently needs only a Markdown reader and Git. Dependency pins,
build commands, and runnable examples will be added with the first actual build;
there are no placeholder commands that claim to compile or flash a hub.

## License

New Brickphyr code and documentation are licensed under [Apache-2.0](LICENSE).
Reused components retain their own licenses; contributions intended for Pybricks
remain suitable for its MIT-licensed code. See [licensing](docs/licensing.md).

Brickphyr is an independent project, not an official LEGO, Pybricks, or Zephyr
release.
