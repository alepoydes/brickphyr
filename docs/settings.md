# Settings and power policy

The F4 build implements runtime power settings through Zephyr Settings, USB VBUS
detection, and inactivity shutdown. Experimental persistence uses Zephyr's file
backend on LittleFS in external SPI flash. Changes apply in RAM; `config save`
persists them explicitly. Save, reboot restoration, and file compaction are
verified on the Inventor F4 over Linux USB through Shell and SMP. Inactivity
shutdown and subsequent startup are checked on USB and battery power, including
operator-observed lights off. USB/battery transitions preserve an active BLE
session and uptime; battery startup restores saved settings. These checks do not
measure residual current. The common contract below also covers future deferred settings.

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

The F4 settings partition occupies 32 KiB at external flash offset `0xb8000`,
inside Pybricks' user-storage region; see the [recovery constraints](recovery.md).
Zephyr SPI NOR discovers geometry through SFDP and uses four-byte address mode.
An entirely erased partition is initialized as LittleFS. Existing contents must
mount successfully with automatic formatting disabled; unknown or corrupt data
is reported, never replaced with a fresh filesystem. Storage failure leaves RAM
configuration available and prevents `config save` from claiming success.

Settings use `/settings/values`, with standard backend compaction. Saving several
keys is not atomic as a group. Reads report the actual stored value, so a partial
save cannot mark unrelated RAM changes as persisted. Loading passes known values
through the same runtime validation; incompatible lengths and truncated files
are errors. This requires the [dependency corrections](upstream.md#settings-storage-prerequisites).

## Current runtime settings

| Name | Type and units | Default | Effect |
| --- | --- | --- | --- |
| `power/battery_idle_s` | Unsigned 32-bit seconds | 0 | Battery inactivity timeout |
| `power/usb_idle_s` | Unsigned 32-bit seconds | 0 | USB-power inactivity timeout |

Zero disables the corresponding timer. Both defaults preserve indefinite power
retention during bring-up. All unsigned 32-bit values are accepted; binary Settings
values use four little-endian bytes. Invalid names, lengths, and shell values leave
the previous value intact. Selected and effective values are currently identical.

```text
config get power/battery_idle_s
config set power/battery_idle_s 300
config set power/usb_idle_s 60
config save
config get power/usb_idle_s
power
```

The commands distinguish the selected/effective RAM value from the saved value.
Reboot restores saved values, or defaults for missing keys. Without a working
storage backend, saving fails explicitly. An open terminal or Pybricks USB session
inhibits automatic shutdown even with a short timeout; explicit shutdown still works.

## Explicit power control

On F4, `shutdown` and a two-second central-button hold request the same shutdown
on the hub thread: turn off button lights, disconnect USB, stop Bluetooth and
release the PA13 power latch. Radio shutdown runs independently of the button
loop; a failed HCI operation cannot prevent the power latch from being released.
A held button may keep power on until released. An active host session
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
Future critical-battery protection must remain independent of automatic shutdown;
battery-voltage monitoring is not implemented in this build.

Diagnostics must expose the power source, configured timeout, remaining idle time,
and the reason shutdown is inhibited. `power` reports these values. The F4 reads
VBUS on PA9 independently of enumeration and DTR. Source changes, button presses,
changes to either timeout, and management acquisition/release restart idle time.
Polling shares the light/UI loop's 20 ms period. An unreadable power source
inhibits automatic shutdown and appears as an error in diagnostics.

Expiry and host acquisition serialize on the same lock: a live owner prevents
expiry, and a claimed shutdown rejects a later acquisition. No user application
runs yet; application activity must join this policy before an execution engine
is enabled. The optional Bluetooth profile acquires the same power hold. USB open/close
attempts rejected during BLE ownership cannot release it. These policy boundaries
are checked with synthetic time, including USB disconnect during an accepted
Shell command and BLE disconnect during an accepted SMP operation. Both retain
the power hold through completion. BLE inhibition beyond a one-second USB-power
timeout is also verified on the Inventor F4. With a three-second USB timeout,
closing the terminal releases
ownership and triggers shutdown, including USB disconnection and observed lights
off. The same terminal holds power beyond that timeout while open.

The native BLE terminal also holds battery power for over 13 seconds without
commands with a three-second battery timeout. USB removal and reconnection change
the reported source without interrupting BLE or resetting uptime. After a second
USB removal, closing BLE allows battery idle shutdown; the operator observed
lights off and subsequent button startup. Both RAM-only test timeouts return to
their saved zero values on startup. With the battery timeout disabled, the hub
continues advertising after the owner closes. These are functional checks on the
Inventor F4, not measurements of residual current or exact physical cutoff timing.

If USB keeps the processor powered after shutdown, the existing ADC button driver
continues running. A debounced center release followed by a new press performs a
full reboot; the shutdown hold itself, ADC errors and contact bounce cannot wake
the hub. No USB connection or menu is active in this state. USB-attached restart
after both command shutdown and a long center hold is checked on the Inventor F4.
Holding the button through shutdown does not restart it. Battery-only long-press
shutdown and subsequent button startup are also checked, followed by a working
BLE terminal and unchanged saved settings. These checks do not measure off-state
current or establish wake initiated by the USB host.

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
