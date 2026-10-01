# STSRL-009 configuration-aware root mapping

`sample_hidden_future_particles_search()` now returns the explicit successor
contract `native-battle-public-particle-search-v2`. Its per-particle root report
uses `native-battle-search-root-v2`, and its occurrence mapping uses
`native-search-root-occurrence-equivalence-v2`.

Every public legal-action occurrence retains its accepted order and has exactly
one `mapping_classification`:

- `searched_direct` has one direct Search root edge;
- `searched_mechanical_duplicate_card_occurrence` shares the existing Search
  edge of its adjacent mechanically equivalent card representative;
- `search_configuration_excluded` is a legal public action that the active
  production Search configuration proves is outside the configured Search
  domain.

The only production exclusion rule in this version is a potion use or discard
under `PLAYER_NORMAL` when `include_potions` is false. Such a row has
`configuration_exclusion_reason=include_potions_false`, no Search edge or
source action, zero visits, and null evaluation/mean fields. It is not assigned
a numeric zero value and does not participate as a Search result.

`root_action_mapping_complete=true` in v2 means every public occurrence is
classified as searched or configuration-excluded and every actual Search root
edge is covered. The full definition is also returned in
`root_action_mapping_completion_semantics`; the count of excluded rows is
returned as `configuration_excluded_public_action_count`.

The stage diagnostic is versioned as
`native-root-occurrence-mapping-diagnostic-v2`. In addition to the v1 fields,
it reports classified, searched, and configuration-excluded public occurrence
counts. Existing ambiguity and missing-required-edge branches remain
fail-closed, including End Turn, enabled-potion mismatches, duplicate-card
representative failures, and uncovered Search edges.

This contract change does not enable potion Search, add Search edges, change
Search visits or values, alter the sampler, or change public-fidelity rules.
The deterministic `stsr009_configuration_aware_root_mapping_audit()` verifies
the two potion cases, ordinary direct/duplicate mappings, strict failure
controls, complete Search-edge coverage, and same-head Search surface/work/RNG
state before and after mapping/report construction.
