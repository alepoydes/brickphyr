# Development context

Last established: 2026-10-10. This file is a handoff for the next development
session. Public contracts live in the linked documents; update this file instead
of appending a transcript of each conversation.

## Start here

1. Read [AGENTS.md](../AGENTS.md) and [architecture](../docs/architecture.md).
2. Read [subsystems](../docs/subsystems.md) and the document owning the next task.
3. Check actual Git/workspace state and any new operator instructions before work.

The accepted name is **Brickphyr**, with the description: "An open application
platform for LEGO hubs, built on Zephyr RTOS." The earlier working name ZeBricks
is retired and must not be used for packages or documentation.

## Current state

- This is a separate repository foundation with design and contribution documents.
  It has no firmware source, west manifest, pinned Zephyr/toolchain combination,
  buildable board target, SDK, host tool, or CI firmware result yet.
- No remote repository or fork has been configured by this work. Do not invent
  organization URLs when adding a manifest or instructions.
- No Brickphyr hardware operation has been performed. No diagnostic connection
  is being maintained by this project. Hardware availability must be rechecked.
- Current authorization covers this repository foundation. Old babybot motor-on
  permissions do not authorize Brickphyr flashing or motion experiments.
- The local upstream source inspected in babybot is a modified virtual-hub
  checkout. Use its recorded revision as a reference, not its working directory
  as a clean Brickphyr dependency. See [hardware](../docs/hardware.md).

On the original workstation, the optional sibling `../babybot/` contains
`scripts/virtualhub.py` (firmware pin), `scripts/README.md` (working desktop
transport/session notes), and `build/virtualhub-src/` (upstream checkout with local
virtual-hub patches). These are research references, not Brickphyr build inputs.
Brickphyr's public documentation must remain usable without that checkout.

## Settled directions

These are selected designs, not open questions to reopen without new evidence:

| Decision | Owning document |
| --- | --- |
| Zephyr foundation, one user application, independent hardware services | [Architecture](../docs/architecture.md) |
| User mode / MPU on the initial capable target; measure costs during implementation | [Applications](../docs/applications.md) |
| Native, Python/Pybricks, WASM through shared hardware semantics | [Applications](../docs/applications.md) |
| Builds select available engines; artifact format selects execution | [Builds](../docs/builds.md) |
| Preserve Python behavior; experimental native ABI with matching SDK | [Compatibility](../docs/compatibility.md) |
| Zephyr general mechanisms, Pybricks LEGO-specific algorithms and bindings | [Subsystems](../docs/subsystems.md) |
| Zephyr Bluetooth host, with radio init and blob licensing to resolve | [Bluetooth](../docs/bluetooth.md) |
| Separate Git dependencies; working upstream contribution path | [Upstream](../docs/upstream.md) |
| Preserve boot chain and original-firmware restoration | [Recovery](../docs/recovery.md) |
| Apache-2.0 own code; retain upstream licenses and MIT contributions to Pybricks | [Licensing](../docs/licensing.md) |

Babybot remains a separate consumer, not part of the firmware or a source of
platform-wide control policy. Its prior experience motivates native hardware
access, predictable execution, and usable program capacity. Its fixed sampling
periods, MicroPython workarounds, button assignments, and model admission rules
must not become Brickphyr requirements. Host simulation and native C math from
that project are not performance evidence for a new Zephyr port.

## Next work, in dependency order

1. Establish a clean west workspace and compatible immutable dependency/toolchain
   pins. Evaluate out-of-tree board integration; add an actual build, not empty
   CMake/Kconfig placeholders. Keep the original repositories independently useful.
2. Identify the first physical hub revision and audit its existing boot/flash path.
   Prepare the correct image layout and recovery procedure before requesting an
   authorized firmware write. Source study and local build work can proceed first.
3. Bring up the minimum board services, then one isolated native application with
   sensor access and explicit motor ownership. Gate outputs for diagnostic tests.
   Verify stop/unload/fault behavior before declaring the native endpoint usable.
4. Integrate LEGO device/control behavior and the chosen Bluetooth host; measure
   complete workloads and memory. Hardware access expands with small executable
   tests, not broad placeholder APIs.
5. Integrate Pybricks-compatible Python and WAMR through the same service boundary.
   These may be developed alongside native work once the necessary interfaces
   exist. Publish SDK/build profiles and compatibility coverage from actual results.

This ordering does not authorize narrowing the platform to a native-only fork or
postponing compatibility until after an incompatible hardware API is entrenched.
The first complete vertical slice establishes contracts reused by the other engines.

## Foundation checks

All Markdown documents were parsed and their relative links resolved locally.
External documentation links were reachable. A temporary Git fixture exercised
topic development, an integration pin, upstream squash acceptance, and moving to
the equivalent upstream commit while retaining the old pin. This validates the
Git mechanics only; it is not a Zephyr build or a real upstream submission.
No hardware was accessed and the babybot worktree was unchanged.

## Decisions requiring implementation evidence

- Exact hardware revision, boot entry, preserved data, and writer/image bounds.
- Zephyr/Pybricks/MicroPython/WAMR/toolchain versions that integrate together.
- Export surface, ELF relocation/MPU layout, syscall and cleanup costs.
- Radio transport/init and rights to distribute the required controller binary.
- Flash/application storage format and recovery-aware partitioning.
- Service periods/priorities, queues, stacks, and runtime heap sizing.
- WASM mode and which combined builds fit; runtime input/API coverage.

Resolve these through source inspection and bounded experiments. If a branch is
not understood, leave the feature explicitly unsupported; if an experiment is
promising but incomplete, document and label it. Do not invent constants or a
successful fallback to finish an apparent checklist.

## Handoff discipline

After meaningful work, replace the current-state and next-work entries with actual
results, paths, and remaining questions. Keep enduring decisions in their owning
documents. Record local versus physical checks and any retained process/hardware
state. The next session should be able to act without rediscovering the design
or trusting stale connection and authorization claims.
