# STSRL-006 native particle-to-Search bridge

STSRL-006 adds an additive native API that composes the accepted T096/T098
public-consistent hidden-future sampler with the existing `BattleScumSearcher2`
root evaluator.  The sampled `BattleContext` remains an ephemeral native value;
Python receives no particle checkpoint, raw RNG state, or reconstructed hidden
state.

The bridge API is:

```python
sim.sample_hidden_future_particles_search(
    sampler_seed, particle_start, particle_count,
    search_simulations, include_potions=False,
)
```

It factors the existing particle constructor into one helper shared by
`sample_hidden_future_particles()` and the bridge.  Each supported row contains
the sampler index/derived seed, audit-only hidden fingerprint, public
projection, ordered public action identities, sanitized Search-v2 root rows,
root evaluation telemetry, and explicit parity/mapping flags.  Raw native
action bits are not returned in bridge rows.

The bridge checks anchor and particle public-projection equality, ordered legal
action equality, and occurrence-safe root mapping.  Search-v2's existing
mechanical deduplication is unchanged: a public card occurrence either maps by
its exact action bits or, for an adjacent mechanically equivalent duplicate,
maps to the single Search edge for that equivalence class.  Each row and the
root report include the source edge, mapping mode, source action identity, and
the number of public occurrences sharing that edge.  Missing or multiply
ambiguous sources, incomplete Search roots, unsupported-fidelity anchors, and
particle parity drift fail closed before an accepted batch is returned.

The returned values have deliberately mixed semantics: the outer particle
distribution follows the normal public-information state, while each
continuation runs full-state Search-v2 inside its native particle.  Therefore
the rows are a `full_state_continuation_strategy_fusion_proxy`; they are not
`Q_public`, an executable no-SL continuation, information-set-optimal search,
or an exact posterior expectation.  The bridge does no cross-particle
aggregation or action selection.

The focused audit covers direct sampler parity, hidden-particle diversity,
duplicate-card occurrence mapping, Frozen Eye/direct Search compatibility,
known draw constraints, and unsupported-anchor fail-closed behavior.  It is a
capability check only and makes no particle-convergence claim.
