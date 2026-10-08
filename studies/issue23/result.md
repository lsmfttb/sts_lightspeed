# Issue 23: natural shared-public Battle search

Disposition: **STUDY_ONLY**. Outcomes: `NATURAL_SEARCH_INTEGRATION_BLOCKED`.

Provenance: native `d1dcd6534ec4a1f38ac1f7f916f01a3f931fcfd0`; STSRL executor `3037b75eca4bd73fa70d018ffd4442a1f2d65628`; public adapter `77771184827de85a0125177b852e762d2d1372dd`; tactical baseline `9a2792e1e02157124b4f90edc91b7ad8765d5d10`.

| Seed | Root replay | Public baseline | Private proposal guard | Public sample pool | B=192 audit | Action executed | Stop |
|---:|---|---|---|---|---|---|---|
| 49 | yes | `{ use card (2) (Strike,4,1,1) -> (1) ACID_SLIME_M }` | yes | yes | yes | no | anchor_invariance_or_sampler_blocked |
| 50 | yes | `{ use card (4) (Strike,3,1,1) -> (1) RED_LOUSE }` | yes | not reached | no | no | anchor_invariance_or_sampler_blocked |

Each run uses the pinned STSRL controlled-run loop after four seeded ExpertNonCombatDriver decisions. Search counts as executed only if native root replay, the 32-particle public-consistent sampler, anchor invariance, and the fixed B=192 search all complete; a blocked run is not reported as a Battle choice. No win-rate, training, or continuation-strength claim is made.

Proposed follow-up: make the sampler depend only on the public root and fixed seed, support the private monster-future counters, and rerun the invariance audit. That changes the proposal law and needs Planner review.

See `result.json` for exact public identities, root replay boundary, search/anchor-invariance evidence when completed, public screen/resource trace, first blocker, and the proposed sampler follow-up. `result.progress.json` records the last durable execution boundary.
