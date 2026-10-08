# Issue 17 — Stage B prospective comparison

**Status:** Stage B run complete; pending independent Reviewer validation.  
**Proposed code disposition:** `STUDY_ONLY`.

## Frozen inputs and execution

- Native simulator base: `lsmfttb/sts_lightspeed:spire/main` at `ab2b11bc3b5b6c6b68d9d855bc9545e9aca62a28`.
- Stage A reviewed/frozen runner head: `6170ec426fbe27dfd5683af049a618161715782b`; comparator source `apps/study_issue17_public_tactical_heuristic.hpp` at blob `387a1d3252a4ffae01e796fc4ec1ee796328cef4`. No comparator, score, budget, or route changes were made after the Stage A pass.
- Stage A PASS was accepted in Issue #17 before these outcomes were opened. Seeds 49–72 were then run once as the declared held-out cohort; no seeds from Issues #14–16 are pooled here.
- Run matrix: 24 baseline battles (one per game seed), plus 48 shared-search battles at B=192 and 48 at B=384 (replicate seeds 17 and 101). Each budget is total simulations per actual decision; particle count is 32.
- Command: `study-issue17-prospective-comparison ab2b11bc3b5b6c6b68d9d855bc9545e9aca62a28 6170ec426fbe27dfd5683af049a618161715782b studies/issue17-stage-b-prospective-results.json` (full-study mode; no `--smoke`).
- Environment: WSL Ubuntu, GNU C++ 15.2.0, CMake 4.2.3, Python 3.14.4. Build used pinned JSON `0b345b20c888f7dc8888485768e4bf9a6be29de0` and pybind11 `d03662f0984f652b60e7ddce53d3868002275197` sources and `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` for the older JSON CMake policy.

## Verification

- Configured with `cmake -S . -B build -DCMAKE_POLICY_VERSION_MINIMUM=3.5` and built `study-issue17-prospective-comparison` plus `test-public-battle-state-semantics` successfully.
- The Stage B runner `--heuristic-check` passed all 7 sanity cases; `test-public-battle-state-semantics` returned `PUBLIC_BATTLE_STATE_SEMANTICS_PASS`.
- The full run returned `ISSUE17_PROSPECTIVE_PUBLIC_SEARCH_STUDY_PASS`, with 120 attempts, 24 elite starts, and 0 setup failures. A read-only evidence audit confirmed 5 rows per seed, identical paired start identities, and the single reported fail-closed attempt.
- Submodule Git cloning stalled in this environment, so the build used GitHub source archives at the exact JSON and pybind11 gitlink commits listed above. These files are build dependencies only; neither submodule gitlink changed.
## Cohort and sampler coverage

All 24 seeds reached a supported first Act 1 Elite start; no prelude setup failures. Encounter mix was 9 Gremlin Nob, 6 Lagavulin, and 9 Three Sentries. All controller restores matched the same full checkpoint and public start identity per seed.

- B=192: 47/48 battles completed. One attempt (seed 55, replicate 101) failed closed at public decision 17 with `sampler_rejected_other` (`public-consistent sampling requires supported public fidelity`). The other 47 completed. Search sampling support was 677/678 attempted decisions (99.85%).
- B=384: 48/48 battles completed, with 750/750 supported decisions (100%).
- Baseline: all 24 battles completed. Its diagnostic sampler-support probe was supported on 471/473 decisions; this probe does not gate or alter the baseline action.

`sampler_rejected_other` is not scored as a defeat. No setup or controller errors occurred. The complete per-attempt and per-decision evidence is in [`issue17-stage-b-prospective-results.json`](issue17-stage-b-prospective-results.json).

## Per-seed outcomes

Cells show outcome and HP lost during the target battle. `W` is win, `L` is loss. The incomplete B=192 attempt is reported explicitly.

