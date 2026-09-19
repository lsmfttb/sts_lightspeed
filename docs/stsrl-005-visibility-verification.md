# STSRL-005 Visibility-Aware Public Information

The T096 public-information projection is versioned as
`native-battle-public-information-v2`.  It represents current player knowledge
as state, rather than inferring it from an action-history replay.

`BattleContext::knownDrawTopUniqueIds` is epistemic metadata.  It is copied by
the ordinary `BattleContext` copy constructor (and therefore simulator
checkpoints), and cannot affect card order, RNG, game mechanics, or legal
actions.  Headbutt and Warcry add a deterministically public top-card fact;
drawing that card consumes it.  Shuffles and an inconsistent native pile clear
or suppress the exact-order claim conservatively.

The public projection reports one of:

- `hidden` for ordinary draw order;
- `known_prefix` with an exact top-first prefix after a supported deterministic
  placement; or
- `full_public_exact` with the complete top-first order while Frozen Eye is
  held.

When sampling hidden-draw particles, the sampler shuffles only the private
remainder after a known prefix and performs no draw-order permutation under
Frozen Eye.  The particle API keeps its private fingerprint strictly outside
the public projection.

With Runic Dome, public monster rows omit the current move, attack category,
move IDs, and current damage/hit structure.  No mechanics or legal action
enumeration changes; the omission is solely a normal-information projection
boundary.

The bounded native smoke remains:

```bash
python scripts/test_t096_public_information_sampler.py
```

The full STSRL acceptance boundary remains external: it must pin the exact
merged source commit and run its clean source verifier before using this
capability for a new scientific task.
