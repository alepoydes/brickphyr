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
updates remain unverified. The optional [Bluetooth controller profile](../docs/bluetooth.md)
passes initialization and Pybricks BLE identity/status, reconnect, return to Shell
and USB/BLE exclusion checks on the same Inventor F4. It now starts advertising
automatically; first BLE connection needs no USB preparation. Standard SMP BLE discovery,
fragmented echo and Settings read/write/save also pass. Application loading and
Python/WASM execution are not implemented yet. Current availability belongs in
the README and the owning subsystem documents.
Pybricks Code discovery, connection and reconnection pass through real Chromium
Web Bluetooth; the website warns about the newer profile version. See
[browser compatibility](../docs/bluetooth.md#pybricks-code) for the verified scope.
The local [HCI timeout correction](../docs/upstream.md#hci-timeout-prerequisite)
passes native checks and normal hub connections; radio-fault recovery remains open.
H:4 transport cleanup also passes UART-emulator checks and normal hub on/off cycles;
full host-stack cleanup after controller failure is still outstanding.

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
The local pybricksdev CDC transport reuses its existing command/event API and the
Python COBS codec shared with the flashing tool. Its identity CLI and reconnect
pass against the compiled Brickphyr handler over a PTY and over Linux USB on the
Inventor F4. Rejected commands leave the connection usable; the in-band return to
Shell works through the client. See the [client prerequisites](../docs/upstream.md#usb-experiment-prerequisites).
Runtime power settings use Zephyr Settings; independent USB/battery timeouts
default to disabled. PA9 detects USB power, and active USB/BLE ownership inhibits
expiry. Experimental LittleFS persistence and standard SMP Settings share the
runtime handlers. Explicit save, RAM-only changes, reboot restoration, file
compaction, and standard SMP clients pass native simulation and Linux USB checks
on the Inventor F4; see [settings](../docs/settings.md) and
[host connections](../docs/host.md). Policy boundaries pass host simulation.
USB power detection, runtime changes, and inhibition beyond a one-second timeout
are verified on the Inventor F4 with an open terminal.
USB idle shutdown disconnects USB and the operator observed lights off; subsequent
startup restores saved zero timeouts. Battery idle expiry, residual current
and charger/battery source transitions still need checking.
[Acceptance milestones](../docs/builds.md#acceptance-milestones) give the development
order from the existing build.

Everything tracked by Git is public documentation or project source. Keep local
development handoffs, device observations, experiment results, and pending session
work in the ignored `reports/` directory. Inspect those notes when available and
recheck current workspace/hardware state; they are not public project contracts
or continuing authorization to operate hardware.
