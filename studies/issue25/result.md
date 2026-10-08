# Issue 25: public-only run coverage frontier

Disposition: **STUDY_ONLY**. Outcomes: `NATURAL_PUBLIC_SEARCH_EXECUTED, PUBLIC_NONCOMBAT_COVERAGE_GAP`.

Provenance: reviewed Issue 24 source `c6021fb4a59d6ef73db6cdca1de981957937078d`; native source `fdc15032d1d363c6329dc85f9ae56aea92e1a349` (base `d1dcd6534ec4a1f38ac1f7f916f01a3f931fcfd0`); STSRL executor `3037b75eca4bd73fa70d018ffd4442a1f2d65628`; public adapter `77771184827de85a0125177b852e762d2d1372dd`; tactical baseline `9a2792e1e02157124b4f90edc91b7ad8765d5d10`.

| Seed | Decisions / screen coverage | Controller modes | Highest verified act/floor; battle turns; encounters | Public HP/gold/potions at stop | Wall / search cost | Stop |
|---:|---|---|---|---|---|---|
| 49 | 42 — EVENT_SCREEN×1, REWARDS×15, MAP_SCREEN×4, BATTLE×22 | expert_non_combat×20, shared_public_tree_B192×1, issue17_public_tactical_heuristic×21 | A1 / F3; turn≤2; SMALL_SLIMES, JAW_WORM, TWO_LOUSE | HP 38/75; gold 201; potions 0/2 | 7.180s / 192 sim/32 particles, 2551 native actions, 3.485s | `PUBLIC_NONCOMBAT_COVERAGE_GAP` |
| 50 | 77 — EVENT_SCREEN×4, REWARDS×19, MAP_SCREEN×8, BATTLE×46 | expert_non_combat×31, shared_public_tree_B192×1, issue17_public_tactical_heuristic×45 | A1 / F6; turn≤3; TWO_LOUSE, JAW_WORM, SMALL_SLIMES, LOOTER | HP 7/75; gold 117; potions 1/2 | 13.235s / 192 sim/32 particles, 2858 native actions, 4.059s | `PUBLIC_NONCOMBAT_COVERAGE_GAP` |

First stop detail per seed:
- Seed 49: `PUBLIC_NONCOMBAT_COVERAGE_GAP screen=SHOP_ROOM field=screen_payload.coverage_status source=StepSimulator::publicProjection screen coverage and choices value=unsupported reason=shop choice coverage is not part of this capability`
- Seed 50: `PUBLIC_NONCOMBAT_COVERAGE_GAP screen=TREASURE_ROOM field=screen_payload.coverage_status source=StepSimulator::publicProjection screen coverage and choices value=unsupported reason=treasure-room choice coverage is not part of this capability`

Causal next action: After review, scope the smallest task-independent public choice renderer for the first unsupported screen(s) SHOP_ROOM, TREASURE_ROOM; keep decisions fail-closed until that exact renderer is available.

Each fixed seed is capped at 96 controller decisions and 180 seconds. An outer supervisor kills the runner process tree at the per-seed wall deadline and preserves the last atomic checkpoint in `result.progress.json`; the completion report is not rewritten after a timeout. The unsupported Shop fixture passed with no policy callback or selected action; it is excluded from natural-run progress. Search counts as executed only if native root replay, the 32-particle public-consistent sampler, anchor invariance, and fixed B=192 search all complete.

The sampler law is a public-consistent proposal Q, not an exact posterior. This study stays disposable until independent review; any core promotion is a separate decision.

See `result.json` for the per-decision public identity/resource trace, fidelity and availability provenance, cumulative grounded work, and exact stop boundary. `result.progress.json` records the last durable execution boundary.
