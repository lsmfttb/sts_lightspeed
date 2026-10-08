# Issue #19: normal-public whole-run compatibility

## Result

**Disposition:** `PUBLIC_NONCOMBAT_FIDELITY_GAP` plus `ADAPTER_COMPATIBILITY_GAP`; keep this branch **study-only**. Do not start the Stage 2 A20 pilot or merge these study checks into the native core. The active simulator base is `lsmfttb/sts_lightspeed:spire/main` at `ab2b11bc3b5b6c6b68d9d855bc9545e9aca62a28`. The read-only legacy source is `lsmfttb/STSRL:main` at `3037b75eca4bd73fa70d018ffd4442a1f2d65628`.

## Compatibility inventory

| Surface | Legacy STSRL expectation | Native `spire/main` observation | Result |
| --- | --- | --- | --- |
| Whole-run loop | `execute_controlled_run` owns reset, legal actions, public context, controller call, step, and history. | The loop shape remains reusable, but it obtains and validates a public projection before calling the controller. | Reuse is conditional on a small adapter/context boundary; the old adapter cannot be copied unchanged. |
| Projection schema | `LightSpeedAdapter.public_projection()` parses `native-public-projection-v1`, including `external_base_commit`, `patch_identity`, and candidate `bits`. | `StepSimulator.public_projection()` emits v2, no legacy provenance keys, and intentionally omits opaque `bits`. The unmodified legacy adapter raises `ValueError: unsupported native public projection schema 'native-public-projection-v2'` on seed 49 before the controller call. | Concrete adapter compatibility blocker; the mismatch fails closed. Do not restore bits to public candidates. |
| Action identity and execution | The adapter creates `action_id` as `scope:bits`, with the native handle and bits in `SimulatorAction.raw`. | Native public candidates contain sanitized `scope`, `kind`, indices, and labels. The bounded probe checked their count, order, and identity against `legalActions()` at each visited screen and battle root, and checked that no public candidate contains `bits`. `step_public_action` only accepts battle actions; noncombat execution must keep opaque handles inside a trusted adapter and use the public selected index. | Public identity/order parity passed on the visited screens. Any shim must separate policy-visible candidate identity from trusted execution handles. |
| Controller inputs | `execute_controlled_run` passes adapter, raw snapshot, actions, and context to the controller. `build_decision_context` derives features from the raw snapshot. | `PolicyController` explicitly ignores the raw trio, but the controller protocol permits access to it. The Expert driver's forbidden-key-name check is not a value-level information firewall. | Existing tests do not establish sanitized inputs for a new whole-run composition. A future wrapper must give the policy only a sanitized public observation, ordered public legal actions, and public history; action handles stay executor-side. |
| Noncombat information | `ExpertNonCombatDriver` is seeded and uses visible-context heuristics, with fallback weights for missing payloads. | In the seed-49 diagnostic, `EVENT_SCREEN`, `REWARDS`, and `MAP_SCREEN` have `screen_payload=unsupported`; visible map graph, current node, and legal routes are unavailable. Event and reward candidates have generic indexed labels, not their human-visible contents. | A seeded fallback can step legally but cannot establish honest public decisions about those contents. Stop before a policy decision at this boundary. Shop/rest screens were not reached by this bounded probe. |
| Battle hidden-state boundary | The Battle controller should consume only public inputs; hidden futures may be used internally by shared-public search. | At a supported seed-49 battle root, two deterministically sampled futures had different draw-pile orders but equal `publicBattleState()` and `publicProjection()`. | This small invariance check passed; it is not a full controller-input sanitation proof or an outcome result. |

## Reproducible evidence

The compact native smoke is `apps/test_issue19_public_boundary.cpp`; the captured output is [`issue19_public_boundary_smoke.json`](issue19_public_boundary_smoke.json). It starts an A20 Ironclad seed-49 simulator and advances the first legal native action at each screen solely as a bounded mechanics probe. It reached Battle in four steps, recorded the missing Event/Rewards/Map fields, verified sanitized candidate order/identity parity, and found a distinct hidden draw order at particle index 1 while both public views remained equal. It makes no policy or outcome claim.

The Release native extension and smoke target built with GNU C++ 15.2.0 / Python 3.14.4; `test-issue19-public-boundary` printed `ISSUE19_PUBLIC_BOUNDARY_PASS`. The exact legacy adapter probe against that extension reproduced the v2 schema `ValueError` above. Focused legacy regressions passed on Windows Python 3.12.10 / pytest 9.1.1:

```text
python -m pytest -p no:cacheprovider \
  tests/test_lightspeed_adapter.py \
  tests/test_native_public_projection.py \
  tests/test_public_run_context.py \
  tests/test_online_controller.py \
  tests/test_non_combat_driver.py
121 passed
```

No Stage 2 full run, B=192 search pilot, or outcome comparison was launched: Stage 1 already fails at the legacy v1/v2 parser boundary, and the native public surface does not expose the encountered Event, reward, and map decision contents. No new seeds were used.

## Smallest follow-up capability

The native public projection needs task-independent, explicitly public payloads for the visible choice contents needed by Event and Rewards screens, plus the visible map graph/current node/legal routes. Those payloads should identify choices without exposing simulator-private state or opaque action bits. Keep selected native action handles exclusively in the trusted executor. Re-run the public-input invariance and fail-closed boundary checks before any whole-run policy pilot; consider Shop/Rest coverage separately because seed 49 did not reach those screens.

## Source pointers

- Legacy action wrapping: `src/sts_combat_rl/sim/lightspeed.py:104-123`; v1 parser and required fields: `src/sts_combat_rl/sim/native_public_projection.py:30-32,83-93,460-479,675-685`.
- Legacy loop/controller boundary: `src/sts_combat_rl/sim/controlled_run.py:253-280,422-455`; controller protocol: `src/sts_combat_rl/sim/controller_contract.py:243-260`; public `PolicyController` raw-argument discard: `src/sts_combat_rl/sim/online_controller.py:70-77,118-133`.
- Native sanitized candidates, v2 schema, unsupported fields, and Battle-only public action stepping: `bindings/slaythespire.cpp:843-861,1110-1174,1363-1385`.
