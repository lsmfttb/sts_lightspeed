---
name: STSRL native change
about: Track a native simulator change needed by STSRL.
title: "STSRL-XXX: "
labels: ""
assignees: ""
---

## STSRL Task

- STSRL task ID:
- STSRL task link:
- Required native capability:
- Public API or telemetry expected by STSRL:

## Objective

Describe the minimal native C++ or pybind work needed in this fork.

## Source And Branch Plan

- Base fork ref:
- Base fork commit:
- Implementation branch:
- Target active branch: `stsrl/main`
- Exact resulting commit intended for STSRL manifest pinning:
- Will `stsrl/main` advance? yes/no
- Tags to create: none / list
- Historical branches to change or delete: none / list with approval link

## Native Surface

- Changed native API names:
- Changed pybind exposure:
- Expected Python import/API smoke command:
- Expected native build command:

## Information Regime

Select every regime that applies and explain why.

- [ ] `normal_public_policy`
- [ ] `normal_belief_search`
- [ ] `full_simulator_state_oracle_like`
- [ ] `sl_attempt_budgeted`

Hidden state exposure statement:

Parity or mechanics risk notes:

Known missingness or unsupported fields:

## STSRL Acceptance

This fork issue does not mark the capability implemented in STSRL. STSRL
acceptance still requires a reviewed STSRL pull request, an exact
`docs/sts_lightspeed_source_manifest.json` update, and source-verifier evidence
from a clean checkout of the manifest commit.

Expected STSRL source-verifier impact:

## Out Of Scope

- Changing Slay the Spire mechanics for training convenience.
- Updating the STSRL source manifest from this fork.
- Deleting historical branches without explicit STSRL maintainer approval.
