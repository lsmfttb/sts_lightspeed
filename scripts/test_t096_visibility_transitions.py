"""Deterministic native evidence for the STSRL-005 visibility boundary."""

from __future__ import annotations

import argparse

import slaythespire


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", default="build-py")
    args = parser.parse_args()
    sim = slaythespire.StepSimulator(
        slaythespire.CharacterClass.IRONCLAD, 123456789, 0
    )
    report = sim.t096_visibility_audit()
    required = (
        "headbutt_known_prefix",
        "native_snapshot_contract",
        "checkpoint_preserves_known_prefix",
        "sampler_preserves_known_prefix",
        "sampler_public_information_invariant",
        "sampler_private_remainder_diverse",
        "havoc_consumes_top_preserves_suffix",
        "rebound_establishes_known_top",
        "forethought_known_position_preserved",
        "subset_reveal_fails_closed",
        "subset_reveal_shuffle_stays_unsupported",
        "subset_reveal_sampler_fails_closed",
        "draw_knowledge_reason_typed",
        "frozen_eye_full_order",
        "frozen_eye_sampler_preserves_order",
        "runic_dome_hides_current_intent",
        "runic_dome_preserves_previous_move",
        "runic_dome_sanitizes_roll_misc",
        "runic_dome_hidden_counter_timing_invariant",
        "runic_dome_mixed_counter_sampler_fails_closed",
        "runic_dome_hides_louse_misc",
        "private_hidden_state_projection_invariant",
        "runic_dome_looter_public_counter_preserved",
        "private_hidden_misc_sampler_supported",
        "runic_dome_retains_visible_power",
        "runic_dome_direct_misc_fail_closed",
    )
    if report.get("schema_id") != "native-battle-visibility-audit-v1":
        raise AssertionError("unexpected visibility audit schema")
    failed = [name for name in required if report.get(name) is not True]
    if failed:
        raise AssertionError(f"visibility audit failed: {', '.join(failed)}")
    print("T096_VISIBILITY_TRANSITIONS_PASS")
    for name in required:
        print(f"{name}=true")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
