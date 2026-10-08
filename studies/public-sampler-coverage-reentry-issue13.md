# Normal-public complete-battle coverage re-entry (Issue #13)

**Classification: coverage milestone.** Simulator base: `ab2b11bc3b5b6c6b68d9d855bc9545e9aca62a28`. Study-only runner: Issue #10 source commit `c28ca05c3a6180c465a812eafd226493ab5a0528`. The runner and its temporary CMake target were not added to the submitted tree. The full per-attempt record is in `public-sampler-coverage-reentry-issue13.csv`.

Cohort: Ironclad A0; first native battle after fixed first-legal-action setup; game seeds 1–8 × controller replicate seeds 17, 101, and 1009; 32 particles; 24 matched seed/replicate pairs per budget. Each budget is the total shared-tree simulation count **per actual public decision** (B=96 or B=384), not per particle. All 48 attempts reached native battle start.

| Budget | Fully supported completions | Sampler-supported decisions | First unsupported categories | Median battle seconds | Native public-action steps |
| ---: | ---: | ---: | --- | ---: | ---: |
| 96 | 24/24 | 432/432 | None | 5.81 | 409850 |
| 384 | 24/24 | 388/388 | None | 21.05 | 1338960 |

Issue #10 completed 2/24 attempts at each budget; its first unsupported categories were `monster_status_timing` (18) and `player_status_timing` (4) per budget. This re-entry completed 24/24 at both budgets. Neither status-timing category nor `insertion_membership_unrepresented` recurred, and no new unsupported category was observed. Thus the previous blockers are lifted for this fixed cohort.

All 48 completed battles were wins. Treat those outcomes as exploratory cohort observations only; they do not estimate policy strength or establish a policy benefit. The evidence supports proceeding to design a separate outcome-strength evaluation, subject to independent review.

Limitations: this is one deterministic Ironclad A0 first-battle cohort with a fixed route-to-battle setup, 32 particles, and a reproducible proposal sampler rather than an exact posterior. Unsupported states still stop without fallback. Search rollouts truncate at 24 public decisions and use the runner's public-state heuristic. Timings are local single-process measurements; they exclude compilation.
