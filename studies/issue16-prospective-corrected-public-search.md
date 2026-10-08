# Issue #16: prospective corrected A20 first-Elite comparison

**Result:** `NO_CLEAR_SIGNAL`  
**Proposed disposition:** `STUDY_ONLY`

## Frozen protocol and coverage

The run used native base `sts_lightspeed:spire/main` at `ab2b11bc3b5b6c6b68d9d855bc9545e9aca62a28`, corrected comparator source `85570f1fda24e30a49ee99756d20be9233e2084a`, and frozen runner commit `41e71c1aba59caeb469c63ffd481a2236e52f353`. The comparator id is `public-tactical-v1-blocked-lethal-fixed`. Issue #15 outcomes were excluded.

Seeds 25–48 each reached their first naturally encountered Act 1 Elite. There were 24 seed clusters, 120/120 completed policy attempts, no pre-target setup failures, no unsupported public states, and no incomplete battles. The encounter mix was Gremlin Nob 9, Lagavulin 7, and Three Sentries 8. Each seed's five attempts shared one verified public start identity.

The cohort contains one corrected-baseline attempt per seed and two search replicates at each budget. The paired intervals below resample 24 complete game-seed clusters (10,000 bootstrap replicates); the 48 replicate rows are not treated as independent clusters.

## Outcomes and costs

| Policy | Wins | Mean paired win change vs baseline (95% seed-cluster interval) | Mean paired HP-loss change vs baseline (95% interval) | Target-battle elapsed p50 / p95 | Decision p50 / p95 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Corrected baseline | 8/24 (33.3%) | — | — | 0.021 / 0.035 s | 0.00034 / 0.00070 s |
| Shared search, B=192 | 18/48 (37.5%) | +4.2 pp (−14.6, +22.9) | −1.10 HP (−5.23, +3.06) | 13.84 / 62.12 s | 1.396 / 2.457 s |
| Shared search, B=384 | 23/48 (47.9%) | +14.6 pp (−6.3, +35.4) | −2.67 HP (−7.33, +1.90) | 29.51 / 117.70 s | 2.758 / 4.813 s |

B=384 versus B=192 had a paired win change of +10.4 pp (95% interval +2.1 to +20.8) and HP-loss change of −1.56 HP (−3.46 to +0.27). The larger budget took about 2.13× the median battle time and 1.89× the p95 battle time of B=192.

Search used 148,608 total simulations and 1,983,761 native public-action steps at B=192; B=384 used 308,352 simulations and 4,058,208 steps. Every search attempt completed with public support. Target-battle latency covers the restored battle loop and excludes the prelude and compilation; measurements are from a local single-process run.

## Interpretation and limits

Both search budgets had positive point estimates against the corrected baseline, but both seed-cluster win intervals include zero. The frozen classification is therefore `NO_CLEAR_SIGNAL`: this cohort does not establish a repeatable outcome signal against the corrected comparator. B=384 was slower and showed a positive paired contrast against B=192, with substantial uncertainty in HP-loss differences.

This is a small exploratory comparison at first Act 1 Elites. The hand-built baseline is not a certified strong policy; damage estimates are approximate, future random streams can diverge after actions differ, and these results do not establish broad A20 or Heart strength, expert-agent superiority, an exact posterior, or Q-public optimality.

## Verification

- CMake configured with `CMAKE_POLICY_VERSION_MINIMUM=3.5` for the pinned older JSON dependency.
- Built `study-issue16-prospective-corrected-comparison` and `test-public-battle-state-semantics`.
- `--heuristic-check`: `ISSUE16_PUBLIC_HEURISTIC_SANITY_PASS cases=7`.
- Public battle semantics executable: `PUBLIC_BATTLE_STATE_SEMANTICS_PASS`.
- Full frozen cohort: `ISSUE16_PROSPECTIVE_CORRECTED_PUBLIC_SEARCH_STUDY_PASS`, `runs=120 elite_starts=24 setup_failures=0`.
- Evidence JSON (gzip-compressed per-attempt records and aggregates): `issue16-prospective-corrected-public-search.json.gz`; recover the original JSON with `gzip -dk issue16-prospective-corrected-public-search.json.gz`.
