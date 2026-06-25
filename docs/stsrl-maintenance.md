# STSRL Fork Maintenance

This fork is used by STSRL as an external native simulator source. It exposes
simulator surfaces needed by STSRL, while Slay the Spire mechanics remain the
authority for behavior.

The detailed role contract lives in the STSRL repository:

- https://github.com/lsmfttb/STSRL/blob/main/docs/sts_lightspeed_maintainer_role.md

## Active Integration Branch

`stsrl/main` is the single active STSRL integration line in this fork.

Native STSRL work should be developed on temporary task branches, reviewed with
build and API evidence, and then integrated by advancing `stsrl/main` to the
accepted commit. STSRL must not depend on local unpushed state or on a moving
branch alone.

## Reproducibility Contract

STSRL accepts simulator source changes through exact commits recorded in its
source manifest, not through branch names by themselves. The authoritative
record is:

- `docs/sts_lightspeed_source_manifest.json` in the STSRL repository

Every STSRL-side manifest update must identify the external repository, the
resolved commit, and source-verifier evidence from a clean checkout of that
commit.

## Historical Branches

Historical task-shaped branches such as `stsrl/t006-*`, `stsrl/t008-*`,
`stsrl/t017-*`, and `stsrl/t018-*` are retained as provenance. They are not the
normal build input once `stsrl/main` is available.

Do not delete historical STSRL branches unless the STSRL main maintainer
explicitly approves that cleanup. Do not force-push over commits that have
already been referenced by a STSRL manifest, PR report, or verifier result.

## Native Change Boundary

Fork-side native changes should stay minimal and task-focused:

- expose required C++ or pybind simulator surfaces;
- report information-regime implications for search or projection APIs;
- avoid changing game mechanics for training convenience;
- keep STSRL Python adapters, manifests, and verifier policy in STSRL.

STSRL acceptance still requires a reviewed STSRL pull request and a passing
source verifier against the exact manifest commit.

## Useful Verification

Check the active integration branch from a clean environment:

```bash
git ls-remote https://github.com/lsmfttb/sts_lightspeed.git refs/heads/stsrl/main
```

Report the resolved commit whenever a STSRL task consumes or verifies this
fork.
