# Workspace and builds

There are no executable Brickphyr builds yet. This document specifies how the
first build is to be organized; the names below describe intended profiles, not
downloadable firmware. Toolchain and dependency versions remain to be selected
together in the first reproducible integration.

## Repository layout

Use a west T2 workspace: Brickphyr is the manifest/application repository and
dependencies are independent Git checkouts. The workspace directory itself is
not a repository. A representative layout is:

```text
brickphyr-workspace/
  .west/
  brickphyr/             # this repository; will own west.yml
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

Add `west.yml` with the first build, using real immutable commits whose combination
has been checked. Import the selected Zephyr manifest and only the required
modules instead of copying its dependency versions. Pin any explicit overrides.
Do not put invented SHAs or floating `main` / `master` refs in a runnable manifest.

Pybricks already depends on MicroPython and other libraries. Resolve one intended
MicroPython source and its necessary submodules; do not accidentally build both
an imported upstream MicroPython and Pybricks' different fork. WAMR must likewise
have one owner in the resolved manifest. The reference firmware in
[hardware](hardware.md) is evidence, not automatic approval of all its dependencies.

Use upstream Zephyr CMake, Kconfig, devicetree, SDK, and west interfaces. Introduce
Python tooling with `pyproject.toml` and a committed `uv.lock`; use `uv run --locked`
for reproducible project commands. Firmware, SDK, and Python package versions are
different dependencies and must all be recorded where applicable.

At a release, retain the resolved manifest, toolchain identity, build configuration,
partition map, resource report, and source licenses. Keep generated output in
`build/` or release artifacts, not in source control. Dependency development and
integration rules are in [upstream](upstream.md).

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

## Acceptance of the first build

The same checkout and locked dependencies must reproduce a board image and its
SDK. The image must respect [recovery](recovery.md); the SDK must load a separate
test application through the production loader. Add the actual commands and
examples here only after they run. A no-op firmware scaffold is not that milestone.
