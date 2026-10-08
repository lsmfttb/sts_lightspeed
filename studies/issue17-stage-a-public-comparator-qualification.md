# Issue 17 — Stage A source-bound requalification

**Disposition:** `STUDY_ONLY`. Stage B remains gated on independent Stage A review.

## Source binding

- Native base: `lsmfttb/sts_lightspeed:spire/main`, `ab2b11bc3b5b6c6b68d9d855bc9545e9aca62a28`.
- Issue 16 runner provenance: handoff `239d4d21e3a3a306590cde2046ef8b020ba5b179`, frozen runner `41e71c1aba59caeb469c63ffd481a2236e52f353`.
- The Stage B runner is `apps/study_issue17_prospective_public_comparison.cpp`. It and the Stage A qualifier include the same selector and score implementation from `apps/study_issue17_public_tactical_heuristic.hpp` (blob `387a1d3252a4ffae01e796fc4ec1ee796328cef4`). The canonical `id_label == GREMLIN_NOB` check, estimate helpers, branch trace, and score output therefore come from one source.

## Cohort boundary

The Stage B runner declares Ironclad A20 seeds 49–72, two replicate seeds, and B=192/B=384. It must not be run until the Stage A freeze is reviewed and this Issue is routed back to Builder. The Stage A qualifier uses only previously exposed development seeds 1–48. No new-cohort battle outcomes have been loaded or executed.

Focused build and requalification evidence will be recorded after verification.
