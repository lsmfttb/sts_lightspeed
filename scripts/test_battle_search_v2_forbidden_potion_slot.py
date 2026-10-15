#!/usr/bin/env python3
"""Focused Search-v2 API checks for an opt-in protected potion slot."""

from __future__ import annotations

import argparse
import importlib
import importlib.machinery
from pathlib import Path
import sys
from typing import Any


def load_module(build_dir: Path) -> Any:
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


def action_identity(action: Any) -> tuple[Any, ...]:
    if isinstance(action, dict):
        return tuple(action[key] for key in ("scope", "kind", "idx1", "idx2", "idx3", "bits", "label"))
    return tuple(getattr(action, key) for key in ("scope", "kind", "idx1", "idx2", "idx3", "bits", "label"))


def find_battle_with_searchable_potions(sts: Any) -> tuple[Any, int, list[Any]]:
    for seed in range(1, 25):
        sim = sts.StepSimulator(sts.CharacterClass.IRONCLAD, seed, 0)
        advance_to_battle(sim)
        snapshot = sim.snapshot()
        for _ in range(snapshot["battle_potion_capacity"]):
            if snapshot["battle_potion_count"] >= snapshot["battle_potion_capacity"]:
                break
            snapshot = sim.rebuild_battle_start(0, True, -1)
        potions = snapshot["battle_potions"]
        occupied = [row["potion_index"] for row in potions if row["id_label"] != "EMPTY_POTION_SLOT"]
        if not occupied:
            continue
        protected_slot = occupied[0]
        legal_actions = sim.legal_actions()
        searchable_other_potion = any(
            action.kind == "potion" and action.idx1 != protected_slot
            for action in legal_actions
        )
        if searchable_other_potion:
            return sim, protected_slot, legal_actions
    raise AssertionError("could not construct a battle with another searchable potion")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--simulations", type=int, default=400)
    args = parser.parse_args()
    if args.simulations <= 0:
        raise ValueError("simulations must be positive")

    sts = load_module(args.build_dir)
    sim, protected_slot, legal_before = find_battle_with_searchable_potions(sts)
    legal_identities = [action_identity(action) for action in legal_before]

    baseline = sim.battle_search_v2(args.simulations, True)
    disabled = sim.battle_search_v2(
        args.simulations, True, None, None, forbidden_potion_slot=-1
    )
    assert baseline == disabled, "disabled slot constraint changed the exact baseline report"

    report = sim.battle_search_v2(
        args.simulations,
        True,
        forbidden_potion_slot=protected_slot,
    )
    assert report["native_api"] == "StepSimulator.battle_search_v2_forbidden_potion_slot.v1"
    assert report["patch_identity"] == "sts_lightspeed_battle_search_v2_forbidden_potion_slot_v1"
    assert report["forbidden_potion_slot"] == protected_slot
    assert report["search_constraint"] == "potion_source_slot_omitted_from_tree_and_random_rollouts"
    assert report["tree_forbidden_potion_slot_exclusion_count"] > 0
    assert report["rollout_forbidden_potion_slot_exclusion_count"] > 0

    rows = report["root_rows"]
    assert [action_identity(row) for row in rows] == legal_identities, (
        "root report lost or reordered normal legal occurrence identities"
    )
    protected_rows = [
        row for row in rows
        if row["kind"] in {"potion", "potion_discard"}
        and row["idx1"] == protected_slot
    ]
    assert protected_rows, "normal legal actions contained no protected-slot occurrence"
    for row in protected_rows:
        assert row["search_exclusion_reason"] == "forbidden_potion_slot"
        assert row["search_tree_present"] is False
        assert row["visits"] == 0
        assert row["evaluation_sum"] is None
        assert row["mean_value"] is None

    ordinary_visited = any(
        row["kind"] not in {"potion", "potion_discard"}
        and row["search_tree_present"]
        and row["visits"] > 0
        for row in rows
    )
    other_potion_visited = any(
        row["kind"] == "potion"
        and row["idx1"] != protected_slot
        and row["search_tree_present"]
        and row["visits"] > 0
        for row in rows
    )
    assert ordinary_visited, "ordinary legal action did not receive a root visit"
    assert other_potion_visited, "an unprotected potion action did not receive a root visit"
    assert [action_identity(action) for action in sim.legal_actions()] == legal_identities, (
        "Search-only constraint changed normal simulator legal actions"
    )
    print("native Search-v2 forbidden potion slot API checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
