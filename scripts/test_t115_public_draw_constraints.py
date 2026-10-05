"""Synthetic native acceptance checks for T115 public draw-pile constraints."""

from __future__ import annotations

import slaythespire


def main() -> int:
    simulator = slaythespire.StepSimulator(
        slaythespire.CharacterClass.IRONCLAD, 123456789, 0
    )
    report = simulator.t115_draw_insertion_audit()
    required = (
        "ordinary_insertion_supported_with_exact_multiset",
        "hidden_realizations_share_public_identity",
        "default_context_potion_slots_are_empty",
        "no_private_identity_or_insertion_outcome_leak",
        "repeated_insertions_remain_jointly_modeled",
        "representative_multi_card_action_supported",
        "unknown_generated_identity_fails_closed_narrowly",
        "same_face_insertion_role_ambiguity_fails_closed",
        "sampled_particles_preserve_public_constraints",
        "empty_pile_insertion_is_exact_top",
        "known_bottom_composes_as_relative_bound",
        "sampler_preserves_known_bottom_bound",
        "known_top_composes_with_insertion",
        "sampler_preserves_known_top",
        "top_draw_consumes_insertion_constraints",
        "unknown_identity_empty_pile_private_until_draw",
        "frozen_eye_remains_full_order_exact",
    )
    if report.get("schema_id") != "native-t115-draw-insertion-audit-v1":
        raise AssertionError("unexpected native T115 audit schema")
    if report.get("synthetic_only") is not True:
        raise AssertionError("T115 native audit must stay synthetic")
    failed = [name for name in required if report.get(name) is not True]
    if failed:
        raise AssertionError(f"T115 native draw constraints failed: {', '.join(failed)}")
    print("T115_NATIVE_DRAW_CONSTRAINTS_PASS")
    for name in required:
        print(f"{name}=true")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
