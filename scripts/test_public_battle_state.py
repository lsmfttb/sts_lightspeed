"""Focused checks for the normal-public battle observation and action path."""

from __future__ import annotations

import argparse
import importlib
import importlib.machinery
import json
import pathlib
import sys
from collections.abc import Mapping, Sequence
from typing import Any


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


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def contains_private_identity(value: Any) -> bool:
    if isinstance(value, Mapping):
        for key, nested in value.items():
            name = str(key)
            if (
                name in {"bits", "seed", "rng", "unique_id"}
                or name.endswith("_rng")
                or "hidden_future" in name
            ):
                return True
            if contains_private_identity(nested):
                return True
    elif isinstance(value, Sequence) and not isinstance(value, (str, bytes, bytearray)):
        return any(contains_private_identity(nested) for nested in value)
    return False


def reach_normal_battle(sts: Any, seed: int, ascension: int) -> Any:
    simulator = sts.StepSimulator(sts.CharacterClass.IRONCLAD, seed, ascension)
    for _ in range(256):
        snapshot = simulator.snapshot()
        if (
            snapshot.get("screen_state") == "BATTLE"
            and snapshot.get("battle_active") is True
            and snapshot.get("battle_input_state") == "PLAYER_NORMAL"
        ):
            return simulator
        actions = simulator.legal_actions()
        if not actions:
            break
        simulator.step(actions[0])
    raise RuntimeError("bounded smoke did not reach a normal battle decision")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=pathlib.Path, default=pathlib.Path("build-py"))
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--ascension", type=int, default=0)
    args = parser.parse_args()
    sts = load_module(args.build_dir)

    audit_simulator = sts.StepSimulator(
        sts.CharacterClass.IRONCLAD, args.seed, args.ascension
    )
    audit = audit_simulator._test_public_battle_state_semantics()
    required_semantics = (
        "hidden_future_projection_invariant",
        "hidden_draw_order_and_rng_absent",
        "headbutt_known_top_prefix",
        "frozen_eye_full_visible_order",
        "runic_dome_hides_intent",
        "runic_dome_hidden_counter_invariant",
        "public_actions_map_uniquely_without_native_bits",
        "intent_visible_without_runic_dome",
        "state_contains_player_visible_decision_fields",
    )
    failed = [name for name in required_semantics if audit.get(name) is not True]
    if failed:
        raise AssertionError(f"public battle-state semantics failed: {', '.join(failed)}")

    simulator = reach_normal_battle(sts, args.seed, args.ascension)
    state = simulator.public_battle_state()
    require(
        state.get("schema_id") == "native-public-battle-state-v1",
        "unexpected normal-public battle-state schema",
    )
    require(state.get("information_regime") == "normal_public", "wrong information regime")
    require(not contains_private_identity(state), "public battle state exposes private identity")

    action_identities = state["ordered_public_legal_actions"]
    native_actions = simulator.legal_actions()
    require(len(action_identities) == len(native_actions), "action surface length mismatch")
    require(action_identities, "normal battle state has no legal actions")
    encoded = [json.dumps(row, sort_keys=True, separators=(",", ":")) for row in action_identities]
    require(len(set(encoded)) == len(encoded), "public legal action identities are ambiguous")
    for identity in action_identities:
        require("bits" not in identity, "public action identity contains native action bits")
        require("bits=" not in identity.get("label", ""), "public action label contains native bits")

    # Resolve every visible identity to its native action on a deterministic clone.
    for action_index, identity in enumerate(action_identities):
        public_simulator = reach_normal_battle(sts, args.seed, args.ascension)
        native_simulator = reach_normal_battle(sts, args.seed, args.ascension)
        clone_state = public_simulator.public_battle_state()
        clone_actions = native_simulator.legal_actions()
        require(
            clone_state["ordered_public_legal_actions"][action_index] == identity,
            "deterministic clone changed public legal-action identity",
        )
        public_result = public_simulator.step_public_action(identity)
        native_simulator.step(clone_actions[action_index])
        if "battle_state" in public_result:
            require(
                public_result.get("battle_state") == native_simulator.public_battle_state(),
                f"public action {action_index} did not execute its matching native action",
            )
        else:
            require(
                public_result.get("screen_state")
                == native_simulator.snapshot().get("screen_state"),
                f"public action {action_index} produced a different screen transition",
            )
        require(
            not contains_private_identity(public_result),
            "public action result exposes private simulator identity",
        )

    print("PUBLIC_BATTLE_STATE_SEMANTICS_PASS")
    for name in required_semantics:
        print(f"{name}=true")
    print(f"executable_public_actions={len(action_identities)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
