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
        "checkpoint_preserves_known_prefix",
        "sampler_preserves_known_prefix",
        "sampler_private_remainder_diverse",
        "havoc_consumes_top_preserves_suffix",
        "rebound_establishes_known_top",
        "frozen_eye_full_order",
        "frozen_eye_sampler_preserves_order",
        "runic_dome_hides_current_intent",
        "runic_dome_preserves_previous_move",
        "runic_dome_sanitizes_roll_misc",
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
