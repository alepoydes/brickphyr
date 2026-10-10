# Settings and power policy

This is the selected behavior for user-configurable settings. It is not
implemented yet. The current F4 build holds power on and provides a diagnostic
shell and explicit power-off; it has no settings store or inactivity shutdown policy.

## Common settings contract

Use Zephyr Settings for loading and persistence. Each setting has one definition
of its name, type, units, default, valid values, and application conditions.
Shell commands and programmatic interfaces use the same validation and handlers.
Kconfig continues to select build-time features; environment variables are not
the persistent configuration store.

| Operation | Required behavior |
| --- | --- |
| Read | Show the selected value, whether it is saved, and any difference from the effective value |
| Change | Validate before mutation; update RAM and apply immediately when supported |
| Deferred application | Explicitly report that a subsystem restart or reboot is required; keep the effective value visible |
| Save | Explicitly persist the selected values, including values awaiting application |
| Startup | Load saved values; use defaults for missing settings; diagnose corrupt or incompatible stored data |

Changing a setting must not silently reboot the hub, stop an application, or
disconnect its owner. Reject a change that cannot meet its application contract,
or clearly report it as pending. Related values must be validated together when
an intermediate combination would violate an invariant.

A rejected value leaves the previous state intact. A failed save reports the
failure and leaves RAM changes marked unsaved. Saving does not itself apply a
pending change. In Zephyr Settings, commit means applying settings through their
handlers; it is distinct from saving to storage.

Expose effective, selected, and saved values through the same settings model,
without separate copies of the rules in each protocol. This does not require a
generic configuration framework or transactional storage for unrelated values.
Do not claim that saving several keys is atomic unless the chosen representation
and backend actually guarantee it.

The storage backend and partition remain to be selected under the
[recovery constraints](recovery.md). The current board description has no settings
partition and the build enables neither a flash driver nor external SPI storage.
Enabling Settings alone does not provide persistence on the hub.

## Explicit power control

On F4, `shutdown` and a two-second central-button hold turn off the button
lights, disconnect USB, and release the PA13 power latch. A held button may keep power on until released. An active host session
inhibits automatic inactivity shutdown, but does not veto an explicit power-off.
Neither power-off nor reboot implicitly saves settings.

`reboot` restarts Brickphyr. `reboot dfu` enters the existing second-stage loader
for installation through Pybricks. These are privileged diagnostic commands;
future application shutdown must quiesce application-owned devices before the
same final power operation. No application or motor driver runs in this build.

The physical effects of USB power, charging, and wake-up need separate tests;
USB disconnection alone is not proof that battery power was cut. Charging remains
disabled while the charger driver is absent.

## Inactivity shutdown

Inactivity means an idle hub with no running user application. Automatic shutdown
is a firmware responsibility and must not require a keep-alive user program.
The policy distinguishes three conditions:

| Condition | Policy |
| --- | --- |
| Battery operation without USB power | Configurable inactivity timeout, including disabling automatic shutdown |
| USB power present, including a charger without a host session | Separate configurable inactivity timeout, including disabling automatic shutdown |
| Active computer management session, over USB or Bluetooth | Inhibit automatic inactivity shutdown for the session's lifetime |

USB power presence, USB enumeration, and an active management session are distinct
states. Define session acquisition and release with the [host interface](host.md);
do not infer an active owner from the cable alone. After a session ends, apply the
relevant idle policy. Normal disconnection does not stop an autonomous application.
Critical-battery protection remains effective when automatic shutdown is disabled.

Diagnostics must expose the power source, configured timeout, remaining idle time,
and the reason shutdown is inhibited. Timeout defaults and the exact events that
restart the idle timer remain to be specified when the policy is implemented.

The first hardware objective is controlled inactivity shutdown and reliable power
retention for automated tests. CPU idle power saving is a separate mechanism.
USB wake or power-on after shutdown is a later hardware experiment: neither a
charging cable nor Zephyr power-management support establishes that capability.

## References

- [Zephyr Settings](https://docs.zephyrproject.org/latest/services/storage/settings/index.html)
  provides runtime handlers and persistent storage backends.
- [SMP settings management](https://docs.zephyrproject.org/latest/services/device_mgmt/smp_groups/smp_group_3.html)
  provides programmatic read/write, apply, load, and save operations.

These mechanisms must be checked against the pinned Zephyr version during
integration. They do not establish Brickphyr hardware support.
