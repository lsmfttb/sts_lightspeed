# Issue #15: held-out cost-capped public-search study

**Result category:** `NO_CLEAR_SIGNAL`<br>
**Code disposition:** `STUDY_ONLY`<br>
**Simulator base:** `ab2b11bc3b5b6c6b68d9d855bc9545e9aca62a28` (`spire/main`)<br>
**Frozen runner and heuristic:** `c813dc2a65afe00d8f323f965b775886a0a6a612`

## Cohort and pairing

The prospective A20 Ironclad seeds 13–24 all reached their first Act 1 Elite. The encounter mix was 6 Gremlin Nob, 4 Three Sentries, and 2 Lagavulin. Issue #14 seeds 1–12 were not used. One full checkpoint was captured per seed and restored for the baseline and both search settings; all public start identities matched. Later RNG streams could diverge after actions differed.

The deterministic public tactical baseline completed 12/12 battles and won 3. Each search budget had 24 attempts (two controller replicates for each seed); all 24 reached the target, completed, and had sampler support. The baseline outcome is paired to each of the two search replicates, so the 24 pairs represent 12 game-seed clusters, not 24 independent encounters.

## Outcomes

| Policy | Completed wins | Mean HP lost across wins and losses | Controller battle time p50 / p95 |
| --- | ---: | ---: | ---: |
| Public tactical baseline | 3/12 | 36.25 | 0.005 / 0.016 s |
| Shared public search, B=192 | 8/24 | 34.04 | 5.30 / 56.25 s |
| Shared public search, B=384 | 12/24 | 31.75 | 11.62 / 112.61 s |

| Paired comparison | Terminal pairs | Search win delta (seed-cluster 95% interval) | HP-loss delta (seed-cluster 95% interval) |
| --- | ---: | ---: | ---: |
| B=192 minus baseline | 24/24 | +0.083 [0.000, 0.250] | -2.21 [-5.79, +0.75] |
| B=384 minus baseline | 24/24 | +0.250 [0.000, 0.500] | -4.50 [-8.67, -0.88] |
| B=384 minus B=192 | 24/24 | +0.167 [0.000, 0.417] | -2.29 [-6.67, +1.29] |

Intervals are 10,000-replicate percentile bootstraps over complete game-seed clusters. The win-delta intervals include zero at both budgets. B=384 has a larger descriptive advantage and lower HP loss, while its p95 battle time is about twice B=192. This cohort does not establish a confirmatory advantage. HP deltas include terminal wins and losses, not wins alone.

## Work and latency

| Search budget | Total simulations | Native public-action steps | Search decision time p50 / p95 | Approx. per-decision phase p50: sampler / projection+keys / selection / transitions |
| --- | ---: | ---: | ---: | ---: |
| B=192 | 51,264 | 621,372 | 1.123 / 2.152 s | 0.033 / 0.307 / 0.114 / 0.284 s |
| B=384 | 104,448 | 1,237,536 | 2.121 / 4.317 s | 0.068 / 0.589 / 0.194 / 0.528 s |

Phase timers are lightweight exclusive call timers; projection/key coverage is partial and the phase totals need not equal total search time. Measurements are from one local WSL process and exclude setup and compilation. The measured p95 battle costs are large for a prospective online controller; the issue supplied no online latency ceiling, so this study does not assign `ECONOMIC_LIMIT`.

## Comparator and limits

`public-tactical-v1` uses only public battle state and legal public actions. It estimates card damage from native static per-hit base damage plus visible strength, Weak, Vulnerable, and target block; it uses a small named block-card table, cost-aware priorities, visible incoming intent, a Gremlin Nob setup penalty, situational Blood/Block potion thresholds, and Fire Potion only for an estimated lethal. Six pre-outcome synthetic checks cover lethal choice, defense under lethal pressure, Gremlin Nob setup, potion restraint, low-HP healing, and rejection of private particle metadata. The estimate omits multi-hit counts, area effects, and card-specific modifiers. It is a stronger-design comparator than Issue #14's unconditional potion-first/Power-first baseline, not an Oracle or certified strong player.

No setup failures, unsupported search decisions, or incomplete battle outcomes occurred. Search rollouts still truncate at 24 public-action decisions with a public HP-fraction evaluator. This is limited to first Act 1 Elites reached through the deterministic prelude; it is not a full-run, Heart, or broad A20 policy-strength result.

The raw paired evidence, per-attempt outcomes, decision audits, seed manifest, and cost timers are in [issue15-heldout-cost-capped-public-search.json](issue15-heldout-cost-capped-public-search.json).
