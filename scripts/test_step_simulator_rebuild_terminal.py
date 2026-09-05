#!/usr/bin/env python3
"""Regression test for terminal battle-start rebuilds.

Seed 851450 reaches a battle whose start effects kill the last monster.  The
native battle context therefore has a victory outcome before the first action
is exposed.  ``rebuild_battle_start`` must complete that battle through the
same lifecycle as ``step`` instead of leaving an active battle with no legal
actions.
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


def main() -> int:
    args = parse_args()
    build_dir = None if args.build_dir == "" else pathlib.Path(args.build_dir)
    add_import_paths(args.module_dir, build_dir)
    sts = importlib.import_module("slaythespire")
    sim = sts.StepSimulator(sts.CharacterClass.IRONCLAD, args.seed, args.ascension)

    for action_index, (scope, bits, kind) in enumerate(REPLAY_ACTIONS):
        if action_index in ASSISTANCE_REBUILDS:
            hp_bonus, add_random_potion = ASSISTANCE_REBUILDS[action_index]
            sim.rebuild_battle_start(hp_bonus, add_random_potion, -1)
        action = find_action(sim.legal_actions(), scope, bits, kind)
        sim.step(action)

    before = dict(sim.snapshot())
    require(
        before["screen_state"] == "BATTLE", f"unexpected pre-rebuild screen: {before}"
    )
    require(
        before["outcome"] == "UNDECIDED",
        f"unexpected pre-rebuild run outcome: {before}",
    )
    require(
        before["battle_active"] is True,
        f"battle was not active before rebuild: {before}",
    )
    require(
        before["battle_outcome"] == "PLAYER_VICTORY",
        f"unexpected pre-rebuild battle outcome: {before}",
    )
    require(
        before["battle_monsters_alive"] == 0,
        f"unexpected pre-rebuild monster count: {before}",
    )
    require(before["cur_hp"] == 26, f"seed replay drifted before rebuild: {before}")

    after = dict(sim.rebuild_battle_start(31, False, -1))
    require(
        after["screen_state"] == "REWARDS",
        f"rebuild did not advance to rewards: {after}",
    )
    require(after["outcome"] == "UNDECIDED", f"rebuild changed run outcome: {after}")
    require(after["battle_active"] is False, f"rebuild left battle active: {after}")
    require(
        after["completed_battle_outcome"] == "PLAYER_VICTORY",
        f"missing completion label: {after}",
    )
    require(
        "battle_outcome" not in after, f"stale active-battle fields remain: {after}"
    )
    require(
        len(sim.legal_actions()) > 0, "completed rebuild left no legal game actions"
    )

    print("terminal battle-start rebuild regression passed")
    print(f"seed: {args.seed}")
    print(f"ascension: {args.ascension}")
    print(f"replayed_actions: {len(REPLAY_ACTIONS)}")
    print(f"pre_rebuild_screen: {before['screen_state']}")
    print(f"post_rebuild_screen: {after['screen_state']}")
    print(f"completed_battle_outcome: {after['completed_battle_outcome']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
