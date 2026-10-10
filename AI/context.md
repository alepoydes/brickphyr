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
button startup; `shutdown` also disconnected USB. Return to official Pybricks
4.1.0b4/b6 and back to Brickphyr is verified over USB. LEGO restoration and interrupted
updates remain unverified. Bluetooth, application loading, and Python/WASM
execution are not implemented yet. Current availability belongs in
the README and the owning subsystem documents.

The current build indicates Pybricks with white/blue and Brickphyr with yellow/pink
breathing on a three-second cycle. BT opens/cancels the interface menu, arrows
preview, and a short center release confirms; white BT lighting marks the menu.
Manual central color stops animation until an interface/menu change or reboot;
shutdown prevents relighting. Menu and SPI-emulator checks pass; visual assessment
of these new palettes/timing remains outstanding.

The F4 build uses published Pybricks helper and host-tool changes pinned to our
fork in the manifest. See
[the downstream delta](../docs/upstream.md#current-downstream-delta). The USB
selection experiment passes host simulation and the Linux USB round trip on the
Inventor F4: terminal editing/history, busy rejection, partial-input isolation,
and continuous uptime. It requires the dependency changes listed
in [upstream integration](../docs/upstream.md#usb-experiment-prerequisites).
Owner commands also select the next interface when their session closes:
Shell `host_mode pybricks` and the Brickphyr-specific Pybricks exit command.
The in-band round trip passes host simulation and Linux USB on the Inventor F4.
[Acceptance milestones](../docs/builds.md#acceptance-milestones) give the development
order from the existing build.

Everything tracked by Git is public documentation or project source. Keep local
development handoffs, device observations, experiment results, and pending session
work in the ignored `reports/` directory. Inspect those notes when available and
recheck current workspace/hardware state; they are not public project contracts
or continuing authorization to operate hardware.
