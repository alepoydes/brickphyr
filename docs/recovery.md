# Firmware installation and recovery

**No hardware-verified Brickphyr flashing procedure is available yet.** The F4
image builds and its layout is checked locally. This document contains
source-derived bounds and the remaining installation work, not commands to
execute on an unidentified hub.

## Required property

An invalid user application, failed application transfer, or broken Brickphyr
Bluetooth implementation must not remove the independent way to reinstall
firmware. Preserve the factory LEGO bootloader and its independent recovery entry.
Use the standard Pybricks installer, including installation or replacement of the
second-stage mboot when that installer requires it. Brickphyr adds no separate
bootloader implementation; hardware detection and installation paths belong in
the maintained Pybricks tooling.

Installation must accept a supported hub running either original LEGO firmware
or Pybricks, without requiring an intermediate Pybricks installation. Updating
Brickphyr and restoring LEGO or Pybricks must use the same maintained installation
mechanisms. Entering the existing bootloader with the hub's buttons is a valid
installation path; automatic entry depends on the installed firmware and must
not be assumed for every version.

Reuse Pybricks host tools and their firmware package format. Brickphyr owns its
Zephyr build and the integration needed to package its image. Hub identification,
USB/Bluetooth flashing protocols, checksums, and restoration mechanisms belong in
the existing Pybricks tooling. Extend that tooling when an actual missing capability
prevents reuse, following [the upstream workflow](upstream.md); do not create a
second implementation or a wrapper that merely renames the same commands.

Bad main firmware and an interrupted update, including a second-stage update,
must remain recoverable through factory DFU without a functioning application.
Neither installation nor application execution may erase the factory loader or
change its protection. Hardware faults are outside this firmware recovery contract.
Physical recovery checks are required before claiming the contract is verified.

## Initial Prime F4 source map

