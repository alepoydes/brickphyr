# Firmware installation and recovery

**No Brickphyr flashing procedure is available yet.** This document is the design
constraint for board bring-up. It contains source-derived addresses, not commands
to execute on an unidentified hub.

## Required property

An invalid user application, failed application transfer, or broken Brickphyr
Bluetooth implementation must not remove the independent way to reinstall
firmware. Preserve the existing hub boot chain and reuse the Pybricks installation
and restoration mechanisms where compatible. Do not introduce a new bootloader
merely because Zephyr commonly uses one.

The supported fault model must include a bad main firmware and interruption of
main-firmware transfer. It does not promise recovery from every hardware failure,
corrupt privileged flash driver, changed protection option, or erased bootloader.
An unrestricted privileged image cannot be made harmless by documentation alone.

## Initial Prime F4 source map

The [reference Pybricks linker script](https://github.com/pybricks/pybricks-micropython/blob/8aa9986cd0666fb610225de05440fb6d1f99c4ab/lib/pbio/platform/prime_hub_f4/platform.ld)
describes:

| Internal region | Start | Size | Treatment |
| --- | --- | --- | --- |
| Factory DFU loader | `0x08000000` | 32 KiB | Preserve; upstream calls it non-erasable, verify the actual protection |
| Second-stage DFU loader | `0x08008000` | 32 KiB | Preserve its role in official firmware compatibility |
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

## Bring-up sequence

1. Identify the physical board, installed firmware, USB interfaces, and independent
   button/bootloader entry. Compare against the corresponding upstream sources.
2. Record its partition/protection map and obtain the required recovery artifacts
   through legitimate existing tools. Preserve unique data where applicable.
   Do not assume a backup of main flash includes external flash or factory data.
3. Establish the existing loader connection before testing new firmware. Select
   and review the exact image type, bounds, metadata, and writer behavior.
4. On explicitly authorized hardware, install the minimal board image while
   preserving the boot chain. Verify recovery when the main application provides
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
- Do not replace factory/second-stage loaders or alter readout/debug protection
  as an incidental step in a radio, storage, or runtime change.
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
