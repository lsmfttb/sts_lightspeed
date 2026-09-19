# STSRL-005 Visibility-Aware Public Information

The T096 public-information projection is versioned as
`native-battle-public-information-v2`.  It represents current player knowledge
as state, rather than inferring it from an action-history replay.

`BattleContext::knownDrawTopUniqueIds` and
`knownDrawPositionUniqueIds` are exact epistemic facts.  They are copied by the
ordinary `BattleContext` copy constructor (and therefore simulator
checkpoints), and cannot affect card order, RNG, game mechanics, or legal
actions.  Typed `knownDrawUnsupportedReasons` records unrepresented current
knowledge separately from those exact facts.  Headbutt and Warcry add a
deterministically public top-card fact; drawing that card consumes it.  Shuffles
clear exact-order constraints, but do not resolve an already-unsupported
information transition.

The public projection reports one of:

- `hidden` for ordinary draw order;
- `known_prefix` with an exact top-first prefix after a supported deterministic
  placement; or
- `full_public_exact` with the complete top-first order while Frozen Eye is
  held; or
- `unsupported_fidelity` when a draw transition or monster state is not
  modeled with sufficient public-information fidelity.

When sampling hidden-draw particles, the sampler shuffles only the private
remainder after a known prefix and performs no draw-order permutation under
Frozen Eye.  The particle API keeps its private fingerprint strictly outside
the public projection.

With Runic Dome, public monster rows omit the current move, attack category,
and the current move's damage/hit structure, while retaining the preceding
observed move and semantic public status values.  Raw native `miscInfo` and
packed `uniquePower0/1` storage are never part of the public schema.  Native
monster storage is classified as private-only, public-semantic, or
mixed/unseparated; private-only random parameters remain hidden without
degrading the otherwise complete public state, public counters use semantic
names, and only mixed/unseparated state is marked `unsupported_fidelity`.

Known draw facts use a common position constraint: Headbutt, Warcry, and
Rebound establish a top prefix, while Forethought establishes a known bottom
position.  Particles shuffle only unconstrained positions.  Subset-reveal and
other unmodeled draw transitions are explicitly marked `unsupported_fidelity`;
the particle sampler rejects such anchors instead of returning rows with a
false supported label.  Exact-order clearing and unsupported-state resolution
are separate operations, so a later shuffle cannot silently make subset
knowledge ordinary again.
The preceding observed move remains public because monster move history is
part of the player's current knowledge and can affect future move selection.
No mechanics or legal action enumeration changes; the omission is solely a
normal-information projection boundary.

The bounded native smoke remains:

```bash
python scripts/test_t096_public_information_sampler.py
```

The deterministic high-risk transition audit is:

```bash
python scripts/test_t096_visibility_transitions.py
```

The full STSRL acceptance boundary remains external: it must pin the exact
merged source commit and run its clean source verifier before using this
capability for a new scientific task.