The [reference Pybricks linker script](https://github.com/pybricks/pybricks-micropython/blob/8aa9986cd0666fb610225de05440fb6d1f99c4ab/lib/pbio/platform/prime_hub_f4/platform.ld)
describes:

| Internal region | Start | Size | Treatment |
| --- | --- | --- | --- |
| Factory DFU loader | `0x08000000` | 32 KiB | Preserve; upstream calls it non-erasable, verify the actual protection |
| Second-stage DFU loader | `0x08008000` | 32 KiB | May be installed or replaced by the standard Pybricks installer |
| Main firmware | `0x08010000` | 960 KiB | Candidate Brickphyr executable region for this layout only |

This map is not universal across hub families or revisions. Product identification,
boot entry, image metadata/checksums, and the actual flash geometry all matter.
In particular, a flashing tool's transfer start may include a second-stage loader
even when the main application linker address does not. Audit both the packager
and writer; changing only a linker address is insufficient.

The reference platform's external-storage configuration preserves regions used
by LEGO and other boot/runtime components. Map those regions and any unique
identity/calibration data before expanding application storage. "Use all flash"
means use the available space after these constraints, not erase the chip.
Restoring LEGO firmware and restoring every original user project are separate
claims; document any required backup and data loss explicitly.

## Current image and installation boundary

The `spike_prime_f4` devicetree limits the image to the main-firmware partition.
Its linker fragment places the mboot information records at offsets `0x1f0` and
`0x264` from the image start. [The image checker](../tools/check_image.py) checks
the resulting ELF and binary against the external F4 memory/boot contract. The
build enables no application flash-write API and packages neither bootloader.
Devicetree `read-only` labels describe ownership; they do not establish physical
flash write protection.

The source reference's
[DFU writer](https://github.com/pybricks/pybricks-micropython/blob/8aa9986cd0666fb610225de05440fb6d1f99c4ab/tools/flash/dfu.py)
distinguishes F4/H5 and frozen/second-stage DFU using device properties and flash
geometry. For F4 already running mboot it writes at `0x08010000`, with the initial
stack word committed last. Its frozen-F4 path can instead install a second-stage
loader starting at `0x08008000`. Both are supported installer paths; neither
erases the factory loader. Device detection and path selection remain owned by
Pybricks. Short tests exercise both paths with a simulated DFU device, checking
factory-region exclusion and first-word-last ordering without changing the writer.

Package integration and focused host-tool checks can be developed without a hub.
Before an actual firmware write, the installer must identify the connected hub
and loader and validate the selected image. Establish the independent button
entry, protection/preserved-data constraints, and restoration artifact for the
physical experiment. Reuse a maintained writer through a documented interface
that supports the required bounds and interrupted-transfer ordering. Do not use
a generic mass erase or treat an ordinary binary download as evidence of that
ordering. The current repository has no flash runner or writer.

The reference's [CLI](https://github.com/pybricks/pybricks-micropython/blob/8aa9986cd0666fb610225de05440fb6d1f99c4ab/tools/flash/__main__.py)
already handles firmware packages and includes paths for entering update mode
from Pybricks and supported official SPIKE Prime firmware. Pybricks Code also
documents [custom firmware ZIP installation](https://pybricks.com/learn/getting-started/install-pybricks/).
These are integration starting points, not evidence of a Brickphyr installation.
The reference's
[metadata generator](https://github.com/pybricks/pybricks-micropython/blob/8aa9986cd0666fb610225de05440fb6d1f99c4ab/tools/metadata.py)
normally expects Pybricks linker-map sections. The experimental extension accepts
images without a customizable name, allowing [Zephyr packaging](builds.md#experimental-firmware-package)
without fabricated sections or duplicate hardware/format tables. The resulting
F4 package round-trips through the Pybricks reader with its binary unchanged.

The first physical cycle is Pybricks → Brickphyr → Pybricks, checking a working
console and the restored firmware's ordinary connection. Also verify cold boot,
`reboot`, and USB disconnection/reconnection while running from the battery;
uptime must continue across host loss. Motor outputs remain inactive. Test the
remaining LEGO transitions and interrupted-transfer recovery separately after
ordinary recovery is established. No such cycle has been performed yet.

## Bring-up sequence

1. Identify the physical board, installed firmware, USB interfaces, and independent
   button/bootloader entry. Compare against the corresponding upstream sources.
2. Record its partition/protection map and obtain the required recovery artifacts
   through legitimate existing tools. Preserve unique data where applicable.
   Do not assume a backup of main flash includes external flash or factory data.
3. Establish the existing loader connection before testing new firmware. Select
   and review the exact image type, bounds, metadata, and writer behavior.
4. On explicitly authorized hardware, install the minimal board image while
   preserving the factory loader. Verify recovery when the main application provides
   no Bluetooth service. Test the retained USB DFU path on the first capable hub.
5. Verify the paths from LEGO and Pybricks to Brickphyr and back to each, with
   named hardware/builds. Test transfer interruption only after the recovery
   mechanism is established and the hardware experiment is authorized.

Reuse evidence from Pybricks for hardware not locally available, clearly labeled
as source/upstream evidence. Physical verification can grow through community
reports; do not claim all-board recovery from a single successful restoration.

## Constraints on implementation

- Application-facing APIs cannot write raw bootloader regions or protection
  options. Firmware updates are a separate privileged operation.
- Image validation must reject wrong target, bad length/range, and incompatible
  format before erasing. Preserve existing bootloader integrity requirements.
- Never erase or overwrite the factory LEGO loader or change its protection.
  Second-stage installation and replacement use the standard Pybricks workflow.
- An image must not be selected solely from a marketing name or USB product
  string. Match the actual descriptors and supported hardware identification.
- Before replacing a running application or firmware, confirm STOP. A command
  acknowledgment alone may precede actual termination and device quiescence.
- Motor outputs remain inactive during bring-up until an explicit motor test is
  authorized. Transport tests must not accidentally run a previously stored app.

## What a published procedure must contain

Specify tested board revisions, required initial firmware/loader state, package
and tool versions, commands, expected device states, protected data, and an
independent restore procedure. Include the result of a failed/interrupted transfer
test for the supported fault model. A procedure must not depend on a functioning
Brickphyr console to repair a broken Brickphyr image.

Upstream starting points are [Pybricks installation/restoration](https://pybricks.com/learn/getting-started/install-pybricks/),
the platform reset/linker sources, and [pybricksdev](https://github.com/pybricks/pybricksdev).
Their mechanisms are reused, but their existing success is not evidence that a
new Brickphyr image has the correct layout.
