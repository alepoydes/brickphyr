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
  pybricksdev/          # host library and CLI with pinned CDC support
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
for reproducible builds and checks. For everyday hub interaction, use the installed
[`brickphyrdev` CLI](host.md#host-cli) or `uv run brickphyrdev`. Firmware, SDK, and
Python package versions are different dependencies and must all be recorded where applicable.

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
pybricksdev dependency. The published 0.1.1 pins include that release's coordinated
[dependency changes](upstream.md#current-downstream-delta). The current hardware
and native-application increments additionally require the local prerequisites
listed in [upstream integration](upstream.md); publication and updated manifest
pins are still pending.

For a read-only summary of active manifest pins, checkout HEADs, branches and
dirty or missing projects (including Pybricks' MicroPython submodule), run
`uv run --locked python tools/workspace_status.py` from Brickphyr. The utility
reads local manifest and Git state; it does not fetch or change checkouts.

Build the Prime F4 target and check the resulting image:

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

The target selects the supported board from the build configuration and performs
no hardware operation. It also packages the limited Technic base below; H5 is
unsupported. Building a package does not verify recovery on hardware.

### Experimental Technic base

This builds the initial power/button/RGB/watchdog port for the four-port Technic
Hub 88012. The base image provides neither a Bluetooth terminal nor application
loading. There is no USB interface on this hub. Read the
[hardware scope](hardware.md#experimental-technic-base) and
[factory-loader boundary](recovery.md#technic-factory-loader-boundary) before
considering installation; no physical Technic checks have been performed.

```sh
uv run --locked west build -b technic_hub -d build/technic-base
uv run --locked west build -d build/technic-base -t firmware_package
```

The package is `build/technic-base/zephyr/firmware.zip`. Packaging requires the
[local metadata extension](upstream.md#technic-base-prerequisites); the board also
requires Zephyr's early chainload interrupt correction described there. The
image checker validates the main-firmware range, reserved loader-selector SRAM,
word alignment and space for the checksum word. Pybricks' package consumer adds
that word using its existing Technic checksum algorithm. Use the maintained
Pybricks BLE installer when hardware acceptance becomes possible; neither the
factory loader nor the CC2640 firmware is included in this package.

To compile the experimental CC2640 transport, initialization and GATT adapter as well:

```sh
uv run --locked west build -b technic_hub -d build/technic-npi -- \
  -DEXTRA_CONF_FILE='boards/technic_hub.conf;boards/technic_hub_npi.conf' \
  -DDTC_OVERLAY_FILE=boards/technic_hub_npi.overlay
```

This is a controller development profile. The controller remains in reset until
explicit initialization; advertising and host connections are not provided yet.
The overlay assigns SPI1 and ready/reset GPIOs to the driver. Its STM32 SPI path
uses interrupt completion with Zephyr's timeout instead of polling indefinitely.
Unused initialization routines may be removed by the linker; this profile's size
is not a full BLE endpoint resource measurement.

The optional BLE profile connects the factory CC2640 to the shared management
Shell, standard SMP BLE service and Pybricks identity endpoint:

```sh
CMAKE_BUILD_PARALLEL_LEVEL=4 uv run --locked west build -b technic_hub -d build/technic-bluetooth -t firmware_package -- \
  -DEXTRA_CONF_FILE=boards/technic_hub_bluetooth.conf \
  -DDTC_OVERLAY_FILE=boards/technic_hub_npi.overlay
```

Its package path is `build/technic-bluetooth/zephyr/firmware.zip`. This is a
build-only profile: no Technic board has been available for boot, BLE, installer
or recovery checks. The base and NPI-only profiles still hold the controller in
reset at boot; this profile initializes it. It does not enable native applications,
Python, ports or persistent program storage. See the
[Technic BLE boundary](bluetooth.md#experimental-technic-controller).

Add the native profile to enable protected RAM applications through the same
Shell/SMP handlers:

```sh
CMAKE_BUILD_PARALLEL_LEVEL=4 uv run --locked west build -b technic_hub -d build/technic-native -t firmware_package -- \
  '-DEXTRA_CONF_FILE=boards/technic_hub_bluetooth.conf;boards/technic_hub_native.conf' \
  -DDTC_OVERLAY_FILE=boards/technic_hub_npi.overlay
uv run --locked west build -d build/technic-native -t brickphyr-sdk
```

The artifacts are `build/technic-native/zephyr/firmware.zip` and
`build/technic-native/zephyr/brickphyr-sdk.tar.xz`. Use the
[SDK example build](#experimental-native-sdk) with this archive, or reuse an
F4 SDK ELF whose required services and memory fit. This profile reserves a
25 KiB shared heap, including its on-demand 2 KiB application stack and privileged
stack overhead; the loader budget is
limited to 16 KiB and uploads to 4 KiB. The upload and loaded program coexist
during loading; replacement briefly also retains the old loader buffer.
Use [BLE application commands](host.md#native-applications) for RAM upload,
start, output and stop. The shared IMU service is available through native calls,
`observe imu`, `observe imu.calibrated` and `observe imu.orientation`, or
`uv run brickphyrdev monitor --ble Brickphyr --source imu.calibrated`.
Calibration is runtime-only. The four ports support electrical identification
through `port`, `port A` and `observe port.A`; native applications read the same
snapshots with `bp_port_get()`. Passive medium/train motors additionally use the
shared [direct motor commands](ports.md#direct-motor-drive) and native API on A–D.
UART negotiation, encoder motor drive and regulated control remain disabled.
Battery voltage/current and connector-selected chemistry use `bp_battery_get()`,
`observe battery` and the BLE monitor; battery temperature is unsupported. See
[the board coefficients and limitations](hardware.md#hub-battery).
There is no Python or persistent program store in this profile.
It omits an unused libc malloc arena and floating
point text formatting in the firmware; the system/application heaps and hardware
float arithmetic remain enabled. Native SDK C-library formatting is linked into
each application independently. Compilation and the compact ARM fixture establish
build fit and shared loader behavior, not physical Technic operation.

### Diagnostic shell

The F4 firmware starts in the experimental Pybricks identification interface.
Select the standard Zephyr Shell from the computer before opening a terminal;
see [host connections](host.md). Boot never waits for a host. Its intended hardware behavior is to keep battery power on and hold
all six motor bridges in coast until an explicit [motor command](ports.md#direct-motor-drive). The
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
| `matrix [status]` | F4: display owner, completion/error status and last applied 5×5 image |
| `matrix fill LEVEL` | F4: set all pixels to brightness 0–100 |
| `matrix pixel ROW COLUMN LEVEL` | F4: set one pixel; row and column 0–4 |
| `matrix frame <25 levels>` | F4: write a row-major frame, brightness 0–100 per pixel |
| `matrix clear` | F4: clear a terminal-owned display and release it; an application owner is busy |
| `sound tone HZ MS VOLUME` | F4: square tone, 20–20000 Hz, duration in ms (0 continues), volume 0–100 |
| `sound [status]` | F4: speaker owner, playback, queue and errors |
| `sound stop` | F4: stop terminal-owned playback and release the speaker |
| `observe [all \| SOURCE]` | Read current hardware/application observations; see [sources and validity](telemetry.md) |
| `port [A..F]` | List connections or inspect a device's [advertised modes and latest data](ports.md#device-inspection) |

Matrix commands wait for the controller write and report its result. Closing a
terminal clears only its own display. For example:

```text
matrix fill 0
matrix pixel 0 0 100
matrix pixel 4 4 50
matrix status
matrix clear
```

See [matrix ownership and native API](hardware.md#hub-matrix).

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
uv run python -m serial.tools.list_ports -v
```

Find `Brickphyr bring-up console` (`2fe3:0004`) and open its device. For
`/dev/ttyACM0`, use:

```sh
uv run brickphyrdev usb select shell
uv run python -m serial.tools.miniterm --raw /dev/ttyACM0 115200
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
allow the power latch to switch off. A new short center press starts the hub,
including with USB left attached; see
[power behavior](settings.md#inactivity-shutdown). Left, right, center, and Bluetooth presses,
including combinations, appear in `buttons`. Press Bluetooth to open the
connection menu: its button lights white at full corrected duty and the central
button previews the selection. Either arrow cycles the two interfaces.
Release a short center press
to confirm; Bluetooth cancels. Center hold still switches power off. An occupied
interface cannot be displaced: confirmation briefly extinguishes the white BT
light and leaves the menu open. Outside the menu, the Bluetooth build flashes
this button blue once every three seconds while awaiting a connection. Connected Pybricks
uses the cyan reactor effect; the native terminal and SMP use yellow. A connected
client that has not yet used either profile shows steady blue. The base build and
a disabled radio leave the button off. See [connection lighting](hardware.md#f4-buttons-lighting-and-power).
The shared [5×5 matrix service](hardware.md#hub-matrix) provides terminal and
application access. An optional
[Bluetooth controller profile](bluetooth.md#experimental-f4-controller) provides
Pybricks BLE identification/status, SMP management and the same interactive Shell
with shared USB/BLE ownership. The Python profile adds
[Pybricks program execution](host.md#implemented-pybricks-subset).

For a short check, press each button briefly, then run:

```text
buttons
led center 0 16 0
led bluetooth 0 0 16
led bluetooth 0 0 0
```

`led center` stops only the central animation; `led bluetooth` overrides only the
Bluetooth button. Interface or menu changes restore automatic lighting, as does
reboot. A BLE state change also restores automatic Bluetooth indication.
RGB values pass through the shared correction; `led pwm` sets raw duty instead.
The base build reports device initialization errors through the shell. The optional
Bluetooth profile still has the [HCI timeout limitation](bluetooth.md#experimental-f4-controller).
The F4 build enables [charging](hardware.md#hub-charger) when the USB host or
detected source permits it, including after shutdown. Use `observe charger` and
`observe battery` to inspect its state. See [hardware](hardware.md) for physical verification and
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
| Native + Python | Current full F4 firmware | Native + MicroPython |
| WASM | Portable WASM plus native applications | Native + WAMR |
| Combined | Convenient capable-hub firmware, subject to measured capacity | Native + MicroPython + WAMR |

These are configuration combinations of one firmware implementation, not forks.
Do not publish every possible combination without a real use and corresponding
checks. Smaller hubs may support only a subset or remain unsupported.

Technic targets native ELF applications only, with the shared application
supervisor, hardware services, management Shell and MPU isolation. Its current
native profile is incomplete and build-only, a secondary goal after the primary
SPIKE/Babybot work. Python execution, including `.mpy`, is not planned for this
hub. The full F4 profile retains native applications and Python, including source
compilation, REPL and compatible `.mpy` modules. It also supports an independently scheduled native controller alongside Python
with a shared application lifetime; see
[concurrent control](applications.md#native-control-alongside-python).

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

### Memory diagnostics

Append `boards/spike_prime_f4_memory.conf` to the F4 build's existing
`EXTRA_CONF_FILE` list to enable Zephyr's kernel Shell and heap counters. For
example, the full Python profile uses:

```sh
uv run west build -b spike_prime_f4/stm32f413xx -d build/python-memory . -- -DEXTRA_CONF_FILE='boards/spike_prime_f4_bluetooth.conf;boards/spike_prime_f4_native.conf;boards/spike_prime_f4_python.conf;boards/spike_prime_f4_memory.conf'
```

Run `kernel heap` before loading, after loading and after application cleanup.
The peak counter persists until reboot; a fresh boot separates unrelated workloads.
Use `app status` for application stack/native-code figures and Python's
`gc.mem_free()`/`gc.mem_alloc()` for its own heap. See
[the memory boundaries](applications.md#experimental-python-profile).

This fragment also exposes upstream kernel commands such as `kernel reboot`,
which bypass Brickphyr's supervised shutdown. Use the ordinary `reboot` command
to preserve its application cleanup and user-data saving behavior. The diagnostic
fragment is not part of the normal Python profile.

## Acceptance milestones

These foundation contracts include already implemented work; they are not a
sequence to restart. The [platform roadmap](roadmap.md) owns current delivery
order and the native-control/Python extension. WASM is outside that milestone.

Continue from the existing F4 image, USB shell, image/package checks, and native
simulation. Packaging and shared hardware helpers are pinned to the public Pybricks
fork. Normal Pybricks restoration and Brickphyr reinstallation have been exercised
on the initial Inventor F4. USB interface selection also passes the Linux round
trip on that hub. Runtime power settings and idle policy are implemented; explicit
save, reboot restoration, compaction, and SMP management pass Linux USB checks.
USB and battery idle shutdown and restart are checked. USB/battery transitions
preserve an active BLE session and uptime; see [power verification](settings.md#inactivity-shutdown).
LEGO transitions and interrupted-transfer recovery remain separate checks.

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
5. **Add application management and a native application.** Implement the
   [native application milestone](#native-application-milestone): temporary runs
   from RAM and installed applications in LittleFS, using LLEXT and one lifecycle.
   USB and BLE provide the same operations through SMP, Shell and `brickphyrdev`.
6. **Integrate Python through that lifecycle.** Complete the remaining Pybricks
   client/API acceptance, cancellation and system responsiveness checks in the
   [Python plan](python.md). Preserve shared service ownership during native
   development. A future WASM engine must use the same lifecycle and services.

USB wake after shutdown is a separate experiment after reliable power control,
not a prerequisite for automated tests. Full compatibility and broader hardware
coverage grow from executable results. Add installation and application commands
only after they work; keep local measurements and handoffs in ignored reports.

## Native application milestone

The SDK/protected-loader, RAM transfers and LittleFS installation/run paths are
implemented experimentally and checked on the Inventor F4 over USB and BLE.
Dependency publication and a clean build from published pins remain release gates.
The goal is to build a separate native application, run it without writing flash,
or install it alongside other applications and start it after a power cycle.
The [application contract](applications.md#storage-and-launch-operations) owns
the shared semantics. Complete each step with an observable result:

1. **SDK and protected loading on F4.** Build a minimal separate C ELF with the
   matching SDK and load it through LLEXT in a user memory domain. Establish the
   entry/import contract, ARM floating-point ABI and MPU requirements using the
   actual board. The first example reads monotonic time, emits bounded output and
   returns. Verify load, execution and unload, and measure peak RAM and firmware
   overhead before choosing image limits. A board fixture may supply the ELF for
   this bounded loader experiment; it is not the end-user upload interface.
2. **Temporary runs over USB and BLE.** Receive an image into RAM through SMP and
   expose run, status, output and explicit stop through `brickphyrdev` and the
   shared Shell handlers. Extend the example to observe buttons and report a
   counter without taking over hub navigation. Verify both transports from their
   first connection after boot. Disconnect, reconnect using either transport and
   inspect/stop the same running application. Integrate application power holds
   and explicit power-off cleanup. Include bounded rejection and fault checks:
   incompatible/truncated images, interrupted upload, insufficient RAM, a
   non-yielding loop and an invalid memory access. The hub must remain manageable;
   repeated load/stop/unload must release owned resources.
3. **Persistent files and installation.** Establish the external-flash partition
   within the recovery constraints, then expose LittleFS file operations and
   standard SMP transfer on both transports. Upload, inspect, list and remove
   files through the existing filesystem mechanisms. Add staged application
   installation and complete-image validation. Verify persistence across reboot,
   capacity errors and preservation of the previous installed image after an
   interrupted update. Installation does not implicitly start or stop an app.
4. **Run installed applications.** Feed selected files into the same loader and
   supervisor used for RAM runs. Store at least two distinguishable applications,
   power-cycle the hub, then select and run each from the terminal and host CLI.
   Exercise temporary and installed runs over both USB and BLE, including
   cross-transport reconnection. Compare RAM use for the two sources and confirm
   that one application's cleanup permits the next to run without a hub reboot.

Keep fast regression checks on the production paths; do not test source text.
Use synthetic tests for malformed images and failure boundaries, and named-board
checks for MPU behavior, resource measurements, persistence and actual transports.
Long stress tests remain separate. Firmware, SDK and both example applications
must build from the same published dependency pins in a clean workspace before
the milestone is released.

Python/WASM execution, IMU/motor integration, automatic application startup and
a RAM filesystem are outside this milestone. Existing recovery, charging and
radio-fault limitations retain their own verification scope. The outstanding
visual BLE-indication check remains a separate small hardware check.

## Experimental native SDK

Native fixture builds require the [LLEXT build prerequisites](upstream.md#native-loader-prerequisites)
in addition to the current manifest pin. They are not yet part of a release.
With that Zephyr checkout, build the Bluetooth/native acceptance image:

```sh
uv run --locked west build -b spike_prime_f4 -d build/native-apps -t firmware_package -- \
  '-DEXTRA_CONF_FILE=boards/spike_prime_f4_bluetooth.conf;boards/spike_prime_f4_native.conf' \
  -DDTC_OVERLAY_FILE=boards/spike_prime_f4_bluetooth.overlay \
  -DCONFIG_BRICKPHYR_NATIVE_FIXTURES=y
uv run --locked west build -d build/native-apps -t brickphyr-sdk
mkdir -p build/native-sdk
tar -xf build/native-apps/zephyr/brickphyr-sdk.tar.xz -C build/native-sdk
uv run --locked cmake -S examples/native -B build/native-examples -G Ninja \
  -DLLEXT_EDK_INSTALL_DIR="$PWD/build/native-sdk/brickphyr-sdk"
uv run --locked cmake --build build/native-examples
```

Set `ZEPHYR_SDK_INSTALL_DIR` to the SDK installed during workspace setup. The
separate example build uses the exported SDK and toolchain, without compiling
the firmware again. Its [versioned ELF ABI](applications.md#portable-elf-abi)
is independent of Zephyr configuration; architecture, required services and
memory must still match the destination. The resulting `hello.elf` demonstrates time, output,
constructors and floating-point arithmetic; `counter.elf` reports a counter and buttons.
`matrix.elf` displays a smile until stopped. `imu.elf` reports calibrated
acceleration (µm/s²) and angular velocity (millidegrees/s) in hub axes.
`sound.elf` plays a short tone followed by generated PCM through the
[shared speaker service](hardware.md#hub-sound).
`control_frame.elf` reads IMU, battery and A/B encoder/output snapshots twice,
including original timestamps and errors, without claiming or moving devices.
See the [control input contract](applications.md#control-input-frame).
`control_group.elf` moves unloaded A/B motors at opposite 20% duty until a
100 ms absolute deadline. Clear both mechanisms before starting it; see
[grouped output semantics](ports.md#grouped-raw-motor-commands).
`control_group_coast.elf` uses the same code with passive outputs only. Both
retain their input, command and driver reports for `brickphyrdev app record`;
`control_record.elf` records observations without claiming either motor. See
[application recording](applications.md#application-recording).
`control_sample.elf` builds the same recording example with IMU-triggered
releases, retaining the exact sample and automatic control-step metadata. It
observes 40 distinct publications with a 100 ms per-wait timeout; it drives
neither motor. A stopped sensor fails the run instead of duplicating old data.
`control_actuation.elf` records 16 periodic A/B grouped commands and their
preceding group's driver outcome, then a final coast observation. It applies
opposite 20% duties until that coast, so clear both mechanisms before use.
`control_actuation_coast.elf` records the same sequence without energizing motors.
See [recorded periodic actuation](applications.md#recorded-periodic-actuation).
`motor.elf` moves an unloaded motor on port A at 200 degrees/second for 250 ms,
then coasts; application exit releases the port. Clear the mechanism before
explicitly starting this example.
`spin.elf` and `fault.elf` are
deliberate failure fixtures. Rebuild old experimental images using the portable SDK;
they contained configuration-specific syscall IDs. Compatible v1 builds can
reuse the new ELF without recompilation.

After [authorized installation](recovery.md), use either terminal transport:

```text
native_fixture hello
app status
app output
native_fixture counter
app stop
diagnostics
```

`diagnostics` reads and consumes retained firmware warnings and errors; overflow
counters remain visible. The default build filters at compilation, without
runtime log-level controls. It shares Zephyr's runtime message packager to reduce
flash use; formatting arguments costs more at the warning/error call site, while
output remains deferred. For a diagnostic build, raise `CONFIG_LOG_MAX_LEVEL`
and the relevant module's log level in an extra configuration file.

`native_fixture` loads an ELF embedded for board acceptance, not the separately
built host file. Use [the host CLI](host.md#native-applications) to upload those
files without rebuilding firmware. Fixture embedding can be disabled for ordinary
use. Read firmware flash and static RAM usage from the build's memory report;
these include the configured system heap and, for Python builds, a dedicated 64 KiB
Python arena. Application stacks and native loader buffers come from the
heap on demand and are released after the supervised application is quiescent.
The loader reports the image size,
reservation and loaded allocation in `app status`. During RAM loading, the receive buffer and loader reservation coexist;
the receive buffer is then released. File loading reserves no ELF receive buffer
and reports the same loader/loaded allocation for the same image.

### Optional CMSIS-DSP native SDK

Append `boards/spike_prime_f4_dsp.conf` to the F4 native configuration to enable
Zephyr's upstream `CONFIG_CMSIS_DSP` and `CONFIG_CMSIS_DSP_FILTERING`. Build the
firmware and export its matching SDK from the same configured directory:

```sh
uv run --locked west build -b spike_prime_f4 -d build/native-dsp-apps -t firmware_package -- \
  '-DEXTRA_CONF_FILE=boards/spike_prime_f4_bluetooth.conf;boards/spike_prime_f4_native.conf;boards/spike_prime_f4_dsp.conf' \
  -DDTC_OVERLAY_FILE=boards/spike_prime_f4_bluetooth.overlay
uv run --locked west build -d build/native-dsp-apps -t brickphyr-sdk
mkdir -p build/native-dsp-sdk
tar -xf build/native-dsp-apps/zephyr/brickphyr-sdk.tar.xz -C build/native-dsp-sdk
uv run --locked cmake -S examples/native -B build/native-dsp-examples -G Ninja \
  -DLLEXT_EDK_INSTALL_DIR="$PWD/build/native-dsp-sdk/brickphyr-sdk"
uv run --locked cmake --build build/native-dsp-examples --target imu_filter imu_record
```

The SDK adds `lib/libcmsis-dsp.a` and `LICENSES/CMSIS-DSP.txt` to Zephyr's EDK;
the EDK supplies the CMSIS-DSP headers and matching F4 hard-float flags. The
example links the archive normally, so only referenced objects enter its ELF.
The example build keeps application code at `-O2` and selects the toolchain's
size-optimized C library with `-Os` at link time. `imu_filter` additionally selects
Picolibc's integer formatter, including `long long`, because it prints integer
millidegrees/second. Remove that target's `vfprintf` selection if adding floating
point format specifiers; DSP arithmetic is unaffected by the formatter choice.
Other upstream `CONFIG_CMSIS_DSP_*` categories can be selected when an application
needs their routines. The full Python F4 profile takes the same optional file
after its Python configuration:

```sh
uv run --locked west build -b spike_prime_f4 -d build/python-dsp-apps -t firmware_package -- \
  '-DEXTRA_CONF_FILE=boards/spike_prime_f4_bluetooth.conf;boards/spike_prime_f4_native.conf;boards/spike_prime_f4_python.conf;boards/spike_prime_f4_dsp.conf' \
  -DDTC_OVERLAY_FILE=boards/spike_prime_f4_bluetooth.overlay
uv run --locked west build -d build/python-dsp-apps -t brickphyr-sdk
```

The `imu_filter.elf` example reads calibrated hub-axis Z angular velocity;
`imu_record.elf` enables bounded recording in the same source for
[desktop replay](applications.md#offline-inspection-and-numerical-replay). Its
recording buffer is allocated only when that application runs. The filter's
sampling and output limits are described in [applications](applications.md#experimental-native-profile).
The full F4 DSP firmware and matching SDK, and the example built separately from
that SDK, compile. The exported example also has ARM/QEMU RAM and LittleFS checks
through SMP with a synthetic IMU and Technic's application limits; see
[local checks](testing.md#local-checks).
The same SDK-built ELF has bounded Robot Inventor F4 RAM and stored-program
checks over USB/BLE, including output and explicit stop. See
[the native profile](applications.md#experimental-native-profile) for measured
application size, stack use and the remaining sensor/timing limits.

## Experimental Python profile

Add Python to the same Bluetooth/native configuration:

```sh
uv run --locked west build -b spike_prime_f4 -d build/python-apps -t firmware_package -- \
  '-DEXTRA_CONF_FILE=boards/spike_prime_f4_bluetooth.conf;boards/spike_prime_f4_native.conf;boards/spike_prime_f4_python.conf' \
  -DDTC_OVERLAY_FILE=boards/spike_prime_f4_bluetooth.overlay
```

The manifest-selected MicroPython submodule must be present. Its core and matching
`mpy_cross` target use one source revision; see [integration](../src/python/README.md).
Export the SDK from this same configured build to compile native `.mpy` modules:

```sh
uv run --locked west build -d build/python-apps -t brickphyr-sdk
mkdir -p build/python-sdk
tar -xf build/python-apps/zephyr/brickphyr-sdk.tar.xz -C build/python-sdk
uv run brickphyrdev compile script.py --sdk "$PWD/build/python-sdk/brickphyr-sdk"
uv run brickphyrdev app run script.mpy
```

`brickphyr-sdk.tar.xz` contains one `brickphyr-sdk` directory. Its `python/`
directory holds the matching `mpy-cross`, `compiler.json`, MicroPython headers,
`py/dynruntime.mk` and `tools/mpy_ld.py` for C native modules. `compile` defaults
to `script.mpy`; `--output` changes the path, and `LLEXT_EDK_INSTALL_DIR` supplies
the SDK when `--sdk` is omitted. To build a C `.mpy`, use upstream's
`dynruntime.mk` from the SDK with Make command-line variables
`MPY_DIR="$SDK/python" MPY_CROSS="$SDK/python/mpy-cross" ARCH=armv7emsp`;
the upstream architecture rules select the F4 hard-float ABI. Run Make in the
project's `uv run` environment to supply its Python/pyelftools dependency. Set `CROSS` to the
installed ARM compiler prefix when the default `arm-none-eabi-` is unavailable.
The SDK emits format-6 bytecode for ordinary Python and format 7.0 for native code.
Native files require this matching SDK; format-6 bytecode from ordinary Pybricks
clients remains accepted. Native format-6.3 files are rejected. The exported SDK's
ELF, native/Viper `.mpy` and dynamic C module have bounded USB/BLE execution checks
on the Inventor F4; ordinary SDK bytecode also passes through both pybricksdev
clients. See [execution coverage](applications.md#experimental-python-profile).

Installation uses the [existing recovery procedure](recovery.md). This profile
provides [temporary and installed Python programs](host.md#python-applications),
including imports, through Brickphyr management. REPL and the Pybricks execution
adapter have software and bounded Inventor F4 USB/BLE checks; remaining acceptance
is listed in the [plan](python.md).
This full profile includes [Classic HubNetwork](bluetooth.md#application-hub-network)
alongside BLE; the reduced profiles below omit its Classic stack. Physical
network acceptance and combined memory measurements remain pending.
The experimental [Pybricks library](compatibility.md#implemented-library-subset)
is enabled by default. `CONFIG_BRICKPHYR_PYBRICKS` selects that library;
`CONFIG_BRICKPHYR_PYBRICKS_EXECUTION` independently selects the USB/BLE client
execution adapter and defaults on with the library. Persistent client slots
require the adapter. Disabling it removes their worker and stack, reports zero
Pybricks execution capabilities, and rejects its application commands without
touching a native application's execution or output. Protocol identity and
connection-status queries remain available.

The library's device bindings follow the corresponding native Kconfig services.
Disabled features are excluded from its Python API and binding sources; see
[reduced-build compatibility](compatibility.md#reference-and-scope). This avoids
retaining unusable device objects in smaller images.

For native Python without either compatibility component, use:

```sh
uv run --locked west build -b spike_prime_f4 -d build/python-core-apps -t firmware_package -- \
  '-DEXTRA_CONF_FILE=boards/spike_prime_f4_bluetooth.conf;boards/spike_prime_f4_native.conf;boards/spike_prime_f4_python_core.conf' \
  -DDTC_OVERLAY_FILE=boards/spike_prime_f4_bluetooth.overlay
```

This profile retains source/bytecode execution, imports, named applications and
REPL through native USB/BLE management. `import pybricks` raises `ImportError`.
For native applications without the Python engine, omit the Python fragment:

```sh
uv run --locked west build -b spike_prime_f4 -d build/native-profile-apps -t firmware_package -- \
  '-DEXTRA_CONF_FILE=boards/spike_prime_f4_bluetooth.conf;boards/spike_prime_f4_native.conf' \
  -DDTC_OVERLAY_FILE=boards/spike_prime_f4_bluetooth.overlay
```

All three F4 profiles have compilation/package checks and bounded checks on one
Robot Inventor F4 (PCB revision unknown). Native-only USB/BLE checks cover
a maximum-size RAM image and a stored application with
80 KiB of writable data; see [native capacity](applications.md#experimental-native-profile).
The latest heap enlargement and early user-stack placement have compilation and
ARM emulation checks only; peak-load capacity on hardware remains to be checked.
Reduced Python checks cover USB/BLE
REPL reset/exit, named execution with imports, Shell launch and a temporary BLE
script. Rejected Pybricks commands preserve the running native Python program
and its output. The native-only profile runs a matching-SDK C example over USB
and BLE and rejects Python operations. Switching through these profiles and back
preserves stored programs; the named Python program runs again in the full image.
Native ELF applications require a compatible architecture/API and sufficient services
and memory; they no longer require the exact firmware configuration. These checks
do not establish every peripheral's behavior or peak-load capacity. The profiles
share the application supervisor and drivers.
