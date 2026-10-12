# Working with upstream repositories

This workflow is part of the project contract. Reuse is incomplete if a local
change cannot be reproduced, reviewed, and returned to its source project.

## Repository roles

Brickphyr owns its application lifecycle, integration, build configuration, SDK,
and board support that is not yet upstream. Dependencies remain ordinary Git
repositories managed by west; see [workspace layout](builds.md).

Start with upstream Zephyr and out-of-tree board/driver support. Create a Zephyr
fork only when a required change cannot live outside its tree. Maintain a thin
Pybricks fork for changes that independently benefit Pybricks. A fork keeps the upstream
history and source layout; it is not a copied subtree with a new root commit.
Do not create an independent PBIO repository before the integration demonstrates
that extracting it would help both consumers and the upstream maintainers agree.

In a working fork, `origin` is our fork and `upstream` is the original project.
For an unmodified dependency, its normal upstream remote is sufficient. The
Pybricks fork is [alepoydes/pybricks-micropython](https://github.com/alepoydes/pybricks-micropython),
with [pybricks/pybricks-micropython](https://github.com/pybricks/pybricks-micropython)
as upstream. The other integration forks are
[alepoydes/zephyr](https://github.com/alepoydes/zephyr) and
[alepoydes/pybricksdev](https://github.com/alepoydes/pybricksdev), preserving their
respective upstream histories. All three publish branch `brickphyr/0.1.1`.

## Put the change in its owning project

| Change | Expected home |
| --- | --- |
| Generic Zephyr driver, kernel, or LLEXT improvement | Zephyr, with its required tests |
| LEGO protocol, calibration, or control improvement useful to Pybricks itself | Pybricks, independent of Zephyr integration |
| Reusable firmware packaging, hub identification, installation, or restoration extension | The existing Pybricks host-tool repository owning that operation |
| Missing reusable MicroPython/WAMR behavior | That runtime's project |
| Brickphyr lifecycle, SDK, glue, application storage policy, Zephyr-specific ports | Brickphyr |

A portable change and its OS-specific integration should be separate commits or
PRs. Explain the upstream use case without requiring the reviewer to adopt
Brickphyr. Follow that project's license, style, and contribution requirements.

Reuse unchanged dependency sources directly when their interfaces fit. If an
adaptation serves Brickphyr alone, port the necessary code here under its existing
license instead of changing the dependency's architecture for our needs. Retain
copyright and license notices, record the source path and exact revision, and
describe the substantive differences beside the port. Compile one implementation
of each function into the firmware. Review relevant upstream fixes when updating
the source version; a port is maintained code, not an automatically updated copy.
Do not copy entire dependencies or create another HAL to keep both execution
models. Upstream acceptance is not assumed, nor required for Brickphyr's progress.

For firmware installation, first use the existing Pybricks tool and package
interface directly. Add dependency changes only for a demonstrated missing
capability; keep Zephyr build integration in Brickphyr. Original LEGO firmware
is a required starting state, as specified in [recovery](recovery.md).

## Developing a dependency change

Before `west update`, inspect dependency worktrees and preserve local work on
named branches. West controls `manifest-rev` and normally leaves detached HEADs;
never develop on that branch or leave new work reachable only from detached HEAD.
Do not update a dirty dependency automatically to satisfy a build script.

For example, after configuring `origin` as a writable Pybricks fork and `upstream`
as the original project, these commands run from the workspace root:

```sh
git -C pybricks-micropython fetch upstream
git -C pybricks-micropython switch --create topic/port-wait upstream/master
```

The branch name is illustrative. Choose the actual target upstream base required
by the change; an older maintained release may be appropriate. Develop and commit
there, then inspect the portable delta:

```sh
git -C pybricks-micropython diff upstream/master...HEAD
git -C pybricks-micropython log --oneline upstream/master..HEAD
```

Run the affected upstream checks and build, then the Brickphyr integration checks
for that commit. A change intended for both consumers must not be validated only
against Brickphyr. See [testing](testing.md) for evidence scopes.

Publish the topic branch to the fork and submit it according to upstream's rules
when publication is authorized. For several dependent patches, keep a published
integration branch assembled from the topic branches. Do not make an unreviewable
rewrite of the upstream tree.

## Pinning and accepting changes

1. Make the chosen dependency commit reachable in a repository available to
   every developer using the manifest. Keep private experiments out of shared pins.
2. Update Brickphyr's manifest to the exact commit in a separate integration
   change. Record why the upstream revision is insufficient and link the topic/PR.
3. Resolve the workspace afresh and check the affected upstream and Brickphyr
   paths. Record resource, compatibility, and hardware implications as applicable.
4. When upstream accepts the change, find the commit/release containing equivalent
   behavior. Review squash/rebase/conflict resolutions; matching titles are not
   proof that the code is identical.
5. Move the pin to the accepted upstream revision, rerun the relevant checks,
   and remove the downstream workaround or duplicate patch from active integration.

Use merges on published integration history. Rebasing an unpublished topic for
review is normal, but do not force-rewrite commits referenced by a published
manifest or release. Preserve release tags and dependency commits. There is no
requirement to merge every upstream change immediately; updates are reviewable
changes with a concrete benefit and validation.

Manifest imports own transitive versions. Overrides need a reason; do not maintain
the same revision in west, a shell script, and a prose installation command.

## Current downstream delta

The [manifest](../west.yml) pins published Zephyr, Pybricks and pybricksdev
integration commits and imports Zephyr's module revisions. The board lives out of
tree in Brickphyr. Pybricks supplies host tools and portable resistor-ladder/TLC5955 helpers.
The firmware adapters use Zephyr ADC, Input, SPI, PWM, GPIO, and LED APIs.

The pinned Pybricks fork includes these changes on the upstream v4.1.0b6 base:

| Owning file | Change and reason |
| --- | --- |
| `tools/metadata.py` | `--no-custom-name` permits checksum-free images without Pybricks-specific linker sections |
| `lib/pbio/include/pbdrv/resistor_ladder.h`, `drv/resistor_ladder/` | Extract the existing decoder from ADC access for reuse with Zephyr |
| `lib/pbio/include/pbdrv/tlc5955.h`, `drv/pwm/pwm_tlc5955_stm32.c` | Share control-frame definitions and grayscale encoding without the STM32/PBIO scheduler adapter |
| `lib/pbio/src/cobs.c` | Reject truncated blocks and reserved wire bytes before protocol dispatch |
| `tools/flash/serdev/cobs.py` | Reuse the Python COBS codec from pybricksdev |
| `tests/tools/` | Short metadata/package checks and factory-loader preservation checks through the unchanged DFU writer |

These changes are published in
[the integration commit](https://github.com/alepoydes/pybricks-micropython/commit/a7b1b04ae4f8449dd46b3dfb97c420334439fd35).
`west update` obtains them through the manifest without manual patches. The
installer and its DFU implementation remain unmodified. No upstream PR has been
submitted yet; remove the downstream delta when equivalent upstream functionality
is adopted.

The pinned [Zephyr integration commit](https://github.com/alepoydes/zephyr/commit/29bbb539f73adee89fed53fef4b94d59c97962bf)
is based on v4.4.2. The pinned
[pybricksdev integration commit](https://github.com/alepoydes/pybricksdev/commit/c970fb90e9c1027bdde93b5fb2806dd34008897b)
supplies the CDC client and shared Python COBS codec. The corrections below are
included in these public pins; no manual dependency patches are required.

## Watchdog prerequisites

The F4 watchdog integration additionally requires two local Zephyr corrections,
not yet present in the manifest pin:

- Task Watchdog must return a failed hardware `wdt_setup()` result and release
  the uninstalled channel. Otherwise callers can report protection when setup
  actually failed. The upstream test in `tests/subsys/task_wdt/setup` exercises
  failure, retry and unchanged channel capacity through the real subsystem.
- STM32 IWDG register-update polling must use a cycle-based timeout, because
  Task Watchdog starts the device with interrupts locked. Kernel uptime alone
  cannot bound a missing register update in that context. The F4 build and
  hardware expiry check exercise successful setup; missing hardware register
  updates remain source-reviewed, not physically injected.

These changes belong to the Zephyr fork; Brickphyr reuses its watchdog interfaces.
Publish the dependency correction before moving the shared manifest pin.

## USB experiment prerequisites

The experimental USB path uses these portable dependency changes:

- Pybricks COBS decoding must reject truncated blocks and reserved wire bytes.
- Zephyr's next-stack CDC ACM driver must implement `uart_irq_tx_complete()`,
  including pending USB packets and the terminating zero-length packet. A FIFO
  alone cannot establish that the previous protocol's output has drained.
- pybricksdev needs a Profile 1.6+ CDC transport, tagged replies, characteristic
  reads, and bounded exchange/cleanup. Its existing command and event handlers
  remain shared with BLE and the older USB transport. The Python COBS codec moves
  from the Pybricks flashing tool to `pybricksdev.usb.cobs`; the flashing tool
  imports that single implementation. The manifest and editable uv dependency
  keep the coordinated client/tool changes together.

These changes remain in their owning repositories with focused tests. Do not
substitute timing delays or inspect private driver buffers in Brickphyr. West
fetches the pinned pybricksdev sibling checkout; uv installs it as an editable
dependency. The released upstream Python package does not yet supply this CDC
client, so install the CLI through the documented workspace procedure.

## Python runtime prerequisites

The MicroPython checkout has a local `readline_init0()` correction that clears
the editor state as well as history. Otherwise a carriage return from a stopped
REPL can pair with, and suppress, the first newline supplied to a new program.
Brickphyr calls this public initialization API for every fresh interpreter,
including soft reset. The correction belongs to MicroPython, not a second line
editor in Brickphyr. Its Unix coverage case checks CR/LF pairing within a session
and independent input after reset; the ARM application test reproduces the
REPL-to-script transition. This dependency change is not yet published or pinned.

## Python library prerequisites

The Pybricks checkout has local corrections used by the experimental Python
library. Its builtin-subpackage configuration now retains the existing
`pybricks.geometry` alias. Task cancellation attempts every unfinished task even
if one `close()` raises, retaining the original operation error or first cleanup
error. `run_task` also closes its iterator when an interrupt arrives between
iterations. The portable `multitasking/cleanup_error.py` case covers exception
preservation and complete, idempotent cancellation; Brickphyr additionally checks
external interrupt delivery through the protected application path.

The float-to-integer argument helper now uses MicroPython's checked conversion,
preserving ordinary truncation while rejecting NaN, infinity and overflow before
hardware operations. Its `color/invalid_float.py` case checks the common helper
without requiring devices. The original unchecked cast allowed NaN brightness
through the ARM display binding; that regression is also checked in Brickphyr.

Matrix formatting now uses MicroPython's float printer instead of clipping every
element to ±999.999 or converting nonfinite/large values to integers. It streams
rows without allocating a second matrix-sized buffer. `tools/matrix_print.py`
checks gravity-scale values, transposition, scaling and nonfinite/large values
through the same public representation. Transposed iteration now visits logical
rows in order; previously it reversed the entire backing array, including vectors.

These corrections independently improve Pybricks and remain in that repository.
They are not yet published or included in the manifest pin. Brickphyr-specific
clock, lifetime and device adaptations remain in Brickphyr.

## Python client prerequisites

The optional Python profile additionally needs local pybricksdev changes not yet
included in the manifest pin. Its existing `download()` path now recognizes the
native-v6.3 capability, selects the official `mpy-cross-v6.3` package and uses the
`armv7m` target for Prime/Inventor source decorators. The multi-module packager
includes precompiled modules and package initializers, validates their ABI and
uses its existing MicroPython disassembler to follow imports. The v5/v6 bytecode
paths remain available. This does not establish support for older native ABIs.

The client also leaves the slot argument absent when `start_user_program(None)`
requests the hub's current selection. Substituting the client's cached selection
can start the wrong slot before the first status event arrives. This correction
is local commit `b67efa0` in the pybricksdev fork and applies to all transports.

Client tests cover compiler selection, precompiled imports and rejected headers;
Brickphyr checks execution with the actual serial client on protected ARM
emulation. Both repositories' uv locks resolve the added compiler package.
Publish the dependency change before moving the shared manifest pin. Both actual
clients now upload and repeat the hub example over USB/BLE on the Inventor F4;
native/Viper imports and a dynamic C module also execute through both clients.
Selection immediately after connection and stored execution after software reboot
have F4 USB/BLE checks. Broader client acceptance remains open.

## Settings storage prerequisites

The experimental persistent-settings build also requires a Zephyr file-backend
correction in the pinned integration: preserve read errors through
file close and reject short values whose stored length says more data exists.
The regression checks use real filesystem operations with truncated Settings
records. The correction and its tests remain in Zephyr.

## SMP prerequisites

The experimental Shell/SMP path uses a Zephyr transport change included in the
manifest pin. The common serial receiver exposes packet extraction and
session reset to custom Shell backends, so Brickphyr can reuse its framing while
joining accepted operations during disconnect. It also rejects oversized lines
and clears rejected header prefixes. The default UART transport remains available;
its input-expiration timer is specific to that backend and disabled for custom
receivers. Portable receiver checks cover both configurations.

The manifest imports zcbor through Zephyr's pinned module list. Host tooling uses
the locked, unmodified `smpclient` and `smpmgr` packages. The receiver implementation
and its checks remain in Zephyr.

BLE SMP also needs transport packet admission/completion callbacks. An accepted
packet keeps its session resources until handlers, response output and cleanup
finish; denied packets never reach a handler. The standard Bluetooth transport
exposes a public connection callback pair, holds the connection reference through
processing, rejects obsolete sessions and clears incomplete/queued packets on
disconnect. Brickphyr uses this boundary to retain the shared management slot
without blocking Bluetooth RX. Focused Zephyr tests cover admission, malformed
input, allocation/output failures and exactly-once completion. The callbacks are
part of the pinned Zephyr integration.


## HCI timeout prerequisite

The Bluetooth profile uses the pinned Zephyr command-lifetime correction.
Synchronous HCI commands return `-ETIMEDOUT` on a missing completion instead of
asserting. Completion and timeout arbitrate access to the
caller's semaphore and state update; a late reply cannot use an expired stack.
Commands still queued at timeout are removed without restoring controller credits.
Buffer allocation inside the synchronous API is also bounded.

The Zephyr native checks use a synthetic HCI controller and the production command
path to cover completion/error replies, late replies, repeated queued cancellation,
buffer exhaustion, and calls from the system workqueue. Both TX scheduling options
are exercised. This is a transaction-lifetime change, not automatic controller
recovery; other Bluetooth APIs can still wait on their own resources.

The H:4 transport also needs its pinned close-path correction: release partial RX,
pending TX and queued buffers even if board teardown fails, and reset framing
before reopening. Transport setup preserves its original error. Native tests use
the production driver and an emulated UART to exercise partial and oversized
packets, pending transmission, teardown failure and repeated open/close. This is
transport cleanup, not complete host-stack recovery after controller failure.

The pinned disable-error correction adapts
[upstream's readiness fix](https://github.com/zephyrproject-rtos/zephyr/commit/709a1b354b88)
to the 4.4.2 lifecycle: reject a missing driver close operation before teardown,
and never restore readiness after a failed close. It retains this base's
`-ENOTSUP` result and failure latch until reboot; a rejected enable cannot clear
that latch. Newer upstream supports retry with additional lifecycle changes that
are not part of this backport. Synthetic HCI tests cover both missing close and
close failure; normal on/off operation is checked on the Inventor F4.

## Native loader prerequisites

The native fixture build requires two portable corrections beyond the current
manifest pin: `add_llext_target` must pass architecture/multilib options to the
compiler-driven link and depend on the produced binary when copying it for
packaging. Otherwise Cortex-M compiler helpers can come from an ARM-state library,
or an incremental build can retain an obsolete application image. They belong in
Zephyr's `cmake/modules/extensions.cmake`, not a replacement Brickphyr builder.

These corrections are on Zephyr's local `topic/llext-build` branch at
`eb9551a2`; the manifest remains on the published integration until this dependency
change is published. The relocatable upstream LLEXT suite passes on ARM QEMU;
the MPU object suite passes its three applicable checks and skips eight images
requiring more memory partitions. Brickphyr additionally exercises linked
soft-float helpers and repeated protected loading on the Inventor F4.

Installed application support additionally requires local Zephyr commit `d1fdd0b7`
(on the same branch): `fs_mgmt_file_close()` exposes the standard file group's
existing transfer cleanup. An application can close an incomplete transfer before
validating and atomically publishing its staging file without accessing private
MCUmgr state. The API retains filesystem errno, and the wire close command now
returns an encoded group error instead of falsely reporting an encoding failure.
A native simulator test exercises the actual SMP handler with an injected
filesystem close failure; Brickphyr tests use real LittleFS. This commit is also
pending publication; keep both prerequisites before building the native profile.

Native applications also require upstream's
[ARM syscall register correction `fe5781eb`](https://github.com/zephyrproject-rtos/zephyr/commit/fe5781eb97475847214a24cc2a20b0f5ad53dabf).
The dispatcher changes `r6`; the inline invocation must declare it read-write.
Without the correction an optimized consecutive call can enter the wrong handler.
The unchanged upstream patch is backported in local commit `305ebb0fa`,
pending publication and integration into the dependency pin. Brickphyr reproduces the
failure and checks the correction on ARM QEMU with GCC. Regenerate the EDK and
rebuild applications after applying it; the SDK identity includes this header.

ARM MPU switching additionally requires local Zephyr commit `019ac4de` on
`topic/arm-mpu-irq`, pending publication and manifest integration. PendSV can
enter the dynamic MPU update with interrupts enabled. An interrupt between the
region base and attribute writes can see the new base with the old read-only
size, faulting on unrelated kernel data. Mask interrupts while programming the
dynamic regions. The short `tests/arch/arm/mpu_irq` QEMU test injects a pending
interrupt at that boundary during a real context switch and checks deferred
delivery; it fails before the correction and passes after it. This fault was
also captured on the Inventor F4 during native application execution.

Local Zephyr commit `85e1a648` on `topic/object-permission-cleanup` additionally
batches permission locking when clearing a departed thread's access to static
objects. The scheduler already masks interrupts during thread termination;
reacquiring the same object lock for every entry unnecessarily extends that
interval. Dynamic objects retain their existing reclamation and lock ordering.
The upstream userspace suite passes on ARM QEMU with dynamic objects both disabled
and enabled, including a check that a surviving thread keeps its grant and a
reused thread cannot regain the departed owner's access. Publication and manifest
integration remain pending.

Local Zephyr commit `3fa06829` on `topic/user-stack-placement` places the
MPU-aligned user-stack group before ordinary `.noinit` objects. This avoids the
interior alignment gap caused by the F4 Python stack while preserving stack
sizes, alignment and kernel-object bounds. Both linker definitions carry the
same order. ARM checks cover stack metadata and access, and Brickphyr's protected
native-Python execution, fault recovery and forced stop. Publication and manifest
integration remain pending.

Local Zephyr commit `1cc7b549` on `topic/kernel-worker-stacks` uses kernel-only
stacks for the MCUmgr, Bluetooth long/TX and STM32 UDC workers. These services
never enter user mode, so they need neither user-stack alignment nor a separate
privileged syscall stack. Configured stack budgets and guards are retained.
F4 compilation, ARM application/transport checks and the standalone MCUmgr
request-lifetime fixture cover the change; physical stack high-water measurements
remain pending. Publication and manifest integration remain pending.

## IMU processing port

Brickphyr owns the adapted calibration/orientation code in
[`src/imu`](../src/imu/README.md). Its acquisition, timing and Settings boundary
does not require a new Pybricks API. The source revision, license and differences
are recorded beside the port. Register and geometry helpers still come from the
dependency; the original PBIO IMU driver and estimator are not
linked alongside the port.

The numerical checks require the finite-input correction in PBIO's geometry
normalizer. It is part of local Pybricks commit `1220889f0` on `topic/imu-fixes`,
pending publication and manifest integration. That commit also fixes Pybricks'
own IMU sample mutation, delayed orientation update and partial settings changes;
its regression checks exercise the original PBIO driver callbacks. These changes
are independent of Brickphyr and introduce no extraction or platform adapter.

## Motor trajectory prerequisite

Motor control requires local Pybricks commit `986a1a2d2` on
`topic/motor-trajectories`, pending publication and manifest integration. PBIO
must reject durations before narrowing them and avoid dividing by a target speed
that rounds to zero. Control replacements build a candidate trajectory before
committing it, so a failed calculation preserves the ongoing maneuver.

Focused upstream trajectory checks and Brickphyr's numerical ARM checks cover
these boundaries. The complete upstream PBIO test application has not been built
with this patch; its Bluetooth submodule is not present in the local checkout.

Drivebase computation also requires local Pybricks commit `3d9e6dca9` on that
branch. `pbio_angle_from_low_res` now normalizes a 64-bit scaled angle instead of
overflowing intermediate 32-bit products. The existing scale limits are retained.
An upstream angle regression and Brickphyr's ARM numerical checks cover the
conversion; publication and manifest integration remain pending.

## USB charger detection prerequisite

The F4 BC1.2 charger-source path requires local, unpublished Zephyr commit
`cf010629` on `topic/stm32-bc12`. A standard BC1.2 child API shares the STM32 UDC's registers
and worker, runs the ported Pybricks detection deadlines before USB attach, and
waits for an in-flight result callback on cancellation. Failed USB attachment
rolls back the PHY before another detection. Brickphyr's delayed automatic CDC
enable uses this API. The detector's native-simulator timing fixture and F4 compilation
check the local integration; physical USB/source behavior remains unverified.
Publish and pin the dependency change before a reproducible release.

## USB acquisition scheduling prerequisite

F4 builds with high-rate acquisition require local Zephyr commit `b4ffd4217`
on `topic/cdc-workqueue-priority`, pending publication and manifest integration.
It makes the existing dedicated CDC ACM workqueue priority configurable; the
default retains the upstream system-workqueue priority. Brickphyr enables that
queue at a preemptible priority below IMU and motor service. Bluetooth retains
its required cooperative system queue. F4 compilation and physical USB telemetry
checks cover this integration.

## CMSDK UART test prerequisite

The ARM QEMU fixture for the shared host dispatcher additionally requires the
local CMSDK APB UART correction. `uart_irq_tx_complete()` must report
the transmitter's idle state independently of whether TX interrupts are enabled.
Otherwise a caller that disables an idle TX interrupt waits indefinitely for a
completion that already happened. The F4 uses a different UART implementation.

Zephyr's local `tests/drivers/uart/cmsdk_apb` regression checks cold idle completion
before and after disabling TX interrupts on `mps2/an385`. That case and
Brickphyr's shared-dispatcher REPL tests pass on ARM QEMU. Publication and manifest
integration remain pending.

## Speaker DMA prerequisite

The speaker profile requires local Zephyr commit `c26d83f5` on `topic/dma-stop`,
pending publication and manifest integration. STM32 DMA v1 must return pending
disable to the common driver's timeout instead of spinning indefinitely. Stop
must propagate timeout, retain channel ownership on failure, and permit retry;
an interrupt caller receives busy rather than sleeping. The existing five-second
thread deadline now uses elapsed time, independent of scheduler tick rounding.

This is a general STM32 DMA correction, not a Brickphyr API. F4 images compile
with it; source inspection establishes the changed error paths. A stalled
physical DMA controller has not been fault-injected. Brickphyr's synthetic DMA
checks separately establish that its service retains ownership after failed stop
and releases it on successful retry. The amplifier and sample trigger are disabled
before waiting for DMA cleanup.

## ADC timeout prerequisite

F4 battery and button acquisition enables the per-conversion timeout from local
Zephyr commit `aae8fbf7` on `topic/adc-timeout`, following the speaker DMA change.
Publication and manifest integration remain pending. This corrects a driver
lifetime problem: a missing completion IRQ otherwise leaves `adc_read()` waiting
indefinitely. Returning from an application-side wait would leave its buffer
reachable by a late ADC interrupt.

`CONFIG_ADC_STM32_CONVERSION_TIMEOUT_MS` currently applies to F4 regular
interrupt-driven conversions, excluding DMA, injected channels and streaming.
The default zero preserves other builds. A nonzero value bounds each sampling,
including asynchronous reads, without limiting intentional sequence intervals.
Timeout or overrun stops the ADC, disables its completion interrupt and cancels
further sampling before completing the read with an error. Late IRQs cannot write
to the released buffer; a later read can retry. The stop operation follows
[ST RM0430, ADC on/off control](https://www.st.com/resource/en/reference_manual/rm0430-stm32f413423-advanced-armbased-32bit-mcus-stmicroelectronics.pdf).

The F4 firmware and Zephyr's `tests/drivers/adc/stm32_timeout` image for
`nucleo_f401re` compile. That physical regression checks cold reads, masked-IRQ
timeout, late IRQs, retry, asynchronous completion and deliberate sequence delays;
it has not been run on a board. Brickphyr's emulated ADC tests separately check
how acquisition/observations handle errors; they do not prove this hardware path.

## Contribution certification

Follow the target project's current DCO and attribution rules. An agent must not
add `Signed-off-by` on behalf of a human. For Zephyr, the human submitter reviews
and signs; disclose AI assistance using its documented `Assisted-by` convention.
Do not invent an identity, model version, approval, or license grant.

References:

- [West branches and workspace behavior](https://docs.zephyrproject.org/latest/develop/west/workspaces.html)
- [Manifest imports](https://docs.zephyrproject.org/latest/develop/west/manifest.html)
- [Zephyr contribution guidelines](https://docs.zephyrproject.org/latest/contribute/guidelines.html)
- [Pybricks contribution guidelines](https://github.com/pybricks/pybricks-micropython/blob/master/CONTRIBUTING.md)
