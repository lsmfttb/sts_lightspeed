# Issue #14: paired normal-public outcome signal

**Status:** exploratory outcome signal; `STUDY_ONLY`
**Simulator base:** `ab2b11bc3b5b6c6b68d9d855bc9545e9aca62a28`
**Runner source:** `78c272513e213294da348d0e3ac1695289c8e801`
**Full attempt evidence:** [`issue14-paired-outcome-signal.json`](issue14-paired-outcome-signal.json)

## Prospective cohort

The fixed cohort was Ironclad A20 game seeds 1–12. The native deterministic public prelude reached the first Act 1 Elite for 11 seeds; seed 6 lost before the target and remains in all-attempt denominators. The 11 starts were all at floor 6: 3 Gremlin Nob, 6 Lagavulin, and 2 Three Sentries. Starting HP ranged from 11 to 71. Each seed's complete simulator checkpoint was captured before any policy outcomes and copied for every controller run. All observed search decisions were sampler-supported; no unsupported attempt was treated as a defeat.

## Outcomes and cost

| Controller | Attempts | Target starts | Terminal battles | Wins / losses | Unsupported search completions | Simulations | Battle wall time p50 / p95 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Public heuristic baseline | 12 | 11 | 11 | 0 / 11 | n/a | 0 | 0.004 / 0.010 s |
| Shared search, B=96 | 24 | 22 | 22 | 3 / 19 | 0 (22/22 completed) | 28,512 | 5.05 / 24.15 s |
| Shared search, B=384 | 24 | 22 | 22 | 10 / 12 | 0 (22/22 completed) | 155,520 | 27.91 / 116.76 s |

Search runs had two attempts per budget that did not reach the target because seed 6 lost during setup. The baseline is deterministic, so it was run once per game seed. Every search run that reached a target battle completed with supported sampling.

| Paired comparison | Terminal pairs | Mean win delta | 95% seed-cluster bootstrap interval | Mean HP-loss delta | 95% seed-cluster bootstrap interval |
| --- | ---: | ---: | ---: | ---: | ---: |
| B=96 vs baseline | 22 / 24 attempts | +0.136 | [0.000, 0.364] | −3.23 HP | [−7.95, 0.00] |
| B=384 vs baseline | 22 / 24 attempts | +0.455 | [0.182, 0.727] | −4.23 HP | [−8.36, −0.91] |
| B=384 vs B=96 | 22 | +0.318 | [0.091, 0.545] | −1.00 HP | [−5.77, 4.86] |

The intervals average the two search replicates within each of the 11 complete game-seed clusters, then resample seeds with replacement (10,000 draws, fixed seed). They describe this small prospective cohort; they are not a confirmatory significance claim. HP-loss differences include terminal wins and losses, not only wins. Incomplete/setup attempts remain visible in the all-attempt summaries and have no terminal HP delta.

## Interpretation and limits

The fixed cohort shows a positive paired outcome signal for search, especially at B=384: the baseline had no wins, while B=384 won 10 of 22 terminal paired attempts across 6 of 11 starts. B=96 won 3 of 22. The B=384 versus B=96 win-delta interval is positive, while the HP-loss interval spans zero. This does not establish broad A20 strength or a general win-rate improvement; the baseline is a simple heuristic, and these outcomes cover only first Act 1 Elite battles.

The compute cost is substantial: median battle time was about 28 seconds at B=384 and 5 seconds at B=96, versus about 4 milliseconds for the baseline. Timings are single-process local measurements from a WSL/Linux build on a mounted Windows path, exclude compilation, and are machine-specific. The result supports a cost-aware follow-up: independently judge the baseline, then profile/optimize B=384 or evaluate a capped budget before committing to a larger cohort. All code and evidence remain study-only; no native-core promotion is proposed.
