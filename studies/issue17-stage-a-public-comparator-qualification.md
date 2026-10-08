# Issue 17 — Stage A public comparator qualification

**Disposition:** `STUDY_ONLY`. This freezes a corrected public tactical comparator for independent review; it does not authorize the Issue 17 held-out comparison.

## Pinned source

- Native simulator: `lsmfttb/sts_lightspeed:spire/main`, `ab2b11bc3b5b6c6b68d9d855bc9545e9aca62a28`.
- Comparator source material: Issue 16 handoff commit `239d4d21e3a3a306590cde2046ef8b020ba5b179`, file `apps/study_issue16_prospective_corrected_comparison.cpp`, blob `ffff15085ed7dcfc18070f77d0f5363464df1599`.
- Corrected Stage A code-freeze commit: `333ab01b5afc49231b9cde1b763546c8876afc40`; qualification runner blob `7f4d5787f3a0c1ade392fbd798cc43c6577a624b`.

## Native schema evidence

The qualification runner called `StepSimulator.public_battle_state()` on 48 naturally reached, supported Act 1 Elite opening states from development seeds 1–48, all previously exposed. Each state reported `schema_id=native-public-battle-state-v1`, `information_regime=normal_public`, and `information_fidelity=supported`. The comparator selected an action present in `ordered_public_legal_actions`; `stepPublicAction()` accepted that exact public action identity.

| Encounter | Public monster fields | Intent and visible damage | Selected legal action |
|---|---|---|---|
| Gremlin Nob (seed 1, floor 6) | `id=25`, `id_label=GREMLIN_NOB`, `name=GREMLIN_NOB`, 88/88 HP, 0 block | `GREMLIN_NOB_BELLOW`, `NON_ATTACK`, 0 × 0 | `card`, hand index 2, Strike |
| Lagavulin (seed 2, floor 6) | `id_label=LAGAVULIN`, `name=LAGAVULIN`, 113/113 HP, 8 block | `LAGAVULIN_SLEEP`, `NON_ATTACK`, 0 × 0 | `card`, hand index 1, Strike |
| Three Sentries (seed 6, floor 6) | Three `id_label=SENTRY` / `name=SENTRY` rows, 41/39/44 HP, 0 block | Two `SENTRY_BOLT` non-attacks; one `SENTRY_BEAM`, `ATTACK`, 10 × 1 | `card`, hand index 2, Anger targeting Sentry index 1 |

Across the 48 snapshots, observed card `type`/`name` values included the four types `ATTACK`, `SKILL`, `POWER`, and `CURSE`; public card `id`, `cost_for_turn`, `upgraded`, and `free_to_play_once` fields had the types the baseline reads. Names used by its damage/block estimates (including Strike, Defend, Feel No Pain, and Power Through) appeared in native output. Public potion `name` and `id_label` fields were strings; native output included the exact compared labels `Blood Potion`, `Block Potion`, and `Fire Potion` as well as other potion names and `EMPTY_POTION_SLOT`. Player HP, max HP, energy, block, and weak were integers; monster HP, max HP, block, intent flags, base damage, and hit count had the expected integer/boolean encodings. Action kinds observed were `card`, `end_turn`, `potion`, and `potion_discard`; action indices `idx1`/`idx2`/`idx3` were integers. The baseline intentionally leaves potion-discard actions unselected to preserve potions; the native legal end-turn action remains available.

## Corrected identity branch and causal score trace

The old comparison looked for display text `Gremlin Nob`. Native output instead emitted both canonical `id_label=GREMLIN_NOB` and `name=GREMLIN_NOB`. The baseline now identifies a living Nob through the canonical public `id_label`; it uses no private runtime identity. All seven existing heuristic sanity checks remain, including the blocked-target lethal check.

On the naturally reached Nob snapshot (seed 1, turn 0), the legal hand contained playable `Feel No Pain` (`POWER`, hand index 4, cost 1) and competing actions. Its recorded priority score was **−52** with the real Nob identity and **22** in a paired public-state control, a **74-point** change; the `identified_gremlin_nob` branch and penalty flag were true only for the real identity. The control changed the encounter ID and monster `id`/`id_label` to `JAW_WORM`, while holding the emitted display name, HP/block, intent, hand, and legal actions fixed. This isolates the canonical-ID branch and makes a display-name-only implementation fail the check.

The 50-point Nob adjustment and safe-window preference are frozen heuristic score choices. They are not claims about game mechanics or a rule that Gremlin Nob mechanically punishes Powers.

## Verification and boundary

- Built `study-issue17-public-comparator-qualification` and `test-public-battle-state-semantics` from the pinned native source.
- `--heuristic-check`: `ISSUE17_PUBLIC_HEURISTIC_SANITY_PASS cases=7`.
- Native qualification: `ISSUE17_STAGE_A_PUBLIC_COMPARATOR_QUALIFICATION_PASS`; three distinct Elite encounter examples; causal Power score delta 74.
- `test-public-battle-state-semantics`: `PUBLIC_BATTLE_STATE_SEMANTICS_PASS`.
- Only previously exposed development seeds 1–48 were used for setup and schema checks. Seeds 49–72 were not loaded or executed; no held-out outcome or comparator-strength claim was produced.
- This qualification establishes that the comparator reads the emitted public schema and executes the intended identity branch. It does not establish baseline strength or validate the prospective search comparison.