| Seed | First Elite | Start HP | Baseline | B=192 r17 | B=192 r101 | B=384 r17 | B=384 r101 |
|---:|---|---:|---|---|---|---|---|
| 49 | Gremlin Nob | 41 | W (37 HP lost) | W (11 HP lost) | W (11 HP lost) | W (0 HP lost) | W (16 HP lost) |
| 50 | Three Sentries | 21 | L (21 HP lost) | L (21 HP lost) | L (21 HP lost) | L (21 HP lost) | L (21 HP lost) |
| 51 | Three Sentries | 68 | L (68 HP lost) | L (68 HP lost) | L (68 HP lost) | L (68 HP lost) | L (68 HP lost) |
| 52 | Three Sentries | 60 | W (51 HP lost) | W (43 HP lost) | W (38 HP lost) | W (44 HP lost) | W (46 HP lost) |
| 53 | Three Sentries | 27 | L (27 HP lost) | W (19 HP lost) | L (27 HP lost) | W (17 HP lost) | W (12 HP lost) |
| 54 | Lagavulin | 42 | L (42 HP lost) | L (42 HP lost) | L (42 HP lost) | L (42 HP lost) | L (42 HP lost) |
| 55 | Three Sentries | 75 | W (50 HP lost) | W (35 HP lost) | Incomplete (sampler_rejected_other, decision 17) | W (45 HP lost) | W (40 HP lost) |
| 56 | Gremlin Nob | 48 | L (48 HP lost) | W (43 HP lost) | L (48 HP lost) | W (24 HP lost) | W (13 HP lost) |
| 57 | Lagavulin | 37 | L (37 HP lost) | W (35 HP lost) | W (25 HP lost) | W (15 HP lost) | W (35 HP lost) |
| 58 | Lagavulin | 41 | L (41 HP lost) | L (41 HP lost) | L (41 HP lost) | L (41 HP lost) | L (41 HP lost) |
| 59 | Gremlin Nob | 72 | L (72 HP lost) | L (72 HP lost) | L (72 HP lost) | L (72 HP lost) | L (72 HP lost) |
| 60 | Lagavulin | 54 | L (54 HP lost) | L (54 HP lost) | W (42 HP lost) | L (54 HP lost) | W (38 HP lost) |
| 61 | Three Sentries | 30 | L (30 HP lost) | L (30 HP lost) | L (30 HP lost) | L (30 HP lost) | L (30 HP lost) |
| 62 | Three Sentries | 37 | L (37 HP lost) | L (37 HP lost) | L (37 HP lost) | L (37 HP lost) | L (37 HP lost) |
| 63 | Gremlin Nob | 34 | L (34 HP lost) | L (34 HP lost) | L (34 HP lost) | L (34 HP lost) | L (34 HP lost) |
| 64 | Gremlin Nob | 32 | L (32 HP lost) | L (32 HP lost) | L (32 HP lost) | L (32 HP lost) | L (32 HP lost) |
| 65 | Three Sentries | 72 | L (72 HP lost) | L (72 HP lost) | L (72 HP lost) | L (72 HP lost) | L (72 HP lost) |
| 66 | Lagavulin | 52 | L (52 HP lost) | L (52 HP lost) | L (52 HP lost) | L (52 HP lost) | L (52 HP lost) |
| 67 | Lagavulin | 59 | W (35 HP lost) | W (35 HP lost) | L (59 HP lost) | L (59 HP lost) | L (59 HP lost) |
| 68 | Gremlin Nob | 57 | L (57 HP lost) | W (50 HP lost) | L (57 HP lost) | W (44 HP lost) | L (57 HP lost) |
| 69 | Gremlin Nob | 59 | L (59 HP lost) | L (59 HP lost) | L (59 HP lost) | L (59 HP lost) | L (59 HP lost) |
| 70 | Gremlin Nob | 75 | L (75 HP lost) | W (71 HP lost) | W (71 HP lost) | W (52 HP lost) | W (58 HP lost) |
| 71 | Gremlin Nob | 68 | W (52 HP lost) | W (33 HP lost) | W (48 HP lost) | W (16 HP lost) | W (16 HP lost) |
| 72 | Three Sentries | 15 | L (15 HP lost) | L (15 HP lost) | L (15 HP lost) | L (15 HP lost) | L (15 HP lost) |

