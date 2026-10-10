# Working with upstream repositories

This workflow is part of the project contract. Reuse is incomplete if a local
change cannot be reproduced, reviewed, and returned to its source project.

## Repository roles

Brickphyr owns its application lifecycle, integration, build configuration, SDK,
and board support that is not yet upstream. Dependencies remain ordinary Git
repositories managed by west; see [workspace layout](builds.md).

Start with upstream Zephyr and out-of-tree board/driver support. Create a Zephyr
fork only when a required change cannot live outside its tree. Maintain a thin
Pybricks fork when portable changes are needed there. A fork keeps the upstream
history and source layout; it is not a copied subtree with a new root commit.
Do not create an independent PBIO repository before the integration demonstrates
that extracting it would help both consumers and the upstream maintainers agree.

In a working fork, `origin` is our fork and `upstream` is the original project.
For an unmodified dependency, its normal upstream remote is sufficient. The
Pybricks fork is [alepoydes/pybricks-micropython](https://github.com/alepoydes/pybricks-micropython),
with [pybricks/pybricks-micropython](https://github.com/pybricks/pybricks-micropython)
as upstream. The USB experiment also needs a published Zephyr fork for the
[CDC completion change](#usb-experiment-prerequisites).

## Put the change in its owning project

| Change | Expected home |
| --- | --- |
| Generic Zephyr driver, kernel, or LLEXT improvement | Zephyr, with its required tests |
| Portable LEGO protocol, calibration, or control fix | Pybricks, independent of Zephyr integration |
| Reusable firmware packaging, hub identification, installation, or restoration extension | The existing Pybricks host-tool repository owning that operation |
| Missing reusable MicroPython/WAMR behavior | That runtime's project |
| Brickphyr lifecycle, SDK, glue, application storage policy | Brickphyr |

A portable change and its OS-specific integration should be separate commits or
PRs. Explain the upstream use case without requiring the reviewer to adopt
Brickphyr. Follow that project's license, style, and contribution requirements.

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

The [manifest](../west.yml) pins upstream Zephyr, CMSIS 6, STM32 HAL, and our Pybricks fork.
The board lives out of tree in Brickphyr. The published pins still select
unmodified Zephyr and its imported modules; the USB experiment has additional
[prerequisites](#usb-experiment-prerequisites). Pybricks supplies host tools and portable resistor-ladder/TLC5955 helpers.
The firmware adapters use Zephyr ADC, Input, SPI, PWM, GPIO, and LED APIs.

The pinned Pybricks fork includes these changes on the upstream v4.1.0b6 base:

| Owning file | Change and reason |
| --- | --- |
| `tools/metadata.py` | `--no-custom-name` permits checksum-free images without Pybricks-specific linker sections |
| `lib/pbio/include/pbdrv/resistor_ladder.h`, `drv/resistor_ladder/` | Extract the existing decoder from ADC access for reuse with Zephyr |
| `lib/pbio/include/pbdrv/tlc5955.h`, `drv/pwm/pwm_tlc5955_stm32.c` | Share control-frame definitions and grayscale encoding without the STM32/PBIO scheduler adapter |
| `tests/tools/` | Short metadata/package checks and factory-loader preservation checks through the unchanged DFU writer |

These changes are published in
[the integration commit](https://github.com/alepoydes/pybricks-micropython/commit/2d62eb45e879075d63ace7728f8626437d3a683b).
`west update` obtains them through the manifest without manual patches. The
installer and its DFU implementation remain unmodified. No upstream PR has been
submitted yet; remove the downstream delta when equivalent upstream functionality
is adopted.

## USB experiment prerequisites

The experimental USB path additionally requires portable dependency
changes, not yet included in the published manifest pins:

- Pybricks COBS decoding must reject truncated blocks and reserved wire bytes.
- Zephyr's next-stack CDC ACM driver must implement `uart_irq_tx_complete()`,
  including pending USB packets and the terminating zero-length packet. A FIFO
  alone cannot establish that the previous protocol's output has drained.
- pybricksdev needs a Profile 1.6+ CDC transport, tagged replies, characteristic
  reads, and bounded exchange/cleanup. Its existing command and event handlers
  remain shared with BLE and the older USB transport. The Python COBS codec moves
  from the Pybricks flashing tool to `pybricksdev.usb.cobs`; the flashing tool
  imports that single implementation. Keep the coordinated client/tool changes
  together until they can be published and their dependency requirements finalized.

Keep these changes in their owning repositories, run their focused tests, and
publish reviewed dependency commits before updating the manifest. A clean
checkout of the current pins does not yet reproduce the complete USB experiment.
Do not substitute timing delays or inspect private driver buffers in Brickphyr.
The manifest includes the published pybricksdev base in a sibling checkout, and
uv installs that checkout as an editable dependency for the coordinated experiment.
The published base alone does not supply the new CDC client or shared codec.

## Settings storage prerequisites

The experimental persistent-settings build also requires a Zephyr file-backend
correction, currently outside the manifest pin: preserve read errors through
file close and reject short values whose stored length says more data exists.
The regression checks use real filesystem operations with truncated Settings
records. Keep this correction in Zephyr, with its tests, until reviewed dependency
commits can be published and pinned.

## SMP prerequisites

The experimental Shell/SMP path requires a Zephyr transport change outside the
published manifest pin. The common serial receiver exposes packet extraction and
session reset to custom Shell backends, so Brickphyr can reuse its framing while
joining accepted operations during disconnect. It also rejects oversized lines
and clears rejected header prefixes. The default UART transport remains available;
its input-expiration timer is specific to that backend and disabled for custom
receivers. Portable receiver checks cover both configurations.

The manifest imports zcbor through Zephyr's pinned module list. Host tooling uses
the locked, unmodified `smpclient` and `smpmgr` packages. Keep the receiver change
in Zephyr; publish reviewed dependency commits before updating shared pins.

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
