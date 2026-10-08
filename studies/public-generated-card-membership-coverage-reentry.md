# Public generated-card membership coverage re-entry

This is a coverage-only replay for Issue #12, using the Issue #10 adaptive shared-public-tree runner (`c28ca05c3a6180c465a812eafd226493ab5a0528`) narrowed to the retained Small Slimes case. The narrowed runner source blob was `aa39f3949da3e7a09884c1fcdcfff8af3280f711`; simulator implementation was `ae2efb619cfb4b3648fe84b8f0d27c609dc63e6b`. Cohort: Ironclad A0, first battle (Small Slimes), game seed 3, controller replicate seed 17, 32 particles, same fixed first-legal-action setup, with no fallback after unsupported decisions.

| Budget / decision | Supported / attempted decisions | Battle result | First unsupported category |
| ---: | ---: | --- | --- |
| 96 | 42 / 42 | Completed, win | None |
| 384 | 29 / 29 | Completed, win | None |

The retained Issue #11 replay stopped this case at decision 18 (`18 / 19`) and decision 21 (`21 / 22`) with `insertion_membership_unrepresented`. That category did not recur in either Issue #12 attempt; both now completed. This is evidence that the blocker is removed for this fixed case, not a broad sampler-coverage or battle-strength claim.

Focused native verification: Release `test-public-battle-state-semantics` printed `PUBLIC_BATTLE_STATE_SEMANTICS_PASS`. The test target covers the public pile lifecycle, duplicate-face semantics, hidden random insertion, sampler membership preservation/noSL anchor invariance, and retained public-state fidelity regressions.

Limitations: one deterministic first-battle case, two search budgets, fixed setup policy, and no broad outcome matrix. The replay is coverage evidence only.