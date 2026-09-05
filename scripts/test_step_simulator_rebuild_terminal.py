#!/usr/bin/env python3
"""Regression test for terminal battle-start rebuilds.

Seed 851450 reaches a battle whose start effects kill the last monster.  The
ordinary game-action entry must complete that battle through the same
``bc.exitBattle`` lifecycle as ``step`` instead of leaving an active battle
with no legal actions.  The test also covers changed and no-op rebuilds on a
normal active battle; a terminal no-op is not publicly reachable after the
ordinary-entry repair.
"""

from __future__ import annotations

import argparse
import importlib
import importlib.machinery
import pathlib
import sys
from collections.abc import Sequence
from typing import Any

REPLAY_ACTIONS = (
    ("game", 1, "event"),
    ("game", 0, "map"),
    ("battle", 65539, "card"),
    ("battle", 0, "card"),
    ("game", 134217728, "reward_gold"),
    ("game", 0, "reward_card"),
    ("game", 805306368, "skip"),
    ("game", 1, "map"),
    ("game", 0, "event"),
    ("game", 1, "map"),
    ("battle", 0, "card"),
    ("battle", 1, "card"),
    ("game", 512, "reward_card"),
    ("game", 134217728, "reward_gold"),
    ("game", 3221225472, "game_potion_discard"),
    ("game", 805306368, "skip"),
    ("game", 2, "map"),
    ("game", 402653186, "shop_reward_potion"),
    ("game", 671088640, "shop_card_remove"),
    ("game", 7, "card_select"),
    ("game", 402653185, "shop_reward_potion"),
    ("game", 3, "shop_reward_card"),
    ("game", 805306368, "shop_skip"),
    ("game", 3, "map"),
    ("game", 0, "event"),
    ("game", 3, "event"),
    ("game", 4, "map"),
    ("game", 1, "rest"),
    ("game", 3, "card_select"),
    ("game", 3, "map"),
    ("game", 1, "event"),
    ("game", 3, "map"),
    ("game", 0, "event"),
    ("game", 2, "map"),
    ("game", 0, "treasure_open"),
    ("game", 536870912, "reward_relic"),
    ("game", 805306368, "skip"),
    ("game", 3, "map"),
)

ASSISTANCE_REBUILDS = {
    2: (0, True),
    10: (0, True),
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--build-dir",
        default="build-py",
        help="Build directory to search for the slaythespire extension.",
    )
    parser.add_argument(
        "--module-dir",
        action="append",
        type=pathlib.Path,
        default=[],
        help="Directory containing the slaythespire extension; may be repeated.",
    )
    parser.add_argument("--seed", type=int, default=851450)
    parser.add_argument("--ascension", type=int, default=20)
    return parser.parse_args()


def add_import_paths(
    module_dirs: Sequence[pathlib.Path], build_dir: pathlib.Path | None
) -> None:
    for module_dir in module_dirs:
        sys.path.insert(0, str(module_dir.resolve()))

    if build_dir is None:
        return
    resolved_build_dir = build_dir.resolve()
    if not resolved_build_dir.exists():
        return
    suffixes = tuple(importlib.machinery.EXTENSION_SUFFIXES)
    candidates = sorted(
        path
        for path in resolved_build_dir.rglob("slaythespire*")
        if path.is_file()
        and path.name.startswith("slaythespire")
        and path.name.endswith(suffixes)
    )
    if candidates:
        sys.path.insert(0, str(candidates[0].parent))


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def find_action(actions: Sequence[Any], scope: str, bits: int, kind: str) -> Any:
    matches = [
        action
        for action in actions
        if action.scope == scope and action.bits == bits and action.kind == kind
    ]
    require(
        len(matches) == 1,
        f"expected one {scope}/{bits}/{kind} action, found {len(matches)}",
    )
    return matches[0]


def assert_rebuild_paths(sts: Any, seed: int, ascension: int) -> None:
    sim = sts.StepSimulator(sts.CharacterClass.IRONCLAD, seed, ascension)
    for scope, bits, kind in REPLAY_ACTIONS[:2]:
        action = find_action(sim.legal_actions(), scope, bits, kind)
        sim.step(action)

    changed = dict(sim.rebuild_battle_start(0, True, -1))
    require(
        changed["screen_state"] == "BATTLE", f"changed rebuild left battle: {changed}"
    )
    require(
        changed["battle_active"] is True, f"changed rebuild is not active: {changed}"
    )
    require(
        changed["battle_outcome"] == "UNDECIDED",
        f"changed rebuild is terminal: {changed}",
    )
    require(
        changed["potion_count"] == 1, f"changed rebuild did not apply potion: {changed}"
    )

    unchanged = dict(sim.rebuild_battle_start(0, False, -1))
    require(
        unchanged["screen_state"] == "BATTLE", f"no-op rebuild left battle: {unchanged}"
    )
    require(
        unchanged["battle_active"] is True, f"no-op rebuild is not active: {unchanged}"
    )
    require(
        unchanged["battle_outcome"] == "UNDECIDED",
        f"no-op rebuild changed outcome: {unchanged}",
    )
    require(
        unchanged["potion_count"] == changed["potion_count"],
        "no-op rebuild changed resources",
    )


def main() -> int:
    args = parse_args()
    build_dir = None if args.build_dir == "" else pathlib.Path(args.build_dir)
    add_import_paths(args.module_dir, build_dir)
    sts = importlib.import_module("slaythespire")
    assert_rebuild_paths(sts, args.seed, args.ascension)
    sim = sts.StepSimulator(sts.CharacterClass.IRONCLAD, args.seed, args.ascension)

    last_transition: dict[str, Any] | None = None
    for action_index, (scope, bits, kind) in enumerate(REPLAY_ACTIONS):
        if action_index in ASSISTANCE_REBUILDS:
            hp_bonus, add_random_potion = ASSISTANCE_REBUILDS[action_index]
            sim.rebuild_battle_start(hp_bonus, add_random_potion, -1)
        action = find_action(sim.legal_actions(), scope, bits, kind)
        last_transition = dict(sim.step(action))

    require(last_transition is not None, "seed replay produced no transition")
    ordinary_entry = last_transition
    require(
        ordinary_entry["screen_state"] == "REWARDS",
        f"ordinary entry did not advance to rewards: {ordinary_entry}",
    )
    require(
        ordinary_entry["outcome"] == "UNDECIDED",
        f"ordinary entry changed run outcome: {ordinary_entry}",
    )
    require(
        ordinary_entry["battle_active"] is False,
        f"ordinary entry left battle active: {ordinary_entry}",
    )
    require(
        ordinary_entry["completed_battle_outcome"] == "PLAYER_VICTORY",
        f"ordinary entry lost completion label: {ordinary_entry}",
    )
    require(
        "battle_outcome" not in ordinary_entry,
        f"ordinary entry retained stale battle fields: {ordinary_entry}",
    )
    require(
        len(sim.legal_actions()) > 0,
        "ordinary entry left no legal game actions",
    )

    print("terminal battle-start transition regression passed")
    print(f"seed: {args.seed}")
    print(f"ascension: {args.ascension}")
    print(f"replayed_actions: {len(REPLAY_ACTIONS)}")
    print(f"ordinary_entry_screen: {ordinary_entry['screen_state']}")
    print(f"completed_battle_outcome: {ordinary_entry['completed_battle_outcome']}")
    print("changed_and_noop_rebuild_paths: passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
