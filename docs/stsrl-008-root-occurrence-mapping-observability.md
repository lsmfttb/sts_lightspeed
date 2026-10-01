# STSRL-008 root-occurrence mapping observability

STSRL-008 introduced an additive
`root_occurrence_mapping_diagnostic` value in each particle row. It is `null`
until that particle enters the existing `root_occurrence_mapping` stage.
Otherwise it is a dictionary with schema
`native-root-occurrence-mapping-diagnostic-v1`, tied to the same most recent
bridge attempt as the surrounding T105 stage trace.

STSRL-009 supersedes this diagnostic with
`native-root-occurrence-mapping-diagnostic-v2`. The successor preserves the v1
failure vocabulary and adds classified, searched, and configuration-excluded
public occurrence counts. See
`docs/stsrl-009-configuration-aware-root-mapping.md` for the versioned partial
mapping semantics; v1's all-public-occurrences-have-edges meaning is not
silently reinterpreted.

The closed `mapping_subreason` vocabulary is written directly at the existing
production mapping branches:

- `no_public_legal_action_surface`
- `multiple_direct_search_root_matches`
- `missing_non_card_direct_search_root_match`
- `card_not_adjacent_mechanical_duplicate`
- `representative_search_root_match_zero`
- `representative_search_root_match_multiple`
- `uncovered_search_root_edge`
- `mapping_completed`

The diagnostic `status` is `entered`, `failed`, or `completed`. A failure code
is recorded before the unchanged native exception is thrown. Successful
mapping reports `mapping_completed`; this is a success outcome marker, not a
failure subreason, and no `failure_subreason` field is emitted. The surrounding
stage remains authoritative: failure markers coincide with
`root_occurrence_mapping=failed`, while `mapping_completed` coincides with
`root_occurrence_mapping=completed`.

Only bounded structural metadata is exposed: public occurrence and Search-root
edge counts, completed mapping/coverage counts, aggregate direct/duplicate
mapping counts, and, for occurrence-specific failures, the public occurrence
index, public action kind, and zero/one/multiple match category. The diagnostic
does not contain raw action bits, exception text, hidden `BattleContext`, raw
card equivalence fields such as `specialData`, RNG data, private intent, Search
tree nodes, or continuation trajectories.

This change does not alter the accepted order in which Search execution
materializes root edges before occurrence mapping. It does not add a fallback,
retry, repair, or alternate mapping rule. The deterministic
`stsr008_root_occurrence_mapping_audit()` uses private, non-pybind-parameterized
test injections to drive the pre-existing branches; production bridge callers
cannot select those injections.
