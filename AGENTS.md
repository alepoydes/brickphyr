# Working instructions

Start with [AI/context.md](AI/context.md), [the architecture](docs/architecture.md),
and the relevant subsystem document. [CONTRIBUTING.md](CONTRIBUTING.md) defines
engineering conventions for humans and agents alike. These files are sufficient
to resume work without access to a previous conversation or a personal rules file.

## Working method

- Follow the user's current scope. Discussion does not authorize implementation;
  an implementation request does authorize the necessary local, reversible work.
  Continue settled work without repeatedly asking for the same permission.
- Describe the required behavior and its invariants before choosing machinery.
  Change the smallest coherent part that solves the actual problem. Fewer lines
  are useful only when they also reduce concepts, duplication, or execution cost.
- Preserve agreed contracts. Discuss substantive changes before implementing
  them. Routine implementation choices within those contracts need no new vote.
- Reuse maintained code through documented interfaces. Do not copy dependencies,
  patch private host APIs, invent hardware facts, or fabricate measurements.
- Keep helper functions local and the public interface small. Do not introduce
  generic managers, services, plugin systems, or parallel implementations without
  a concrete need. Remove superseded paths instead of layering workarounds.
- Resolve uncertainty with source inspection or a bounded experiment. Unimplemented
  operations must report that fact before side effects; an experimental solution
  must state its hypothesis, limitations, and the evidence needed to accept it.
- Do not add fallback behavior that silently changes semantics, hides corrupted
  state, or blocks useful work without a justified invariant.

## Evidence and validation

- Distinguish source inspection, compilation, simulation, and measurements on a
  named physical board. None implies the others. Mark synthetic workloads clearly.
- Test meaningful behavior, boundaries, and regressions through the production
  path. Do not test placeholders, file layout, or an implementation's own formula
  against a copied formula. Keep long hardware/soak tests separate from fast checks.
- For performance changes, account for the entire affected path, including
  allocation, logging, interrupts, queues, driver waits, and measurement overhead.
  Derive budgets; do not choose periods, priorities, or buffers to hide failures.
- Preserve the first error and its operation context through cleanup. Repeated
  errors need counters and a representative example, not unbounded output.
- Do not mark unsupported hardware or unfinished APIs as working to make a matrix
  or a test green. Update the owning document when evidence changes its status.

## Repositories and tools

- Follow [the upstream workflow](docs/upstream.md). Dependencies are independent
  Git repositories in a west workspace, not source copies inside Brickphyr.
- Inspect existing work before changing it. Never overwrite unrelated edits or
  rewrite published dependency commits used by a manifest or release.
- Use uv for local Python tooling and commit its resolved `uv.lock` when Python
  tooling is introduced. Use west/CMake/Kconfig/devicetree for Zephyr instead of
  a competing build or configuration framework.
- Keep generated code, builds, captures, raw logs, and local devices/credentials
  out of Git. Small curated regression fixtures need provenance and a purpose.
- Agents must not invent human authorship or add human DCO sign-offs. Follow the
  destination project's current AI attribution rules for upstream submissions.

## Hardware and handoff

- Follow [the recovery constraints](docs/recovery.md) before changing firmware.
  Native applications must not gain raw flash-controller access as an API shortcut.
- Connection to a hub is not permission to flash it or energize motors. Use the
  current explicit authorization; do not import permission from an old experiment.
  Once authorized, do not repeatedly ask for unchanged work in that scope.
- Confirm the previous application has stopped before replacement. Normal host
  disconnection must not stop an autonomous application. Diagnostics have their
  own explicit stop/cleanup contract and one host connection owner.
- At handoff, report hardware state and any live diagnostic process. Close links
  unless the operator requested an ongoing session; never claim a dead process is
  keeping a hub awake.
- Everything tracked by Git is public. Keep technical reports, session handoffs,
  device observations, and local work notes in ignored `reports/`; never force-add
  them. Keep [AI/context.md](AI/context.md) a short public orientation guide.
  Public documents own lasting contracts, usage, support status, and limitations,
  not the history of development sessions.
