# Workspace and builds

The experimental `spike_prime_f4` target builds a diagnostic firmware image.
Zephyr, CMSIS, and STM32 HAL revisions are resolved through [west.yml](../west.yml);
Python tools are locked in [uv.lock](../uv.lock). The SDK version comes from the
pinned Zephyr checkout's `SDK_VERSION`. No hardware installation has been verified.
Runtime profiles below remain selected designs.

## Repository layout

Use a west T2 workspace: Brickphyr is the manifest/application repository and
dependencies are independent Git checkouts. The workspace directory itself is
not a repository. A representative layout is:

```text
brickphyr-workspace/
  .west/
  brickphyr/             # this repository, including west.yml
  zephyr/                # upstream checkout, or a justified thin fork
  modules/
    pybricks/            # Pybricks checkout/fork with its original history
    ...                  # modules resolved by the selected manifests
babybot/                 # separate application repository
```

Do not put copied dependency trees into Brickphyr or make a second super-repository.
The present standalone checkout can be used as the manifest repository when the
workspace is created. See [west workspaces](https://docs.zephyrproject.org/latest/develop/west/workspaces.html).

## Dependency resolution

The manifest pins an immutable Zephyr commit and imports only CMSIS 6 and STM32
HAL from its manifest. No dependency patches or copied source trees are needed.
Keep this pattern as dependencies are added; pin any explicit overrides.
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

For a standalone checkout whose parent is not yet a west workspace:

```sh
uv sync --locked --group sdk
uv run --locked west init -l .
uv run --locked west update --narrow -o=--depth=1
uv run --locked --group sdk west sdk install --gnu-toolchains arm-zephyr-eabi --install-base ../toolchains
```

`west init -l .` makes the parent directory the workspace root. Dependencies are
siblings of Brickphyr, as shown above; the parent itself is not a Git repository.
Skip initialization for an existing workspace, and inspect dependency worktrees
before updating them. SDK installation uses the version required by the pinned
Zephyr checkout and downloads only the ARM compiler and host tools.

Build the source-derived Prime F4 target and check the resulting image:

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
is bundled. There is deliberately no `west flash` runner or firmware writer yet:
see [recovery](recovery.md) for the remaining identification and writer audit.

The firmware starts the standard Zephyr USB CDC ACM shell without waiting for a
terminal. Its intended hardware behavior is to keep battery power on and hold
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

`info` reports the **build target**, not an independently detected board revision.
The source revision identifies the base commit for uncommitted work; retain the
binary and its checksum when recording an experiment. The shell is a privileged
firmware diagnostic, not a user application or Python REPL. User mode and MPU are
enabled in the F4 build, but application isolation is not yet exercised.

After installation and USB enumeration are verified, pyserial's
`serial.tools.list_ports` and `serial.tools.miniterm` are available through
`uv run --locked python -m ...` to locate and open the CDC device. Local console
behavior can already be exercised with the native simulator; the exact build
and test commands are in [testing](testing.md).

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

The board bring-up milestone precedes application loading: install the image on
an identified revision, exercise the console and reboot, disconnect/reconnect the
host without resetting uptime, and restore the starting Pybricks firmware.
The current build and simulator checks complete only its local preparation.

For the subsequent native-application milestone, the same checkout and locked
dependencies must reproduce a board image and its SDK. The image must respect
[recovery](recovery.md); the SDK must load a separate
test application through the production loader. Add the actual commands and
examples here only after they run. A no-op firmware scaffold is not that milestone.
