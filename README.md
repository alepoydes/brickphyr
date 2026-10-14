# Brickphyr

An open application platform for LEGO hubs, built on Zephyr RTOS.

Source repository: [alepoydes/brickphyr](https://github.com/alepoydes/brickphyr).

Brickphyr provides a foundation for robots with application-defined control,
state estimation and learning. SPIKE Prime / Robot Inventor F4 is the primary
target, with Babybot as the first integration workload. Native applications and
MicroPython share hardware services and a supervised application lifecycle.
Pybricks supplies reusable implementations, APIs and client compatibility.

**Status: experimental native and Python platform.** On one Robot Inventor
F4, the firmware provides:

- One interactive Zephyr Shell over USB or BLE, with editing, history and completion.
- USB interface selection and Pybricks identification over USB and BLE;
  Pybricks Code can discover and connect to the hub.
- Standard SMP management, runtime settings and explicit persistent save.
- Four buttons, connection lighting, independent USB/battery idle timeouts,
  shutdown and button restart with or without USB attached.
- Software DFU through the standard Pybricks installer; return to official
  Pybricks 4.1.0b4/b6 and back to Brickphyr has been exercised.

Bluetooth is an optional [build profile](docs/bluetooth.md). One computer owns
the management connection at a time. The experimental [native profile](docs/builds.md#experimental-native-sdk)
adds a C SDK, protected execution and RAM uploads over USB/BLE with `brickphyrdev app`.
The native profile also installs several applications in LittleFS and runs them
through the same protected loader. The experimental [Python profile](docs/builds.md#experimental-python-profile)
adds temporary and installed programs, source/bytecode/native imports and the same
shared lifecycle. REPL and pybricksdev workflows have bounded F4 USB/BLE checks;
Pybricks Code has BLE checks. Complete client and hardware acceptance remains open. See the
[Python plan](docs/python.md) for implemented bindings and their limits.
Native and Python execution currently alternate. The [roadmap](docs/roadmap.md)
adds independent native control alongside Python, timed observations/actuation
and experiment recording while completing SPIKE support and reducing resource
costs. WASM remains unimplemented and outside this milestone.
LEGO restoration and interrupted-transfer recovery remain unverified. See
[hardware](docs/hardware.md) for evidence and limitations and [builds](docs/builds.md)
for setup and commands.

Native Technic/Control+ support is a secondary, build-only port. Other hubs follow
concrete robot needs; broad hub compatibility is not the project's purpose.
See [hardware support](docs/hardware.md) for board-specific evidence.

## Design

- Zephyr owns scheduling, isolation, and general-purpose hardware services.
- Pybricks supplies LEGO-specific knowledge, reusable device/control algorithms,
  and the Python API whose behavior we intend to preserve.
- Native applications access hardware without Python through a small firmware
  interface. Applications are separate from the firmware and its source tree.
- Installed runtime engines determine which application formats a firmware can
  load. The uploaded application's format selects its engine.
- The factory LEGO bootloader and independent recovery are preserved; Pybricks
  manages second-stage updates. Available flash is used subject to actual
  hardware and recovery constraints, without the
  inherited fixed division into five small program slots.

The balancing robot project, babybot, remains an independent application and an
integration workload. Its controller, learning algorithms, and operating modes
do not belong in this firmware.

## Read by task

| Task | Document |
| --- | --- |
| Follow priorities and preparation for Babybot | [Platform roadmap](docs/roadmap.md) |
| Understand the system boundaries | [Architecture](docs/architecture.md) |
| Find what comes from which project | [Subsystems](docs/subsystems.md) |
| Check hub and feature availability | [Hardware](docs/hardware.md) |
| Understand workspace, builds, and dependencies | [Builds](docs/builds.md) |
| Write or load an application | [Applications](docs/applications.md) |
| Check Pybricks compatibility | [Compatibility](docs/compatibility.md) |
| Understand host interfaces and session ownership | [Host connections](docs/host.md) |
| Understand settings and inactivity shutdown | [Settings and power](docs/settings.md) |
| Work on Bluetooth | [Bluetooth](docs/bluetooth.md) |
| Install or restore firmware | [Installation commands](docs/builds.md#diagnostic-shell), [recovery constraints](docs/recovery.md) |
| Validate software or hardware | [Testing](docs/testing.md) |
| Contribute and exchange upstream changes | [Contributing](CONTRIBUTING.md), [upstream workflow](docs/upstream.md) |
| Understand licensing | [Licensing](docs/licensing.md) |
| Orient a contributor or agent | [Project orientation](AI/context.md), [agent instructions](AGENTS.md) |

## Start contributing

Read the architecture and the document owning the subsystem you want to change.
The [roadmap](docs/roadmap.md) defines usable delivery blocks and their acceptance:
complete SPIKE services, improve execution and observation, then validate the
Babybot workload using the existing native/Python foundation.
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
