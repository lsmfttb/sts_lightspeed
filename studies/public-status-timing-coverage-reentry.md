# Public status timing coverage re-entry

This is a coverage check for Issue #11, not an outcome study. The replay used the Issue #10 adaptive shared-public-tree runner over its retained first-battle cohort subset: Ironclad A0, game seeds 1, 3, and 4, replicate seed 17, 32 particles, and per-decision budgets 96 and 384. Each run used the same first-legal-action setup and stopped on unsupported public fidelity without fallback. Status implementation: `5f086866f75fcd41804cdf4c2735f043307e9f74`.

| Seed / encounter | Budget | Supported / attempted decisions | Result |
| --- | ---: | ---: | --- |
| 1 / Cultist | 96 | 15 / 15 | Complete |
| 1 / Cultist | 384 | 13 / 13 | Complete |
| 3 / Small Slimes | 96 | 18 / 19 | Stopped at decision 18: `insertion_membership_unrepresented` |
| 3 / Small Slimes | 384 | 21 / 22 | Stopped at decision 21: `insertion_membership_unrepresented` |
| 4 / Jaw Worm | 96 | 9 / 9 | Complete |
| 4 / Jaw Worm | 384 | 11 / 11 | Complete |

The previous `monster_status_timing` and `player_status_timing` failures did not recur in these six attempts. Four of six battles completed overall (two of three at each budget). Both Small Slimes attempts later failed closed because the draw-pile insertion membership was not representable. This subset establishes re-entry past the prior status blocker, not broad coverage or a battle-strength signal.

Focused native verification: Release target `test-public-battle-state-semantics` built and printed `PUBLIC_BATTLE_STATE_SEMANTICS_PASS`, including Issue #9 fail-closed regressions.
