# Issue 17 — Stage A source-bound requalification

**Disposition:** `STUDY_ONLY`. The prospective Stage B runner is prepared but remains gated on independent Stage A review.

## Frozen source

- Native simulator base: `lsmfttb/sts_lightspeed:spire/main`, `ab2b11bc3b5b6c6b68d9d855bc9545e9aca62a28`.
- Stage A comparator code-freeze commit: `f5ec56c4e62200fcc7ba3348e1a53eaa1938f1cd`.
- Issue 16 source provenance: handoff `239d4d21e3a3a306590cde2046ef8b020ba5b179`, frozen runner `41e71c1aba59caeb469c63ffd481a2236e52f353`.
- Stage B runner source: `apps/study_issue17_prospective_public_comparison.cpp`, pinned to game seeds 49–72. Both it and the Stage A qualifier include the same comparator from `apps/study_issue17_public_tactical_heuristic.hpp` (blob `387a1d3252a4ffae01e796fc4ec1ee796328cef4`). That shared source owns the block/damage estimates, selector, canonical Nob identity branch, and branch/score audit.

## Native public-snapshot evidence

The requalification called `StepSimulator.public_battle_state()` on naturally reached supported Act 1 Elite states from previously exposed development seeds 1–48. It checked 48 native Elite snapshots. Every baseline selection was present in `ordered_public_legal_actions`, and `stepPublicAction()` accepted the exact selected public action.

| Encounter | Native public fields | Selected legal action |
|---|---|---|
| Gremlin Nob (seed 1, floor 6) | `id=25`, `id_label/name=GREMLIN_NOB`, 88/88 HP, 0 block; `GREMLIN_NOB_BELLOW`, non-attack, 0×0 | Card index 2, Strike |
| Lagavulin (seed 2, floor 6) | `id_label/name=LAGAVULIN`, 113/113 HP, 8 block; `LAGAVULIN_SLEEP`, non-attack, 0×0 | Card index 1, Strike |
| Three Sentries (seed 6, floor 6) | `id_label/name=SENTRY`, 41/39/44 HP, 0 block; two bolts and one beam at 10×1 | Card index 2, Anger targeting Sentry index 1 |

The 48 snapshots exercised card types/names, potion names, monster identity/intent, HP/block, and public action kinds consumed by the heuristic. Observed action kinds were `card`, `end_turn`, `potion`, and `potion_discard`; all public fields were accepted with the expected string, integer, and boolean types. The emitted Nob identity and name were both `GREMLIN_NOB`.

## Executed Nob-branch effect

On the real Nob state at seed 1, turn 0, the eligible `Feel No Pain` Power (hand index 4, cost 1) scored **−52** and recorded branch `identified_gremlin_nob`. In an otherwise identical public-state control, only encounter ID and monster `id`/`id_label` changed to `JAW_WORM`; the name, HP/block, intent, hand, and legal actions stayed fixed. The Power then scored **22** with no Nob penalty, a **74-point** difference. This directly verifies the canonical-ID branch in the shared source; it does not describe game mechanics.

## Verification and boundary

- Built `study-issue17-public-comparator-qualification`, `study-issue17-prospective-comparison`, and `test-public-battle-state-semantics` against the pinned native base and submodule revisions.
- Stage A qualifier `--heuristic-check`: `ISSUE17_PUBLIC_HEURISTIC_SANITY_PASS cases=7`.
- Stage B runner `--heuristic-check`: `ISSUE17_PUBLIC_HEURISTIC_SANITY_PASS cases=7`.
- Native semantics check: `PUBLIC_BATTLE_STATE_SEMANTICS_PASS`.
- Native requalification, with Stage B source commit `f5ec56c4e62200fcc7ba3348e1a53eaa1938f1cd`: `ISSUE17_STAGE_A_PUBLIC_COMPARATOR_QUALIFICATION_PASS`; 3/3 encounter examples; causal score delta 74.
- Seeds 49–72 were not loaded or executed. The Stage B binary was compiled and its seven heuristic sanity cases were run; the outcome runner itself was not executed.
- The 50-point Power adjustment and safe-window preference are frozen heuristic choices. This is schema/branch qualification only, not a baseline-strength claim or a search-quality estimate.
