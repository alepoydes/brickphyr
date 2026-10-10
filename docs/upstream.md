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
For an unmodified dependency, its normal upstream remote is sufficient. Fork
hosting/organization names have not been chosen and no remote fork exists as part
of this foundation.

## Put the change in its owning project

| Change | Expected home |
| --- | --- |
| Generic Zephyr driver, kernel, or LLEXT improvement | Zephyr, with its required tests |
| Portable LEGO protocol, calibration, or control fix | Pybricks, independent of Zephyr integration |
| Missing reusable MicroPython/WAMR behavior | That runtime's project |
| Brickphyr lifecycle, SDK, glue, application storage policy | Brickphyr |

A portable change and its OS-specific integration should be separate commits or
PRs. Explain the upstream use case without requiring the reviewer to adopt
Brickphyr. Follow that project's license, style, and contribution requirements.

## Developing a dependency change

Before `west update`, inspect dependency worktrees and preserve local work on
named branches. West controls `manifest-rev` and normally leaves detached HEADs;
never develop on that branch or leave new work reachable only from detached HEAD.
Do not update a dirty dependency automatically to satisfy a build script.

For example, after configuring `origin` as a writable Pybricks fork and `upstream`
as the original project, these commands run from the workspace root:

```sh
git -C modules/pybricks fetch upstream
git -C modules/pybricks switch --create topic/port-wait upstream/master
```

The branch name is illustrative. Choose the actual target upstream base required
by the change; an older maintained release may be appropriate. Develop and commit
there, then inspect the portable delta:

```sh
git -C modules/pybricks diff upstream/master...HEAD
git -C modules/pybricks log --oneline upstream/master..HEAD
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

There are no downstream patches, fork URLs, or dependency pins yet. When the first
patch is needed, record its owning repository, reason, commit/PR link, and condition
for removal here. Keep this list about remaining divergence; Git and upstream PRs
hold the detailed change history.

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
