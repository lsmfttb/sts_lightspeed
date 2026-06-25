# STSRL Native Change Report

## STSRL Linkage

- STSRL task ID:
- STSRL task link:
- Fork issue:

## Source Identity

- External repository URL: https://github.com/lsmfttb/sts_lightspeed
- Base fork ref:
- Base fork commit:
- Implementation branch:
- Target active branch: `stsrl/main`
- Exact resulting commit intended for STSRL manifest pinning:
- Did `stsrl/main` advance? yes/no
- Tags created: none / list
- Historical branches changed or deleted: none / list with approval link

## Native API Surface

- Changed native API names:
- Changed pybind exposure:
- Information regime classification:
  - `normal_public_policy`
  - `normal_belief_search`
  - `full_simulator_state_oracle_like`
  - `sl_attempt_budgeted`

Hidden state exposure statement:

Parity or mechanics risk notes:

Known missingness or unsupported fields:

## Evidence

Native build command and result:

```text

```

Python import/API smoke command and result:

```text

```

Expected STSRL source-verifier impact:

```text

```

## STSRL Acceptance Boundary

- [ ] This PR does not claim STSRL implementation by itself.
- [ ] STSRL acceptance still requires a reviewed STSRL PR.
- [ ] STSRL must pin an exact external commit in
      `docs/sts_lightspeed_source_manifest.json`.
- [ ] STSRL source-verifier evidence must come from a clean checkout of that
      manifest commit.
- [ ] No normal-information path receives hidden simulator state.
- [ ] No Slay the Spire mechanics are changed for training convenience.
