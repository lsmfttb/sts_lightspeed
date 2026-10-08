# Issue 21: native v3 public-policy firewall

This is a disposable study target based on native `spire/main` commit
`a655dbb264b2274c54acf0426cd5df19a8c8ec96`. It reuses the actual STSRL
controlled-run executor and seeded noncombat policy from
`3037b75eca4bd73fa70d018ffd4442a1f2d65628`; the Battle heuristic is copied
byte-for-byte and the bounded `searchDecision` / `selectSharedEdge` logic is
adapted from Issue 17 final study commit
`9a2792e1e02157124b4f90edc91b7ad8765d5d10`.

## Run

From the native repository root, prepare a clean pinned STSRL checkout and
build the study plus focused native regressions:

```sh
git clone https://github.com/lsmfttb/STSRL.git ../STSRL-issue21
git -C ../STSRL-issue21 checkout --detach 3037b75eca4bd73fa70d018ffd4442a1f2d65628
cmake -S . -B build-py -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build-py --target study-issue21-v3-firewall test-public-noncombat-projection test-public-battle-state-semantics -j2
./build-py/test-public-noncombat-projection
./build-py/test-public-battle-state-semantics
./build-py/study-issue21-v3-firewall studies/issue21/native-witness.json
python3 -m py_compile studies/issue21/run_v3_public_firewall.py studies/issue21/policy_worker.py
python3 studies/issue21/run_v3_public_firewall.py \
  --st-srl-root ../STSRL-issue21 \
  --build-dir build-py \
  --native-witness studies/issue21/native-witness.json \
  --output studies/issue21/result.json
```

The runner rejects a dirty or wrong-revision STSRL checkout. It writes the
reproducible result and native witness, plus `result.progress.json`, which is
atomically updated at each coarse execution boundary and records failure or
interruption state.

## Interpretation

The real A20 Ironclad runs for seeds 49 and 50 execute supported Event and
Rewards choices through `execute_controlled_run`, then stop before a strategic
Map action because native v3 does not expose the revealed Act boss identity.
The Battle root is reached only in a separately labeled four-transition
mechanics fixture; those setup actions are not policy decisions. On that
supported root, the Issue 17 public heuristic and a shared-public search with
192 simulations both map their public identity to the same ordered native
legal-action surface and are accepted by `stepPublicAction`.

The Battle policy subprocess used to test the executor firewall is a
public-input probe, not either comparison policy. No whole-run, win-rate, or
A20 strength conclusion is made. The bounded firewall result is
`PUBLIC_RUN_FIREWALL_ESTABLISHED`; code disposition remains `STUDY_ONLY`.
