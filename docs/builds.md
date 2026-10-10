# Workspace and builds

The experimental `spike_prime_f4` target builds a diagnostic firmware image.
Zephyr, CMSIS, STM32 HAL, and Pybricks revisions are resolved through [west.yml](../west.yml);
Python tools are locked in [uv.lock](../uv.lock). The SDK version comes from the
pinned Zephyr checkout's `SDK_VERSION`. The current
[hardware results](hardware.md) cover installation and USB shell behavior on one
Inventor F4. Runtime profiles below remain selected designs.

## Repository layout

Use a west T2 workspace: Brickphyr is the manifest/application repository and
dependencies are independent Git checkouts. The workspace directory itself is
not a repository. A representative layout is:

```text
brickphyr-workspace/
  .west/
  brickphyr/             # this repository, including west.yml
  zephyr/                # upstream checkout, or a justified thin fork
  pybricks-micropython/  # Pybricks checkout/fork with its original history
  pybricksdev/          # host library and CLI; coordinated CDC topic during bring-up
  modules/
    hal/                 # CMSIS and STM32 HAL resolved by the Zephyr manifest
babybot/                 # separate application repository
```

Do not put copied dependency trees into Brickphyr or make a second super-repository.
The present standalone checkout can be used as the manifest repository when the
workspace is created. See [west workspaces](https://docs.zephyrproject.org/latest/develop/west/workspaces.html).

## Dependency resolution

The manifest pins immutable Zephyr, Pybricks, and pybricksdev commits and imports
CMSIS 6, STM32 HAL, and LittleFS from Zephyr's manifest. The firmware uses Zephyr modules;
the pinned Pybricks fork supplies host tools and shared button/LED helpers. See
[the downstream delta](upstream.md#current-downstream-delta). Keep dependencies
in separate repositories and pin any explicit overrides.
Do not put invented SHAs or floating `main` / `master` refs in a runnable manifest.

Python and WASM dependencies are not part of this bring-up build. Pybricks already
depends on MicroPython and other libraries. Resolve one intended
MicroPython source and its necessary submodules; do not accidentally build both
an imported upstream MicroPython and Pybricks' different fork. WAMR must likewise
have one owner in the resolved manifest. The reference firmware in
[hardware](hardware.md) is evidence, not automatic approval of all its dependencies.

Use upstream Zephyr CMake, Kconfig, devicetree, SDK, and west interfaces. Python
tooling uses `pyproject.toml` and a committed `uv.lock`; use `uv run --locked`
for reproducible project commands. Firmware, SDK, and Python package versions are
different dependencies and must all be recorded where applicable.

At a release, retain the resolved manifest, toolchain identity, build configuration,
partition map, resource report, and source licenses. Keep generated output in
`build/` or release artifacts, not in source control. Dependency development and
integration rules are in [upstream](upstream.md).

## Setup and board bring-up

The commands below run from this repository on Linux. Host prerequisites are Git,
uv, a C/C++ compiler, make, and GNU gperf 3.1 (supported by Zephyr's object-table
generator). CMake, Ninja, west, and the Python dependencies are installed into
`.venv` by uv.

To create a new workspace checkout:

```sh
mkdir brickphyr-workspace
cd brickphyr-workspace
git clone https://github.com/alepoydes/brickphyr.git
cd brickphyr
```

For a standalone checkout whose parent is not yet a west workspace:

```sh
uvx west init -l .
uvx west update --narrow -o=--depth=1
uv sync --locked --group sdk
uv run --locked --group sdk west sdk install --gnu-toolchains arm-zephyr-eabi --install-base ../toolchains
```

`west init -l .` makes the parent directory the workspace root. Dependencies are
siblings of Brickphyr, as shown above; the parent itself is not a Git repository.
Skip initialization for an existing workspace, and inspect dependency worktrees
before updating them. SDK installation uses the version required by the pinned
Zephyr checkout and downloads only the ARM compiler and host tools.
Bootstrap west fetches the sibling checkouts before uv resolves the editable
pybricksdev dependency. During the USB experiment, the coordinated
[dependency changes](upstream.md#usb-experiment-prerequisites) must be present
before synchronizing Python tools; the published pins alone remain insufficient.

`west update` checks out the published Pybricks helper changes from the pinned fork.
It does not publish or supply the pending USB/storage dependency corrections.
Build the Prime F4 target and check the
resulting image:

```sh
uv run --locked west build -b spike_prime_f4 -d build/prime &&
uv run --locked python tools/check_image.py build/prime/zephyr/zephyr.elf build/prime/zephyr/zephyr.bin
```

If gperf is installed outside `PATH`, pass its location at initial configuration:
`west build ... -- -DGPERF=/absolute/path/to/gperf`. A locally unpacked SDK can
similarly be selected with `ZEPHYR_SDK_INSTALL_DIR`. The image check examines the
ELF load addresses, binary contents, initial stack/reset vector, and the retained
bootloader's metadata. Passing it does not establish physical board compatibility.

The artifacts are `build/prime/zephyr/zephyr.{elf,bin,hex}` and the linker map in
the same directory. The raw binary contains only main firmware; neither bootloader
is bundled. There is no `west flash` runner or Brickphyr firmware writer:
see [recovery](recovery.md) for the remaining identification and writer audit.

### Experimental firmware package

Packaging uses the [Pybricks host-tool extension](upstream.md#current-downstream-delta)
included in the pinned fork. After building the F4 image:

```sh
uv run --locked west build -d build/prime -t firmware_package
```

This checks the linked F4 image and produces `build/prime/zephyr/firmware.zip`
using Pybricks' metadata generator. The package contains the main binary,
metadata, and component license texts. Hub-name customization is disabled so
the installer cannot patch a name into a Zephyr image. A short test opens the
package through the Pybricks reader and checks that the binary is unchanged.

The target packages only the existing F4 build and performs no USB operation.
It does not add H5 support. Building a package does not verify recovery on hardware.

### Diagnostic shell

The F4 firmware starts in the experimental Pybricks identification interface.
Select the standard Zephyr Shell from the computer before opening a terminal;
see [host connections](host.md). Boot never waits for a host. Its intended hardware behavior is to keep battery power on and hold
all six motor bridges in coast. The implementation has no motor commands. The
current USB descriptors use Zephyr's development VID/PID (`2fe3:0004`); a project
allocation is required before distribution. These IDs do not identify a physical
hub revision.

| Shell command | Result |
| --- | --- |
| `help` | Available diagnostic commands |
| `info` | Firmware version, source revision, Zephyr version, and build target |
| `uptime` | Monotonic milliseconds since boot |
| `reboot` | Cold restart through Zephyr's reboot API |
| `reboot dfu` | F4: reset into the installed mboot for the standard Pybricks installer |
| `shutdown` | F4: release the hub's power latch |
| `buttons` | F4: button states, press counters, latest ADC samples and error counts |
| `host_mode pybricks` | F4: select compatibility mode after this terminal closes |
| `bluetooth [on\|off]` | F4 BLE profile: report radio state, or enable/disable it until reboot |
| `power` | F4: power source, management owner, idle policy, and inhibition reason |
| `config get NAME` / `config set NAME SECONDS` | F4: read/change [runtime power settings](settings.md#current-runtime-settings) |
| `config save` | F4: explicitly persist settings; report unavailable or failed storage |
| `storage info` / `storage read` | F4: inspect or dump the settings partition without raw write access |
| `led center R G B` | F4: stop animation and set both central-button LEDs, corrected RGB 0–255 |
| `led bluetooth R G B` | F4: set the Bluetooth-button LED, corrected RGB 0–255 |
| `led pwm center R G B` / `led pwm bluetooth R G B` | F4: direct channel duty 0–65535, bypassing gamma and white balance |

RGB commands use gamma 2 followed by the board's white-balance coefficients.
The provisional F4 channel weights are `255:47:64`; see [color handling](hardware.md).
For example, these commands produce the same PWM for the central button:

```text
led center 255 255 255
led pwm center 65535 12079 16448
```

Both forms stop central animation when targeting `center`; Bluetooth writes leave
it running. A mode/menu change or reboot restores the animation. RGB commands now
describe colors; use `led pwm` for electrical duty experiments.

`info` reports the **build target**, not an independently detected board revision.
The source revision identifies the base commit for uncommitted work; retain the
binary and its checksum when recording an experiment. The shell is a privileged
firmware diagnostic, not a user application or Python REPL. User mode and MPU are
enabled in the F4 build, but application isolation is not yet exercised.

Run these commands from the Brickphyr repository. First list serial ports:

```sh
uv run --locked python -m serial.tools.list_ports -v
```

Find `Brickphyr bring-up console` (`2fe3:0004`) and open its device. For
`/dev/ttyACM0`, use:

```sh
uv run --locked --group flash python -m tools.host select shell
uv run --locked python -m serial.tools.miniterm --raw /dev/ttyACM0 115200
```

Replace `/dev/ttyACM0` if the port listing shows another device. Press Enter to
display `brickphyr> `. Type `info` to identify the firmware, or call `uptime`
twice to check that it advances. Exit miniterm with **Ctrl+]**. For a wireless session, use the
[native BLE terminal](host.md#native-bluetooth-terminal). After `reboot`,
select Shell and open the USB port again; its device name may change. Only one terminal may own it.

The shell uses Zephyr's standard line editing: Left/Right, Home/End,
Backspace/Delete, Tab completion, and Up/Down command history. History is held
in RAM for the terminal session. Closing the session clears its history and
unfinished input. `--raw` lets the host terminal interpret
the shell's cursor-control sequences without miniterm filtering them.

The F4 build softly fades in the central button over 1.2 seconds, then shows a
steady saturated color with continuous small brightness fluctuations and rare
surges: cyan for Pybricks, yellow for Brickphyr Shell and experimental SMP.
The yellow base uses
95% of the strongest channel, cyan 40%. This experimental reactor effect
indicates the selected interface, not a radio connection or DFU progress.
Interface colors blend over 300 ms. Brightness peaks clip individual channels,
which can shift the perceived hue. Both central LEDs update together with native
16-bit duty. See [lighting behavior](hardware.md#f4-buttons-lighting-and-power) for details.
Holding the central button for two seconds requests power-off; release it to
allow the power latch to switch off. Left, right, center, and Bluetooth presses,
including combinations, appear in `buttons`. Press Bluetooth to open the
connection menu: its button lights white at full corrected duty and the central
button previews the selection. Either arrow cycles the two interfaces.
Release a short center press
to confirm; Bluetooth cancels. Center hold still switches power off. An occupied
interface cannot be displaced: confirmation briefly extinguishes the white BT
light and leaves the menu open. In normal operation BT lighting is off.
The 5×5 display is not implemented. An optional
[Bluetooth controller profile](bluetooth.md#experimental-f4-controller) provides
Pybricks BLE identification/status and SMP management with shared USB/BLE ownership.

For a short check, press each button briefly, then run:

```text
buttons
led center 0 16 0
led bluetooth 0 0 16
led bluetooth 0 0 0
```

`led center` stops the animation; `led bluetooth` leaves it running. Interface or
menu changes restore automatic lighting, as does reboot.
RGB values pass through the shared correction; `led pwm` sets raw duty instead.
The base build reports device initialization errors through the shell. The optional
Bluetooth profile still has the [HCI timeout limitation](bluetooth.md#experimental-f4-controller).
Charging control is not implemented; the LED controller's charger-enable output
is held disabled. See [hardware](hardware.md) for physical verification and
[testing](testing.md) for local driver checks.

To update from this build, run `reboot dfu` in the hub terminal and wait for
the USB disconnect before exiting miniterm with Ctrl+]. Wait for the DFU device
to appear in `lsusb` before running the existing installer from the repository:

```sh
uv run --locked --group flash python ../pybricks-micropython/tools/flash build/prime/zephyr/firmware.zip
```

`reboot dfu` does not write flash. The installer handles the firmware package and
existing bootloaders. If the console is unavailable, independent factory DFU entry remains the recovery
path: disconnect USB and remove/reinsert the battery, then hold Bluetooth while
connecting USB and release it after the colored blink. See the
[recovery constraints](recovery.md) before installing.

## Firmware profiles

| Intended profile | Purpose | Engines |
| --- | --- | --- |
| Native | Small initial platform and native applications | Native loader |
| Python | Pybricks-compatible Python plus native applications | Native + MicroPython |
| WASM | Portable WASM plus native applications | Native + WAMR |
| Combined | Convenient capable-hub firmware, subject to measured capacity | Native + MicroPython + WAMR |

These are configuration combinations of one firmware implementation, not forks.
Do not publish every possible combination without a real use and corresponding
checks. Smaller hubs may support only a subset or remain unsupported.

The firmware advertises its actual engines and hardware capabilities. Application
format chooses an installed engine; the original programming language does not.
A Rust application compiled to WASM and one compiled to ARM are different formats.
An unsupported application receives a clear error, not an automatic reflash.

## Memory reporting

Report at least firmware flash, static RAM, system/application stacks, runtime
heap, loaded image and relocation memory, and peak loader scratch memory. Report
remaining application storage separately from executable memory. MPU alignment
and linker padding count. Heap size alone does not describe fragmentation or
largest loadable image.

Compare isolated and diagnostic non-isolated measurements when needed, but keep
the selected isolation policy explicit. Inactive engines should not each reserve
their maximum heap. The first resource figures must identify board, configuration,
compiler settings, and instrumentation.

## Acceptance milestones

Continue from the existing F4 image, USB shell, image/package checks, and native
simulation. Packaging and shared hardware helpers are pinned to the public Pybricks
fork. Normal Pybricks restoration and Brickphyr reinstallation have been exercised
on the initial Inventor F4. USB interface selection also passes the Linux round
trip on that hub. Runtime power settings and idle policy are implemented; explicit
save, reboot restoration, compaction, and SMP management pass Linux USB checks.
USB idle shutdown and restart are checked; battery expiry and source transitions
remain to be checked. LEGO transitions and
interrupted-transfer recovery remain separate checks.

1. **Package the existing image.** Integrate with the maintained Pybricks package
   and installation tools, resolve their metadata interface for a Zephyr image,
   check writer bounds, and prepare restoration artifacts. Reuse or extend the
   owning upstream tools; do not build a second flasher. See [recovery](recovery.md).
2. **Verify boot and restoration.** Identify the actual board and loader, establish
   independent recovery, then verify Pybricks → Brickphyr → Pybricks on authorized
   hardware. Exercise the existing console, cold boot, reboot, and USB reconnection
   without resetting battery-powered uptime. Keep motor outputs inactive. Verify
   the original LEGO transitions and interrupted-transfer recovery separately;
   the first ordinary cycle does not establish them.
3. **Evaluate USB interface selection.** Run the [host-interface experiment](host.md#usb-selection-experiment)
   on the working USB foundation: select Shell and Pybricks identification from
   the computer and return without buttons or reboot. Reject the mechanism if
   host-driver access or session ownership cannot meet the contract.
4. **Implement settings and power policy.** Start with actual power/session state,
   runtime settings, and controlled inactivity shutdown. Add a storage driver and
   a recovery-compatible partition before enabling explicit save. Verify effective
   values, invalid-input rejection, persistence across reboot, and physical power
   behavior under the [common settings contract](settings.md).
5. **Add application management and a native application.** Evaluate standard
   management/transfer mechanisms, with SMP as the preferred candidate. Use one
   lifecycle for Shell and protocol operations. Reproduce the firmware and SDK
   from the same checkout; load a separate application through the real loader.
   Verify output, stop, disconnect/reconnect, rejection, and fault cleanup before
   expanding to sensor and motor access.
6. **Integrate Python and WASM through that lifecycle.** Verify supported Pybricks
   client/API behavior and WAMR execution incrementally, including cancellation
   and system responsiveness. Preserve their shared service requirements during
   native development; neither engine is an incompatible later fork.

USB wake after shutdown is a separate experiment after reliable power control,
not a prerequisite for automated tests. Full compatibility and broader hardware
coverage grow from executable results. Add installation and application commands
only after they work; keep local measurements and handoffs in ignored reports.
