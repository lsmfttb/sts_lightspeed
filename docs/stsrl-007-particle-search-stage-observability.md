# STSRL-007 particle/Search bridge stage observability

The STSRL-006 bridge now retains a safe structured trace for its most recent
`sample_hidden_future_particles_search()` attempt. Call
`last_particle_search_stage_diagnostics()` after either a successful call or a
caught bridge exception to read it. The original bridge return value and
exception behavior are preserved.

The trace schema is `native-particle-search-stage-observability-v1`. Its
top-level fields are `attempt_status`, `first_failed_stage`, `failure_code`,
`accepted_root_report_returned`, and `particles`. Each particle row contains
`particle_index` (null only for an anchor preflight failure), `stages`,
`first_failed_stage`, and `failure_code`. Each of the six stage values is one
of `not_reached`, `entered`, `completed`, or `failed`:

1. `hidden_future_sample_construction`
2. `public_fidelity_validation`
3. `root_occurrence_mapping`
4. `search_setup`
5. `search_execution`
6. `sanitized_root_report`

Statuses are set at the native control-flow boundaries. In the current bridge,
root occurrence mapping is validated after Search execution because Search-v2
materializes its root edge surface during execution. This instrumentation does
not move that check or change its rules. A failure is recorded at the active
boundary, later untouched stages remain `not_reached`, and the original native
exception is rethrown. Stable coarse failure codes do not include exception
text.

The snapshot contains no exception payload, `BattleContext`, hidden card order,
RNG data, raw action bits, Search tree state, or continuation trajectory. It is
diagnostic control-flow evidence only; it does not establish a failure's game
mechanics root cause.

`stsr007_particle_search_stage_audit()` is a deterministic test-only audit. Its
private failure injection is reachable only from that audit method and is not
an argument to the production bridge API. The audit covers success, unsupported
anchor fidelity, occurrence mapping, Search setup, Search execution, and
sanitized report boundaries.
