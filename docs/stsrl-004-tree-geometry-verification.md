# STSRL-004 Battle Search v2 Tree Geometry Verification

Issue: [#8](https://github.com/lsmfttb/sts_lightspeed/issues/8)

The tree-geometry capability was introduced by commit
`5e95b43468140f7f0a5207e5262b4ae82ad46313` on `stsrl/main`.
This follow-up adds exact parity coverage for the T070 callback combination:
policy priors and learned leaf values enabled together.

## Clean-checkout verification

Run from a clean checkout of the commit under review:

```bash
git submodule update --init --recursive
cmake -S . -B build-py -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build-py --target slaythespire -j 16
python scripts/stsrl_api_smoke.py --build-dir build-py
python scripts/test_battle_search_v2_tree_geometry.py --build-dir build-py
```

The native build, API smoke check, and tree-geometry integration script passed
for the capability implementation. The integration script verifies that a
one-simulation search retains one expansion, all geometry invariants hold, and
the companion API is deterministic from a restored checkpoint.

The combined T070 parity arm uses the same checkpoint and a budget of 16 with
both callbacks enabled. It requires the normal and companion reports to match
after `tree_geometry` is removed, and compares callback counts, native step
counts, and root rows directly.

## Review boundary

The complementary reviewer conclusion and Issue closure belong to the pull
request that contains this follow-up. No simulator state or callback payload is
recorded by this document; geometry output remains aggregate integer structure.
