#!/usr/bin/env python3
"""Small native T092 API/firewall smoke; not a registered cohort execution."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys


def _to_battle(module):
    sim = module.StepSimulator(module.CharacterClass.IRONCLAD, 1, 20)
    for _ in range(200):
        snapshot = sim.snapshot()
        if snapshot.get("screen_state") == "BATTLE" and snapshot.get("battle_active"):
            return sim
        actions = sim.legal_actions()
        if not actions:
            raise RuntimeError("could not reach a battle")
        sim.step(actions[0])
    raise RuntimeError("could not reach a battle")


def _contains_forbidden(value):
    forbidden = ("checkpoint", "rng", "draw_order", "action_queue", "private")
    if isinstance(value, dict):
        return any(any(token in str(key).lower() for token in forbidden) or _contains_forbidden(child) for key, child in value.items())
    if isinstance(value, list):
        return any(_contains_forbidden(child) for child in value)
    return False


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", required=True)
    args = parser.parse_args()
    sys.path.insert(0, str(Path(args.build_dir)))
    import slaythespire

    off = _to_battle(slaythespire).battle_search_v2(8, False, None, None)
    on = _to_battle(slaythespire).battle_search_v2_with_internal_teacher_telemetry(8, False)
    geometry_only = _to_battle(slaythespire).battle_search_v2_with_tree_geometry(8, False, None, None)
    for field in ("root_rows", "root_visits", "native_simulator_steps", "best_action_value", "min_action_value", "outcome_player_hp"):
        assert off[field] == on[field], field
    assert on["native_api"] == "StepSimulator.battle_search_v2_with_internal_teacher_telemetry.v1"
    assert on["patch_identity"] == "sts_lightspeed_battle_search_v2_internal_teacher_telemetry_v1"
    telemetry = on["tree_internal_telemetry"]["internal_teacher_telemetry"]
    geometry = on["tree_internal_telemetry"]["tree_geometry"]
    assert geometry == geometry_only["tree_internal_telemetry"]["tree_geometry"]
    assert geometry["schema_id"] == "native-battle-search-v2-tree-geometry-v1"
    assert geometry["total_expanded_node_count"] == on["tree_internal_telemetry"]["expanded_nodes"]
    assert sum(row["expanded_node_count"] for row in geometry["depth_rows"]) == geometry["total_expanded_node_count"]
    assert sum(row["discovered_child_edge_count"] for row in geometry["depth_rows"]) == geometry["total_discovered_child_edge_count"]
    assert sum(row["visited_child_edge_count"] for row in geometry["depth_rows"]) == geometry["total_visited_child_edge_count"]
    teacher = on["teacher_config"]
    assert teacher["schema_id"] == "t092-frozen-search-v2-teacher-config-v1"
    assert teacher["simulations"] == 8
    assert teacher["include_potions"] is False
    assert teacher["policy_prior"] is None
    assert teacher["learned_leaf_value"] is None
    assert telemetry["search_rng_or_counter_mutated"] is False
    assert telemetry["raw_private_state_exported"] is False
    assert telemetry["candidate_count"] == len(telemetry["rows"])
    assert not _contains_forbidden(telemetry["rows"])
    for row in telemetry["rows"]:
        assert row["tree_depth"] >= 1
        assert row["input_state"] in {"PLAYER_NORMAL", "CARD_SELECT"}
        for child in row["teacher_searchable_actions"]:
            assert child["visits"] > 0 or child["mean_value"] is None
    try:
        _to_battle(slaythespire).battle_search_v2_with_internal_teacher_telemetry(1, True)
    except ValueError as error:
        assert "no-potion" in str(error)
    else:
        raise AssertionError("T092 telemetry must reject potion-enabled Search")
    print("native T092 internal teacher telemetry assertions passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
