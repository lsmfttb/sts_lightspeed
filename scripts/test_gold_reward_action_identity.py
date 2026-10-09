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
            projection = sim.public_projection()
            screen_payload = projection["screen_payload"]
            if screen_payload["availability"] != "unsupported":
                raise AssertionError(
                    "public projection unexpectedly exposed the private reward payload"
                )
            projected_actions = projection["candidate_actions"]["value"]
            projected_gold = [
                candidate
                for candidate in projected_actions
                if candidate["kind"] == "reward_gold"
            ]
            if not any(candidate["idx1"] == action.idx1 for candidate in projected_gold):
                raise AssertionError("public projection lost the gold action slot identity")
            if any(
                "gold_reward" in candidate or "amount" in candidate
                for candidate in projected_gold
            ):
                raise AssertionError("public projection exposed a gold reward amount")
            before_gold = sim.snapshot()["gold"]
            after = sim.step(action)
            if after["gold"] <= before_gold:
                raise AssertionError(
                    f"gold action did not collect its reward at step {step_index}: "
                    f"before={before_gold} after={after['gold']}"
                )
            print(
                "public gold action round-trip passed: "
                f"seed={args.seed} step={step_index} idx1={action.idx1} "
                f"bits={action.bits} gold={before_gold}->{after['gold']}"
            )
            return 0

        sim.step(action)

    raise AssertionError(
        f"seed {args.seed} replay did not reach a gold reward action within "
        f"{min(args.max_steps, len(REPLAY_ACTIONS))} steps"
    )


if __name__ == "__main__":
    raise SystemExit(main())
