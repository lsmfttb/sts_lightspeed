# Issue 24: natural shared-public Battle search

Disposition: **STUDY_ONLY**. Outcomes: `NATURAL_PUBLIC_SEARCH_EXECUTED, COST_BLOCKED`.

Provenance: native source `925c39d4de6bb01a2d52e2bfec14d58ea0fac524` (base `d1dcd6534ec4a1f38ac1f7f916f01a3f931fcfd0`); STSRL executor `3037b75eca4bd73fa70d018ffd4442a1f2d65628`; public adapter `77771184827de85a0125177b852e762d2d1372dd`; tactical baseline `9a2792e1e02157124b4f90edc91b7ad8765d5d10`.

| Seed | Root replay | Public baseline | Counter fault control | Public sample pool | B=192 audit | Action executed | Stop |
|---:|---|---|---|---|---|---|---|
| 49 | yes | `battle.card idx1=2 idx2=1 idx3=2` | yes | yes | yes | yes | fixed_decision_budget |
| 50 | yes | `battle.card idx1=4 idx2=1 idx3=4` | yes | yes | yes | yes | fixed_decision_budget |

Each run uses the pinned STSRL controlled-run loop after four seeded ExpertNonCombatDriver decisions. Search counts as executed only if native root replay, the 32-particle public-consistent sampler, anchor invariance, and the fixed B=192 search all complete; a blocked run is not reported as a Battle choice. No win-rate, training, or continuation-strength claim is made.

The sampler law is a public-consistent proposal Q, not an exact posterior. This study stays disposable until independent review; any core promotion is a separate decision.

See `result.json` for exact public identities, root replay boundary, search and anchor-invariance evidence, public screen/resource trace, and the final stop boundary. `result.progress.json` records durable execution progress.
