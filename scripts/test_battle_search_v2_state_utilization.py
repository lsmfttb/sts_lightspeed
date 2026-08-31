#!/usr/bin/env python3
"""Smoke and fail-closed checks for the T079 read-only companion API."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", required=True)
    args = parser.parse_args()
    source = (
        Path(__file__).resolve().parents[1]
        / "src/sim/search/BattleScumSearcher2.cpp"
    )
    source_text = source.read_text(encoding="utf-8")
    if 'out << "battle-context-v1|" << state' in source_text:
        raise RuntimeError(
            "canonicalBattleState must not include BattleContext::operator<<"
        )
    sys.path.insert(0, args.build_dir)
    import slaythespire

    sim = slaythespire.StepSimulator(slaythespire.CharacterClass.IRONCLAD, 1, 20)
    for _ in range(200):
        snapshot = sim.snapshot()
        if snapshot.get("screen_state") == "BATTLE" and snapshot.get("battle_active"):
            break
        actions = sim.legal_actions()
        if not actions:
            raise RuntimeError("could not reach a battle")
        sim.step(actions[0])
    else:
        raise RuntimeError("could not reach a battle")

    def policy(_snapshot, actions):
        return [1.0] * len(actions)

    def value(_snapshot, _actions):
        return 0.5

    result = sim.battle_search_v2_with_state_utilization(3, False, policy, value)
    assert result["native_api"] == (
        "StepSimulator.battle_search_v2_with_state_utilization.v1"
    )
    assert result["patch_identity"] == (
        "sts_lightspeed_battle_search_v2_state_utilization_v1"
    )
    telemetry = result["tree_internal_telemetry"]
    state = telemetry["state_utilization"]
    assert state["identity_complete"] is True
    assert state["identity_semantics"].startswith("all future-dynamics BattleContext values")
    assert "BattleContext.curCardQueueItem.all_fields" in state["identity_components"]
    assert "ActionQueue.indices_size_and_clear_bits" in state["identity_components"]
    assert state["digest_collision_count"] == 0
    assert state["collision_check"] == (
        "canonical_payload_equality_within_digest_bucket"
    )
    assert state["expanded_path_node_count"] == len(state["expanded_states"])
    assert telemetry["expanded_nodes"] == state["expanded_path_node_count"]
    assert telemetry["policy_prior_calls"] == 3
    assert telemetry["leaf_value_calls"] == 3
    assert telemetry["tree_geometry"]["total_expanded_node_count"] == 3
    print("native T079 state-utilization assertions passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
