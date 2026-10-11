#!/usr/bin/env python3
"""Check indexed gold actions through the public StepSimulator adapter."""

from __future__ import annotations

import argparse
import importlib
import importlib.machinery
import pathlib
import sys

from test_step_simulator_rebuild_terminal import REPLAY_ACTIONS


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=pathlib.Path, default=pathlib.Path("build-py"))
    parser.add_argument("--module-dir", type=pathlib.Path, action="append", default=[])
    parser.add_argument("--seed", type=int, default=851450)
    parser.add_argument("--ascension", type=int, default=20)
    parser.add_argument("--max-steps", type=int, default=256)
    return parser.parse_args()


def import_simulator(args: argparse.Namespace):
    for path in reversed(args.module_dir):
        sys.path.insert(0, str(path.resolve()))

    build_dir = args.build_dir.resolve()
    if build_dir.is_dir():
        for path in build_dir.rglob("slaythespire*"):
            if path.is_file() and any(
                path.name.endswith(suffix)
                for suffix in importlib.machinery.EXTENSION_SUFFIXES
            ):
                sys.path.insert(0, str(path.parent))
                break
    return importlib.import_module("slaythespire")


def main() -> int:
    args = parse_args()
    simulator = import_simulator(args)
    sim = simulator.StepSimulator(simulator.CharacterClass.IRONCLAD, args.seed, args.ascension)

    for step_index, (expected_scope, expected_bits, expected_kind) in enumerate(
        REPLAY_ACTIONS[: args.max_steps]
    ):
        actions = sim.legal_actions()
        matching_actions = [
            action
            for action in actions
            if action.scope == expected_scope
            and action.bits == expected_bits
            and action.kind == expected_kind
        ]
        if not matching_actions:
            raise AssertionError(
                f"seed {args.seed} replay action missing at step {step_index}: "
                f"scope={expected_scope} bits={expected_bits} kind={expected_kind}"
            )
        action = matching_actions[0]

        if action.kind == "reward_gold":
            if action.idx1 != (action.bits & 0xFF):
                raise AssertionError(
                    f"public idx1 disagrees with serialized action bits at step {step_index}: "
                    f"idx1={action.idx1} bits={action.bits}"
                )
            projection = sim.public_projection_v3()
            if projection.get("schema_id") != "native-public-projection-v3":
                raise AssertionError("gold action check received an unexpected public projection schema")
            screen_payload = projection["screen_payload"]
            if screen_payload["availability"] != "available":
                raise AssertionError(
                    "public projection omitted the currently visible reward choices"
                )
            payload = screen_payload["value"]
            projected_gold = [
                choice
                for choice in payload["choices"]
                if choice["choice_type"] == "gold"
                and choice["candidate_action"]["idx1"] == action.idx1
            ]
            if len(projected_gold) != 1:
                raise AssertionError("visible gold option is missing or ambiguously associated")
            gold_choice = projected_gold[0]
            candidate_index = gold_choice["candidate_index"]
            if type(candidate_index) is not int:
                raise AssertionError("visible gold option has a non-integer candidate index")
            projected_actions = projection["candidate_actions"]["value"]
            if not 0 <= candidate_index < len(projected_actions):
                raise AssertionError("visible gold option has an invalid candidate index")
            candidate = projected_actions[candidate_index]
            if candidate != gold_choice["candidate_action"]:
                raise AssertionError("visible gold option is associated with another legal action")
            if any(
                candidate[field] != getattr(action, field)
                for field in ("scope", "kind", "idx1", "idx2", "idx3")
            ):
                raise AssertionError("visible gold option does not identify the selected action")
            gold_amount = gold_choice["gold"]
            if type(gold_amount) is not int or gold_amount <= 0:
                raise AssertionError("visible gold option has an invalid reward amount")
            before_gold = sim.snapshot()["gold"]
            after = sim.step(action)
            if after["gold"] - before_gold != gold_amount:
                raise AssertionError(
                    f"gold action did not collect the projected reward at step {step_index}: "
                    f"expected={gold_amount} before={before_gold} after={after['gold']}"
                )
            print(
                "public gold option identity and amount round-trip passed: "
                f"seed={args.seed} step={step_index} idx1={action.idx1} "
                f"amount={gold_amount} gold={before_gold}->{after['gold']}"
            )
            return 0

        sim.step(action)

    raise AssertionError(
        f"seed {args.seed} replay did not reach a gold reward action within "
        f"{min(args.max_steps, len(REPLAY_ACTIONS))} steps"
    )


if __name__ == "__main__":
    raise SystemExit(main())
