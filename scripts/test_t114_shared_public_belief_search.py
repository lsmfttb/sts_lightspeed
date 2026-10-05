from __future__ import annotations

import argparse
import importlib
import importlib.machinery
import pathlib
import sys
from typing import Any


def load_module(build_dir: pathlib.Path) -> Any:
    suffixes = tuple(importlib.machinery.EXTENSION_SUFFIXES)
    candidates = sorted(
        path
        for path in build_dir.rglob("slaythespire*")
        if path.is_file() and path.name.endswith(suffixes)
    )
    if not candidates:
        raise AssertionError(f"no slaythespire extension found under {build_dir}")
    sys.path.insert(0, str(candidates[0].parent))
    return importlib.import_module("slaythespire")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=pathlib.Path, default=pathlib.Path("build-T114"))
    args = parser.parse_args()
    sts = load_module(args.build_dir)

    simulator = sts.StepSimulator(sts.CharacterClass.IRONCLAD, 114, 20)
    audit = simulator.t114_shared_public_belief_search_audit()
    required = (
        "synthetic_only",
        "exact_T096_root_projection_parity",
        "hidden_particle_diversity_private_only",
        "same_public_projection_shares_node_identity",
        "prepared_pool_prefixes_2_4_8_16_are_nested",
        "fixed_seed_schedule_is_deterministic_and_state_independent",
        "fixed_seed_schedule_uses_unbiased_rejection_sampling",
        "missing_public_action_mapping_fails_closed",
        "ambiguous_public_action_mapping_fails_closed",
        "incompatible_public_action_surface_fails_closed",
        "changed_exact_public_projection_is_not_aliased",
        "native_missing_action_mapping_fails_closed",
        "native_ambiguous_action_mapping_fails_closed",
        "root_precheck_failure_stage_and_cause_are_allowlisted",
        "inconsistent_exact_fact_failure_is_classified",
        "missing_action_mapping_failure_is_classified",
        "ambiguous_action_mapping_failure_is_classified",
        "incompatible_action_surface_failure_is_classified",
        "invalid_action_execution_failure_is_classified",
        "shared_edge_global_exploration_and_backup",
        "multiple_particles_contribute_to_one_shared_root",
        "same_seed_reconstructs_same_private_pool",
        "wild_strike_random_insertion_transition_reached",
        "wild_strike_node_surface_reports_unsupported_fidelity",
        "wild_strike_child_surface_reports_unsupported_fidelity",
        "failure_diagnostic_contains_only_allowlisted_fields",
        "failure_diagnostic_does_not_expose_private_state_rng_or_exception_text",
        "arbitrary_native_exception_text_is_not_exported",
        "report_build_failure_stage_is_allowlisted",
        "same_inputs_reproduce_aggregate_report",
        "aggregate_shared_edge_count_covers_all_nodes",
        "every_searched_occurrence_maps_to_its_shared_class",
        "equivalent_duplicate_occurrences_share_one_class",
        "excluded_occurrences_have_null_unvalued_edge_rows",
        "potions_remain_auditable_but_excluded_and_unvalued",
        "output_uses_public_action_identity_only",
        "no_per_particle_controller_output",
    )
    if audit.get("schema_id") != "native-t114-shared-public-belief-search-audit-v1":
        raise AssertionError("unexpected T114 shared public belief search audit schema")
    false_fields = [name for name in required if audit.get(name) is not True]
    if false_fields:
        raise AssertionError(f"T114 synthetic audit failed: {', '.join(false_fields)}")
    if audit.get("prepared_particle_count") != 16:
        raise AssertionError("T114 audit did not prepare the exact N=16 private root pool")
    if audit.get("real_scientific_state_invoked") is not False:
        raise AssertionError("T114 audit unexpectedly invoked a real scientific state")
    diagnostic = simulator.t114_last_search_failure_diagnostic()
    if diagnostic.get("status") != "NO_FAILURE_RECORDED":
        raise AssertionError("T114 synthetic audit leaked its internal test diagnostic")

    print("T114_SHARED_PUBLIC_BELIEF_SEARCH_AUDIT_PASS")
    for name in required:
        print(f"{name}={audit[name]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
