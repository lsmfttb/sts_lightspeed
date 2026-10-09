# RETIRED EXPERIMENTAL INTEGRATION BRANCH — spire/main

This branch is NOT the authoritative native integration for A20 Heart noSL research.

- **Use existing:** `lsmfttb/STSRL:main` with the `lsmfttb/sts_lightspeed:stsrl/main` exact commit pinned by STSRL's `docs/sts_lightspeed_source_manifest.json`.
- The `spire/main` branch experimented with a parallel `native-public-projection-v3` interface. The reviewed historical SHA `d1dcd6534ec4a1f38ac1f7f916f01a3f931fcfd0` is frozen at `archive/spire-v3-2026-10-09`; do not use it as a new native implementation base.
- The v3 implementation and only its tests/build targets were retired from this branch. Do not reintroduce v3, extend its Shop/Treasure/Rest/Card Select coverage, or create a bridge merely to satisfy that schema.
- A generic interface change must begin from a **specific STSRL scientific requirement** and an independently verified defect in the *existing pinned* public policy/adapter interface. Demonstrate the exact missing human-visible input or hidden-state leak and why a smaller adapter-only repair cannot suffice. Obtain explicit Planner authorization before native implementation.
- Do not change the upstream game mechanics, STSRL working runtime or independent native `stsrl/main` through this retired branch.
- Prior `spire/main` Issue studies and their exact SHAs remain historical evidence only. A passing focused reviewer test does not authorize further v3 coverage work.

See `lsmfttb/spire-research/AGENTS.md` and its standing research ledger Issue #1 for current authority.
