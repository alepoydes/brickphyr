# Project orientation

Brickphyr is an open application platform for LEGO hubs, built on Zephyr RTOS.

Read [AGENTS.md](../AGENTS.md), [CONTRIBUTING.md](../CONTRIBUTING.md), and
[the architecture](../docs/architecture.md), then the document owning the task:

- [Builds](../docs/builds.md) and [testing](../docs/testing.md): setup and checks.
- [Hardware](../docs/hardware.md) and [recovery](../docs/recovery.md): board support
  and firmware installation constraints.
- [Subsystems](../docs/subsystems.md): dependency and implementation ownership.
- [Compatibility](../docs/compatibility.md) and [Bluetooth](../docs/bluetooth.md):
  Pybricks behavior, host interaction, and radio integration.
- [Applications](../docs/applications.md): native, Python, and WASM lifecycle.
- [Host connections](../docs/host.md): interface selection and one management session.
- [Settings and power](../docs/settings.md): runtime changes, explicit save, and shutdown.
- [Upstream](../docs/upstream.md): dependency changes and contributions.

The experimental F4 build boots on an Inventor F4 and provides a verified USB
diagnostic shell, terminal reconnection, software reboot, four buttons, central/
Bluetooth button lighting, and software DFU followed by a standard reinstall.
The operator observed battery operation, long-press shutdown, and subsequent
button startup; `shutdown` also disconnected USB. Restoration remains unverified.
Bluetooth, application loading, and
Python/WASM execution are not implemented yet. Current availability belongs in
the README and the owning subsystem documents.

The F4 build uses local, unpublished Pybricks helper and host-tool changes;
the public manifest still pins their upstream base. See
[the downstream delta](../docs/upstream.md#current-downstream-delta). Complete
reproducible dependency integration and the boot/restoration cycle before the USB
selection experiment. [Acceptance milestones](../docs/builds.md#acceptance-milestones)
give the development order from the existing build.

Everything tracked by Git is public documentation or project source. Keep local
development handoffs, device observations, experiment results, and pending session
work in the ignored `reports/` directory. Inspect those notes when available and
recheck current workspace/hardware state; they are not public project contracts
or continuing authorization to operate hardware.
