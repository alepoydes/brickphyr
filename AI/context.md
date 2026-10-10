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
- [Upstream](../docs/upstream.md): dependency changes and contributions.

The experimental F4 build provides a USB diagnostic shell. Bluetooth, application
loading, and Python/WASM execution are not implemented yet. Current availability
belongs in the README and the owning subsystem documents.

Everything tracked by Git is public documentation or project source. Keep local
development handoffs, device observations, experiment results, and pending session
work in the ignored `reports/` directory. Inspect those notes when available and
recheck current workspace/hardware state; they are not public project contracts
or continuing authorization to operate hardware.
