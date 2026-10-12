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

## Custom UART power

With `CONFIG_BRICKPHYR_PORT_UART`, `uart/power_allowed` controls admission of new
UART leases that request battery voltage on P1/P2. It is an unsigned 32-bit
little-endian boolean: 0 refuses, 1 allows, default 0. Python's first-use `Y`
acknowledgement and the native SDK setter change this same value in RAM.

```text
config get uart/power_allowed
config set uart/power_allowed 1
config save
```

Only explicit save persists permission. Setting it to 0 prevents subsequent
powered leases; it does not disconnect or remove power from an existing lease.
That lease retains its selected power through clear/baud changes and releases it
on application cleanup. The setting records acknowledgement of custom wiring;
it is not protection against an application already allowed to command motors.
See [UART behavior and evidence](ports.md#custom-serial-devices).

## IMU calibration

These settings are available with `CONFIG_BRICKPHYR_IMU`. Each value is signed
32-bit, stored as little-endian words. Related axis values form one setting and
are validated together before mutation. Defaults use Pybricks' calibration model.

| Name | Values and units | Default |
| --- | --- | --- |
| `imu/gyro_stationary_mdps` | Nonnegative angular-velocity variation threshold, millidegrees/s | 2000 |
| `imu/accel_stationary_um_s2` | Nonnegative acceleration variation threshold, µm/s² | 2500000 |
| `imu/gyro_bias_start_mdps` | Initial X/Y/Z gyro bias, millidegrees/s | 0 0 0 |
| `imu/gyro_scale_mdeg` | Measured X/Y/Z rotation per full turn, millidegrees | 360000 360000 360000 |
| `imu/accel_calibration_um_s2` | Measured acceleration with +X/+Y/+Z, then −X/−Y/−Z pointing up, µm/s² | +9806650 on positive axes; −9806650 on negative axes |
| `imu/heading_correction_mdeg` | Measured heading per full turn, millidegrees | 360000 |

Scale and heading values must lie strictly between 345000 and 375000. Each
acceleration value must be within 10% of the corresponding signed standard
gravity. Initial bias is limited to the sensor's representable rate. Thresholds
accept all nonnegative signed 32-bit values; conversion saturates at the sensor's
raw limit. Zero on either threshold disables stationary learning.

```text
config get imu/gyro_bias_start_mdps
config set imu/gyro_stationary_mdps 2000
config set imu/gyro_scale_mdeg 360000 360000 360000
observe imu.calibrated
config save
```

Changes apply in RAM immediately. The next sample starts a new orientation
reference and increments its `epoch`; the intervening snapshot reports `-EAGAIN`.
They do not stop an application. Bias changes also replace the current estimate.
Stationary learning subsequently refines the effective bias, shown by
`observe imu.calibrated`; `config get` shows the selected starting bias instead.
When no initial bias has been selected or loaded, the first three stationary
windows supply a starting bias in RAM. Only explicit save persists it. Existing
saved settings remain unchanged during subsequent learning.

Python `hub.imu.settings()` uses this same configuration. Its positional arguments
are angular-velocity threshold, acceleration threshold, heading correction,
gyro bias, gyro scale and acceleration correction. With no arguments it returns
thresholds, bias, scale, acceleration correction and heading correction, in that
order. Angles use degrees and acceleration uses mm/s²; acceleration correction
interleaves `+X, -X, +Y, -Y, +Z, -Z`. Selected values are rounded to the integer
resolution in the table above. An invalid field rejects the entire update.
Changes persist across application exit in RAM; only `config save` writes them
to flash. Axis selection and heading reset are application state, not settings.

These paths are checked with synthetic acquisition and real simulated
LittleFS/Settings. Physical calibration accuracy is not yet verified.

## Explicit power control

On F4, `shutdown` and a two-second central-button hold request the same shutdown
on the hub thread: stop applications and hardware outputs, turn off button lights,
stop Bluetooth and release the PA13 power latch. With charger support, USB stays
configured to retain its charging current budget; other profiles disconnect USB.
Radio shutdown runs independently of the button
loop; a failed HCI operation cannot prevent the power latch from being released.
A held button may keep power on until released. An active host session
inhibits automatic inactivity shutdown, but does not veto an explicit power-off.
Neither power-off nor reboot implicitly saves settings.
The separate [application user-data region](applications.md#persistent-user-data)
follows Pybricks' save-on-shutdown behavior. `storage save data` saves those bytes
explicitly; it does not save settings.

`reboot` restarts Brickphyr. `reboot dfu` enters the existing second-stage loader
for installation through Pybricks. These are privileged diagnostic commands;
application shutdown quiesces application-owned devices before the same final
power operation.

USB disconnection alone is not proof that battery power was cut. The
[USB-powered off state](#inactivity-shutdown) describes charging and button restart;
[battery support](hardware.md#hub-battery) records charging limits.

## System watchdog

The F4 build enables `CONFIG_BRICKPHYR_WATCHDOG`. Zephyr Task Watchdog supervises
the hub loop and system workqueue; the STM32 independent watchdog (IWDG) resets
the MCU if interrupts or the kernel stop servicing the software watchdog. This
does not depend on an open USB/BLE connection or on a user program yielding.
It is not a replacement for application isolation, explicit stop or motor
deadline handling.

Run `watchdog` in either terminal to read its startup status, configured progress
deadline and captured reset flags. A hardware expiry reports `watchdog`; a task
watchdog expiry uses a software reset and reports `software`. Software resets
also include ordinary reboot, so that flag alone does not identify the failed
service. Reset flags are captured and cleared once at watchdog initialization.

Initialization has a 10-second progress budget, starting immediately after the
IWDG device is ready at POST_KERNEL priority 51. Bootloader, clock and earlier
device initialization precede this protection. On entry to the hub loop, the
budget becomes 3 seconds. The loop normally runs at intervals of at most 20 ms;
system-queue progress is checked every 750 ms. These are recovery deadlines, not
measured worst-case execution guarantees. During synchronous PF cleanup, a
completed coast transmission counts as hub-loop progress; merely polling or
waiting does not. Only the thread that runs the hub loop may refresh its watchdog,
so progress in a management/device thread cannot hide a stalled hub loop. Other
long operations must stay outside those paths, expose actual progress, or provide
a justified revised budget.

The hardware fallback is configured for 2 seconds at nominal LSI frequency and
is refreshed at least every second while software supervision works. The F413
datasheet specifies LSI from 16.1 to 47 kHz around a nominal 32 kHz: the resulting
hardware interval is approximately 1.36–3.98 seconds. The extra margin prevents
the faster oscillator from expiring before the software refresh interval.
See [ST's F413 datasheet, table 46](https://www.st.com/resource/en/datasheet/DM00282249.pdf).

After shutdown, USB may still power the MCU. The off loop continues supervision
while waiting for release and a new central-button press; the IWDG cannot be
disabled without reset. DFU entry resets the MCU before entering the existing
loader, before Brickphyr arms its watchdog again. Reset starts with no application
running and motor outputs inactive. Stored applications are not launched
automatically.

One Robot Inventor F4 has passed a one-shot interrupt-lock expiry and a stalled
system-workqueue reset, with USB console recovery and inactive A/B motor outputs.
DFU/reinstallation with watchdog armed is checked. Native simulation also covers
hub-loop starvation and supervision throughout the USB-powered off state.
Command shutdown and button restart with watchdog enabled are checked with USB
attached, including the charging profile. Battery-only watchdog recovery remains
unverified. These checks do not establish recovery from damaged boot code
or interrupted firmware writes. See [watchdog checks](testing.md#watchdog).

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
Critical-battery protection is independent of these inactivity rules. On F4,
filtered voltage at or below 6.0 V sets the critical condition, which clears at
6.8 V. Three seconds continuously critical requests the normal shutdown and
resource cleanup, even with an application or management session. Low-voltage
warning sets at 6.8 V and clears at 7.2 V. These Pybricks thresholds are not
user-adjustable inactivity settings. A read error retains an already established
critical condition; startup waits for a real measurement. `observe battery`
reports the values and acquisition errors. Synthetic ADC checks cover hysteresis,
recovery, error retention and shutdown despite both power holds; deliberate
physical battery depletion is not an acceptance procedure.

Diagnostics must expose the power source, configured timeout, remaining idle time,
and the reason shutdown is inhibited. `power` reports these values. The F4 reads
VBUS on PA9 independently of enumeration and DTR. Source changes, button presses,
changes to either timeout, and management acquisition/release restart idle time.
Polling shares the light/UI loop's 20 ms period. An unreadable power source
inhibits automatic shutdown and appears as an error in diagnostics.

Expiry and host acquisition serialize on the same lock: a live owner prevents
expiry, and a claimed shutdown rejects a later acquisition. A running native
application holds power until it stops or finishes. The optional Bluetooth
profile acquires the same power hold. USB open/close
attempts rejected during BLE ownership cannot release it. These policy boundaries
are checked with synthetic time, including USB disconnect during an accepted
Shell command and BLE disconnect during an accepted SMP operation. Both retain
the power hold through completion. BLE inhibition beyond a one-second USB-power
timeout is also verified on the Inventor F4. In a profile without charging, a
three-second USB timeout is checked to release ownership, disconnect USB and
turn the lights off after the terminal closes. The same terminal holds power
beyond that timeout while open.

The native BLE terminal also holds battery power for over 13 seconds without
commands with a three-second battery timeout. USB removal and reconnection change
the reported source without interrupting BLE or resetting uptime. After a second
USB removal, closing BLE allows battery idle shutdown; the operator observed
lights off and subsequent button startup. Both RAM-only test timeouts return to
their saved zero values on startup. With the battery timeout disabled, the hub
continues advertising after the owner closes. These are functional checks on the
Inventor F4, not measurements of residual current or exact physical cutoff timing.

With charger support enabled, shutdown closes existing management sessions and
stops applications, motors, sensors, Bluetooth and lighting, while keeping the
USB device configured. This retains the computer's current budget for charging;
USB presence after shutdown does not mean a working terminal or running program.
Removing USB disables charging. USB suspend also pauses charging, and resume or
reconfiguration can restore it. Other profiles retain USB disconnection at shutdown.
On the Inventor F4, command shutdown leaves USB configured, rejects both old and
new terminal input, turns off the lights, and permits center-button restart.
Charge-current observation resumes after restart. Current while off and a full
charge cycle have not been measured with instruments.

If USB keeps the processor powered after shutdown, the existing ADC button driver
continues running. A debounced center release followed by a new press performs a
full reboot; the shutdown hold itself, ADC errors and contact bounce cannot wake
the hub. No management session or menu is active in this state. USB-attached restart
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