## Paired seed-cluster comparisons

Effects are search minus baseline; HP deltas are HP lost. For uncertainty, the runner averaged replicate effects within each game seed, then used 10,000 game-seed-cluster bootstrap resamples. Intervals are descriptive for this small fixed cohort, not confirmatory significance claims.

| Comparison | Terminal paired runs | Complete seed clusters | Mean win delta (95% bootstrap interval) | Mean HP-loss delta (95% bootstrap interval) |
|---|---:|---:|---:|---:|
| B=192 vs baseline | 47/48 | 23 | 15.22% [2.17%, 30.43%] | -2.74 HP [-5.65, -0.26] |
| B=384 vs baseline | 48/48 | 24 | 16.67% [0.00%, 35.42%] | -5.96 HP [-10.90, -1.25] |
| B=384 vs B=192 | 47/48 | 23 | 2.17% [-4.35%, 8.70%] | -3.15 HP [-6.83, -0.02] |

The B=192 comparison has 23 complete clusters because the unsupported seed-55 replicate is incomplete; the internal B=384-vs-B=192 comparison likewise uses 23 complete clusters. B=384 vs baseline includes all 24 seeds.

## Work and latency

Wall times are local single-process measurements from this WSL run. Target-battle p50/p95 covers the battle loop; controller-action p50/p95 excludes the baseline sampler probe. Search-decision p50/p95 is for decisions where search ran.

| Policy | Wins / losses / incomplete | Mean HP loss (completed) | Shared simulations | Native public-action steps | Target-battle wall p50/p95 (s) | Controller-action wall p50/p95 (s) | All-decision wall p50/p95 (s) | Search-decision wall p50/p95 (s) |
|---|---:|---:|---:|---:|---:|---:|
| Baseline | 5 / 19 / 0 | 45.75 HP | 0 | 473 | 0.021 / 0.045 | 0.008 / 0.017 | 0.000436 / 0.000790 | n/a |
| B=192 | 16 / 31 / 1 | 42.66 HP | 129984 | 1670050 | 12.319 / 70.598 | 12.319 / 70.596 | 1.356 / 2.528 | 1.358 / 2.526 |
| B=384 | 18 / 30 / 0 | 39.79 HP | 288000 | 3770625 | 25.711 / 158.950 | 25.710 / 158.948 | 2.839 / 5.183 | 2.838 / 5.182 |

## Interpretation and limits

The analysis gate uses complete game-seed-cluster mean win deltas, matching the cluster bootstrap estimand. It assigns `NO_CLEAR_SIGNAL`: B=192's seed-cluster win interval lower bound is positive, but B=384's is exactly 0, so the predeclared requirement that **both** lower bounds be strictly positive is not met. The mean win deltas are positive at both budgets, and mean HP loss is lower than baseline at both, but this small outcome cohort does not establish policy strength or broad A20 performance. B=384 also costs more wall time than B=192; no online latency ceiling was specified, so `ECONOMIC_LIMIT` was not applied. The JSON labels any retained completed-pair averages as `terminal_pair_weighted_mean_*`; they do not drive the category. This analysis correction preserved all 120 Stage B outcomes and did not rerun seeds.

Limitations: the cohort is 24 first-Act-1-Elite starts selected by a deterministic public prelude; the hand-built baseline is not a strong-player or oracle comparator and its static damage proxy omits multihit counts, area effects, and card-specific modifiers; future RNG streams can diverge after paired actions differ; unsupported search attempts stop without fallback; rollouts truncate at 24 public decisions with a public HP-fraction leaf heuristic; timings are local; and clustered intervals from this small cohort are not a confirmatory significance claim. Do not pool with seeds 1–48 or promote the study comparator/search into native core.


