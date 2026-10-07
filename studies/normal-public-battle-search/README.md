# Normal-public complete-battle feasibility

**Conclusion: `COVERAGE_BLOCKED`.** The sampler was available at most decisions, but fail-closed public-state boundaries stopped 22 of 24 attempts at each search budget. Only two battles per budget completed with supported sampling at every decision. This is not enough complete-battle evidence to judge whether the larger budget improves outcomes; the next work should improve sampler coverage before increasing search strength.

## Protocol

- Native simulator base: `spire/main` at `3381617d98c793cf20850e3840469817a1315444`.
- Runner: `apps/study_end_to_end_public_battle.cpp` at `c28ca05c3a6180c465a812eafd226493ab5a0528`.
- Cohort: Ironclad, Ascension 0, first native battle for each fixed game seed 1–8. Before combat, the runner advances the seeded game using the first currently legal action. It does not filter by encounter or sampler support. The resulting starts include 4 Cultists, 2 Small Slimes, and 2 Jaw Worms.
- Replicates: controller/search/sampler seeds 17, 101, and 1009 for each game seed.
- Search budgets: 96 and 384 shared-tree simulations per supported public decision, with 32 fixed particles. The budget is not multiplied by particle count.
- Controller: one UCT tree per actual decision, shared across sampled particles. Tree keys contain only public state and public action/result history; the real action is selected from shared root visits and values and executed through `step_public_action`.
- Rollouts: terminal win/loss values are +1/−1. Rollouts that reach 24 public decisions use the public HP-fraction heuristic described in `results.json`.
- Unsupported decisions stop the trial. There is no full-state fallback or Oracle controller.

The retained `results.json` contains all 48 attempts, the deduplicated public battle-start manifest, per-decision seeds/support/errors/actions/timing/work, and budget summaries. HP lost is starting HP minus combat-ending HP, before post-combat relic healing. Wall times are local single-process measurements; compilation is excluded.

## Results

| Per-decision budget | Attempts | Fully supported completions | Supported decisions / attempted | Win/loss among completed | Decision latency p50 / p95 | Battle time p50 / p95 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 96 | 24 | 2 (8.3%) | 141 / 163 (86.5%) | 2 / 0 | 0.677 / 0.904 s | 3.25 / 8.51 s |
| 384 | 24 | 2 (8.3%) | 110 / 132 (83.3%) | 2 / 0 | 2.688 / 3.594 s | 12.19 / 23.14 s |

First unsupported decisions were `monster_status_timing` in 18 attempts and `player_status_timing` in 4 attempts at each budget. Across fully supported completions, both budget groups won, but only one game-seed/replicate pair completed at both budgets. That pair had unchanged win/loss and 21 fewer HP lost at budget 384. One pair is not a credible battle-level improvement signal.

The p50 and p95 decision latency increased about fourfold from 96 to 384 simulations. The fixed per-decision budget therefore bounded work as intended, while the tested increase did not improve coverage. The primary result is the low complete-battle coverage, not the outcomes of the small completed subset.

## Disposition

Study-only evidence; do not promote the runner or results into core. Route for independent review. If accepted, address public-state/sampler coverage before running a larger battle-outcome comparison.
