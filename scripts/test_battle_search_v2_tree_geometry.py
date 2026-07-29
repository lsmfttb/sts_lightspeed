#!/usr/bin/env python3
"""Deterministic integration checks for Battle Search v2 tree geometry."""

from __future__ import annotations

import argparse
import copy
import importlib
import importlib.machinery
import pathlib
import sys
from collections.abc import Mapping, Sequence
from typing import Any


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=pathlib.Path, default=pathlib.Path("build-py"))
    parser.add_argument("--seed", type=int, default=123456789)
    return parser.parse_args()


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


def advance_to_battle(sim: Any) -> None:
    for _ in range(128):
        if sim.snapshot()["screen_state"] == "BATTLE":
            return
        actions = sim.legal_actions()
        if not actions:
            raise AssertionError("no legal action while advancing to battle")
        sim.step(actions[0])
    raise AssertionError("failed to reach a battle in 128 legal actions")


def without_geometry(report: Mapping[str, Any]) -> dict[str, Any]:
    copied = copy.deepcopy(report)
    copied["tree_internal_telemetry"].pop("tree_geometry", None)
    return copied


def check_geometry(report: Mapping[str, Any]) -> None:
    telemetry = report["tree_internal_telemetry"]
    geometry = telemetry["tree_geometry"]
    assert set(geometry) == {
        "schema_id",
        "schema_version",
        "root_depth",
        "total_expanded_node_count",
        "total_discovered_child_edge_count",
        "total_visited_child_edge_count",
        "max_expanded_depth",
        "depth_rows",
    }
    assert geometry["schema_id"] == "native-battle-search-v2-tree-geometry-v1"
    assert geometry["schema_version"] == 1
    assert geometry["root_depth"] == 0

    rows = geometry["depth_rows"]
    assert isinstance(rows, Sequence)
    assert sum(row["expanded_node_count"] for row in rows) == telemetry["expanded_nodes"]
    assert telemetry["expanded_nodes"] == geometry["total_expanded_node_count"]
    assert sum(row["discovered_child_edge_count"] for row in rows) == geometry["total_discovered_child_edge_count"]
    assert sum(row["visited_child_edge_count"] for row in rows) == geometry["total_visited_child_edge_count"]

    for expected_depth, row in enumerate(rows):
        assert row["depth"] == expected_depth
        assert row["visited_child_edge_count"] <= row["discovered_child_edge_count"]
        histogram = row["branching_histogram"]
        assert histogram == sorted(histogram, key=lambda bucket: bucket["child_count"])
        assert sum(bucket["node_count"] for bucket in histogram) == row["expanded_node_count"]
        assert sum(bucket["child_count"] * bucket["node_count"] for bucket in histogram) == row["discovered_child_edge_count"]

    if geometry["max_expanded_depth"] == -1:
        assert rows == []
    else:
        assert geometry["max_expanded_depth"] == len(rows) - 1
        assert rows[0]["discovered_child_edge_count"] == report["search_edge_count"]


def main() -> int:
    args = parse_args()
    sts = load_module(args.build_dir)
    sim = sts.StepSimulator(sts.CharacterClass.IRONCLAD, args.seed, 0)
    advance_to_battle(sim)
    checkpoint = sim.capture_checkpoint()

    baseline = sim.battle_search_v2(1)
    assert "tree_geometry" not in baseline["tree_internal_telemetry"]
    sim.restore_checkpoint(checkpoint)
    geometry = sim.battle_search_v2_with_tree_geometry(1)
    assert without_geometry(baseline) == without_geometry(geometry)
    check_geometry(geometry)
    single = geometry["tree_internal_telemetry"]["tree_geometry"]
    assert single["total_expanded_node_count"] == 1
    assert single["max_expanded_depth"] == 0

    policy_calls = 0

    def policy_prior(snapshot: Mapping[str, Any], actions: Sequence[Mapping[str, Any]]) -> list[float]:
        nonlocal policy_calls
        policy_calls += 1
        assert snapshot["screen_state"] == "BATTLE"
        return [1.0] * len(actions)

    sim.restore_checkpoint(checkpoint)
    policy_report = sim.battle_search_v2_with_tree_geometry(1, False, policy_prior)
    assert policy_calls == 1
    assert policy_report["tree_internal_telemetry"]["policy_prior_calls"] == 1
    check_geometry(policy_report)

    leaf_calls = 0

    def leaf_value(snapshot: Mapping[str, Any], actions: Sequence[Mapping[str, Any]]) -> float:
        nonlocal leaf_calls
        leaf_calls += 1
        assert snapshot["screen_state"] == "BATTLE"
        assert actions
        return 1.0

    sim.restore_checkpoint(checkpoint)
    leaf_report = sim.battle_search_v2_with_tree_geometry(1, False, None, leaf_value)
    assert leaf_calls == 1
    assert leaf_report["tree_internal_telemetry"]["leaf_value_calls"] == 1
    check_geometry(leaf_report)

    def make_combined_callbacks() -> tuple[Any, Any, dict[str, int]]:
        counts = {"policy": 0, "leaf": 0}

        def combined_policy(
                snapshot: Mapping[str, Any],
                actions: Sequence[Mapping[str, Any]]) -> list[float]:
            counts["policy"] += 1
            assert snapshot["screen_state"] == "BATTLE"
            return [1.0] * len(actions)

        def combined_leaf(
                snapshot: Mapping[str, Any],
                actions: Sequence[Mapping[str, Any]]) -> float:
            counts["leaf"] += 1
            assert snapshot["screen_state"] == "BATTLE"
            assert actions
            return 1.0

        return combined_policy, combined_leaf, counts

    baseline_policy, baseline_leaf, baseline_counts = make_combined_callbacks()
    sim.restore_checkpoint(checkpoint)
    combined_baseline = sim.battle_search_v2(
        16, False, baseline_policy, baseline_leaf)
    geometry_policy, geometry_leaf, geometry_counts = make_combined_callbacks()
    sim.restore_checkpoint(checkpoint)
    combined_geometry = sim.battle_search_v2_with_tree_geometry(
        16, False, geometry_policy, geometry_leaf)
    assert without_geometry(combined_baseline) == without_geometry(combined_geometry)
    assert baseline_counts == geometry_counts
    assert combined_baseline["tree_internal_telemetry"]["policy_prior_calls"] == (
        combined_geometry["tree_internal_telemetry"]["policy_prior_calls"])
    assert combined_baseline["tree_internal_telemetry"]["leaf_value_calls"] == (
        combined_geometry["tree_internal_telemetry"]["leaf_value_calls"])
    assert combined_baseline["native_simulator_steps"] == combined_geometry["native_simulator_steps"]
    assert combined_baseline["root_rows"] == combined_geometry["root_rows"]
    check_geometry(combined_geometry)

    sim.restore_checkpoint(checkpoint)
    multi_a = sim.battle_search_v2_with_tree_geometry(16)
    sim.restore_checkpoint(checkpoint)
    multi_b = sim.battle_search_v2_with_tree_geometry(16)
    assert multi_a == multi_b
    check_geometry(multi_a)

    print("battle_search_v2 tree geometry integration checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
