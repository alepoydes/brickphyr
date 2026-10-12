# Contributing

Brickphyr should remain understandable to someone who knows embedded systems
but has not followed its development. Prefer a small, coherent implementation
with clear ownership and measured costs over layers that merely rename calls.

The [README](README.md) describes current availability. The experimental board
image and simulator can be built using [the workspace procedure](docs/builds.md).

## Making a change

1. Read the relevant contract in [architecture](docs/architecture.md) or its linked
   subsystem document. State the behavior being changed and why.
2. Find the code's proper home using [subsystems](docs/subsystems.md). An improvement
   useful to a dependency's own users belongs there, with integration separate.
   A Brickphyr-specific adaptation belongs here, including a licensed source port
   when an existing implementation cannot be reused unchanged.
3. Implement the smallest complete solution, preserving existing contracts unless
   their change has been agreed. Remove obsolete alternatives in the same scope.
4. Check meaningful invariants and failure boundaries. Use the production path;
   mocks must not replace the property being tested.
5. Review the final diff as one design. Update the owning documentation and explain
   what was checked locally, on hardware, or not yet checked.

For changes crossing repositories, follow [the upstream workflow](docs/upstream.md).
Do not publish an unpushed local dependency commit in a shared manifest.

## Implementation conventions

- Model the domain first. States, ownership, units, coordinates, lifetimes, and
  error meanings must agree across drivers, runtimes, tools, and storage.
- Prefer existing public APIs. If an upstream boundary is inadequate, improve it
  in that project instead of reaching through its private representation.
- Use functions, types, arrays, and controlled mutation where they clarify the
  actual operation. A helper does not need a public class or a new module.
- Keep computation separate from device access. Shared math and protocol logic
  should be testable without a board; board-specific behavior still needs a board.
- Define each setting or format once. Generate derived constants and SDK material
  from it. Use Kconfig for options and devicetree for hardware, not parallel tables.
- Use the destination project's language and style in upstream changes. In new
  firmware code, C and Zephyr conventions are the default. Application languages
  are independent of the firmware implementation language.
- Design hot paths around caller-owned storage and bounded work. Make copying,
  allocation, blocking, and precision choices visible. Optimize cold code only
  when its cost matters.
- Public invalid input requires an error, not an assertion compiled out of the
  release. Assertions are appropriate for internal invariants.
- An unsupported feature returns a documented unsupported result before changing
  hardware. Never simulate success with zeros or an empty callback.
- Validate user memory ranges, sizes and required alignment before entering a
  privileged critical section. Application faults must not strand service locks
  or leave a driver accessing an application's buffer after release.
- Retry only when retrying can help. Record the cause and keep recovery bounded.
  A cleanup failure must not replace the original fault.
- Comments explain reasons and non-obvious contracts. Do not narrate obvious code
  or preserve abandoned implementations in commented blocks.

Correct the cause of a defect, then add a focused regression check when it protects
meaningful behavior. Update a working rule only when the defect exposes a reusable
gap; do not accumulate a new rule for every incidental bug.

## Documentation conventions

Public documentation and code comments are in English. Names are `Brickphyr` in
prose and `brickphyr` in repository, package, and command identifiers.

Each large question has one owning document. Link to it instead of reproducing
the decision elsewhere. The short architecture document describes relationships;
it is not a second copy of every subsystem specification. `AI/context.md` is a
short public orientation guide.

Everything tracked by Git is public. Keep technical reports, development handoffs,
device/session observations, workstation paths, and experiment logs in `reports/`,
which must remain ignored and untracked. Public documentation contains design,
usage, supported configurations, and limitations; it must be readable without
the development history. Do not link public instructions to private reports.

Distinguish these claims explicitly:

| Claim | Required meaning |
| --- | --- |
| Selected design | An agreed direction; no implementation or test implied |
| Experimental | An implementation with named unresolved assumptions; explicit opt-in |
| Implemented | Code exists and has the listed checks; physical behavior may remain untested |
| Hardware-verified | Named revision, build, procedure, and observed result |
| Unsupported | No working implementation is provided |

Write self-contained explanations, not replies to past conversations. Keep actual
commands runnable, identify illustrative fragments, and record units and test
conditions beside measurements. Unknown values stay unknown. Avoid adjectives
such as "safe", "fast", or "compatible" without their scope and evidence.

Configuration and generated reports own factual build data. Documentation owns
rationale, limits, and interpretation. Preserve links to exact upstream revisions
when an implementation detail matters; "latest" documentation is guidance, not a
dependency pin.

## Checks and pull requests

Use [testing](docs/testing.md) to select checks proportional to the change. Long
soak tests and hardware tests are explicit jobs. Passing a host simulation is not
evidence of interrupt latency, USB behavior, or physical motor stopping.

For each test, identify the defect it can catch and use the cheapest production
boundary that exposes it. Check numerical behavior directly; use runtime/transport
tests for integration-specific behavior. Do not repeat lower-level coverage in
every binding, exhaust independent parameter combinations, or test trivial checks
already clear from inspection. Preserve focused regressions for actual defects
and consequential boundaries such as data loss, isolation and resource cleanup.

During debugging, run the failing case. After the fix, run the affected set once;
do not rerun unchanged successes. Broaden for a changed shared contract or an
unresolved failure. Reserve the full integration collection for acceptance of a
large milestone, not each small change or commit. Aim for an affected set that
executes in 5–10 seconds and keep total local automated test execution for a
usual feature sprint within 30 seconds, including retries, fixture startup and
cleanup. A real defect may justify a larger check; explain the overrun. Never
skip a meaningful failure or terminate a check and report success to meet the
target. Select physical checks separately and make their elapsed time visible.

Build the required fixtures separately and incrementally, with at most four
compilation jobs in total; time compilation separately from test execution.
Tests must not silently rebuild firmware. Reuse a simulator for stateless library
cases while starting a fresh application; cold-start and persistence checks
require fresh firmware. Prefer event completion or virtual time to sleeps. Real
waits and repetitions must exercise a named timing/lifetime property, not
compensate for flakiness. Measure test costs when choosing or revising coverage;
do not optimize test count.

For documentation changes, review local links, terminology, cross-document
consistency, and whitespace. For firmware changes, run the affected build and
short behavior checks. Tests must exercise executable behavior or produced
artifacts, not inspect source text or assert that a scaffold exists.

A pull request explains the problem, resulting behavior, validation, and remaining
limitations. Include dependency PRs or commit links for cross-repository work.
Keep unrelated formatting or rewrites out of a functional change.

New files follow [the license policy](docs/licensing.md); existing files retain
their notices. Human contributors are responsible for rights to submitted work.
Do not fabricate authorship, approvals, or upstream DCO certification.
