"""Focused smoke checks for the task-independent native simulator surface."""

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


def reach_normal_battle(
    sts: Any,
    seed: int,
    ascension: int,
    character_class: Any | None = None,
) -> Any:
    if character_class is None:
        character_class = sts.CharacterClass.IRONCLAD
    simulator = sts.StepSimulator(character_class, seed, ascension)
    for _ in range(512):
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

    simulator = reach_normal_battle(sts, args.seed, args.ascension)
    state = simulator.public_battle_state()
    require(
        state.get("schema_id") == "native-public-battle-state-v1",
        "unexpected public battle-state schema",
    )
    require(state.get("information_regime") == "normal_public", "wrong information regime")
    require(not contains_private_identity(state), "public battle state exposes private identity")
    player = state["player"]
    require(isinstance(player.get("active_statuses"), list), "active player statuses are missing")
    require("stance" in player and "stance_id" in player, "player stance is missing")
    require("orb_slots" in player and "orb_state" in player, "player orb state is missing")

    identities = state["ordered_public_legal_actions"]
    actions = simulator.legal_actions()
    require(identities and len(identities) == len(actions), "public action surface mismatch")
    encoded = [json.dumps(row, sort_keys=True, separators=(",", ":")) for row in identities]
    require(len(set(encoded)) == len(encoded), "public legal-action identities are ambiguous")
    for identity in identities:
        require("bits" not in identity, "public action identity contains native action bits")
        require("bits=" not in identity.get("label", ""), "public action label contains native bits")

    # Compare every public action against the corresponding executable native action.
    for action_index, identity in enumerate(identities):
        public_simulator = reach_normal_battle(sts, args.seed, args.ascension)
        native_simulator = reach_normal_battle(sts, args.seed, args.ascension)
        cloned_state = public_simulator.public_battle_state()
        native_actions = native_simulator.legal_actions()
        require(
            cloned_state["ordered_public_legal_actions"][action_index] == identity,
            "deterministic clone changed public legal-action identity",
        )
        public_result = public_simulator.step_public_action(identity)
        native_simulator.step(native_actions[action_index])
        if "battle_state" in public_result:
            require(
                public_result["battle_state"] == native_simulator.public_battle_state(),
                f"public action {action_index} diverged from its native action",
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

    checkpoint = simulator.capture_checkpoint()
    before = simulator.public_battle_state()
    simulator.step_public_action(identities[0])
    simulator.restore_checkpoint(checkpoint)
    require(
        simulator.public_battle_state() == before,
        "checkpoint restore did not recover the public battle decision state",
    )

    search = simulator.battle_search_v2(16)
    require(search.get("simulations_requested") == 16, "search budget was not preserved")
    require(search.get("root_visits", 0) > 0, "unguided native search did not visit its root")
    require(search.get("model_calls") == 0, "unguided native search reported model calls")
    require(
        len(search.get("root_rows", [])) == len(identities),
        "search root rows do not cover the public legal actions",
    )
    require(not contains_private_identity(search), "search result exposes native action identity")

    battle_start_simulator = reach_normal_battle(sts, args.seed, args.ascension)
    encounters = battle_start_simulator.legal_battle_start_encounters()
    require(encounters, "battle-start API returned no legal encounter")
    rebuilt = battle_start_simulator.rebuild_battle_start(0, True, -1)
    require(rebuilt.get("battle_active") is True, "battle-start rebuild lost active battle state")

    defect_simulator = reach_normal_battle(
        sts, args.seed, args.ascension, sts.CharacterClass.DEFECT
    )
    defect_state = defect_simulator.public_battle_state()
    require(
        defect_state.get("information_fidelity") == "unsupported_fidelity",
        "unmodeled Defect orb state was reported as supported",
    )
    require(
        defect_state["player"]["orb_state"].get("availability") == "unsupported",
        "unmodeled Defect orb state was not identified",
    )

    print("NATIVE_CAPABILITY_SMOKE_PASS")
    print(f"public_actions={len(identities)}")
    print(f"search_root_visits={search['root_visits']}")
    print(f"battle_start_encounters={len(encounters)}")
    print("defect_orb_state=unsupported_fidelity")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
