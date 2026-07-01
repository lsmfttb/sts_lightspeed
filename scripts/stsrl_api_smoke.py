#!/usr/bin/env python3
"""Smoke-check the STSRL-required slaythespire pybind API surface."""

from __future__ import annotations

import argparse
import importlib
import importlib.machinery
import pathlib
import sys
import traceback
from collections.abc import Mapping, Sequence
from typing import Any


REQUIRED_STEP_SIMULATOR_METHODS = (
    "reset",
    "snapshot",
    "observation",
    "legal_actions",
    "step",
    "capture_checkpoint",
    "restore_checkpoint",
    "public_projection",
    "battle_search",
    "battle_search_with_root_priors",
    "legal_battle_start_encounters",
    "rebuild_battle_start",
)

REQUIRED_SNAPSHOT_FIELDS = (
    "potions",
    "deck",
    "relics",
    "blue_key",
    "green_key",
    "red_key",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Import the slaythespire pybind module and validate the "
            "STSRL-required StepSimulator API surface."
        )
    )
    parser.add_argument(
        "--build-dir",
        default="build-py",
        help=(
            "Build directory to search recursively for slaythespire extension "
            "artifacts before importing. Use an empty string to skip this "
            "search. Default: build-py"
        ),
    )
    parser.add_argument(
        "--module-dir",
        action="append",
        type=pathlib.Path,
        default=[],
        help=(
            "Directory to prepend to PYTHONPATH before importing slaythespire. "
            "Can be passed more than once."
        ),
    )
    parser.add_argument("--seed", type=int, default=123456789)
    parser.add_argument("--ascension", type=int, default=0)
    return parser.parse_args()


def add_import_paths(module_dirs: Sequence[pathlib.Path], build_dir: pathlib.Path | None) -> list[pathlib.Path]:
    added: list[pathlib.Path] = []
    for module_dir in module_dirs:
        resolved = module_dir.resolve()
        sys.path.insert(0, str(resolved))
        added.append(resolved)

    if build_dir is None:
        return added

    resolved_build_dir = build_dir.resolve()
    if not resolved_build_dir.exists():
        return added

    suffixes = tuple(importlib.machinery.EXTENSION_SUFFIXES)
    candidates = sorted(
        path
        for path in resolved_build_dir.rglob("slaythespire*")
        if path.is_file() and path.name.startswith("slaythespire") and path.name.endswith(suffixes)
    )
    if candidates:
        module_parent = candidates[0].parent
        sys.path.insert(0, str(module_parent))
        added.append(module_parent)
    return added


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def require_mapping(value: Any, label: str) -> Mapping[str, Any]:
    require(isinstance(value, Mapping), f"{label} must return a mapping, got {type(value).__name__}")
    return value


def require_sequence(value: Any, label: str) -> Sequence[Any]:
    require(
        isinstance(value, Sequence) and not isinstance(value, (str, bytes, bytearray)),
        f"{label} must return a non-string sequence, got {type(value).__name__}",
    )
    return value


def main() -> int:
    args = parse_args()
    build_dir = None if args.build_dir == "" else pathlib.Path(args.build_dir)

    try:
        added_paths = add_import_paths(args.module_dir, build_dir)
        sts = importlib.import_module("slaythespire")

        require(hasattr(sts, "CharacterClass"), "slaythespire.CharacterClass is missing")
        require(hasattr(sts.CharacterClass, "IRONCLAD"), "slaythespire.CharacterClass.IRONCLAD is missing")
        require(hasattr(sts, "StepSimulator"), "slaythespire.StepSimulator is missing")

        sim = sts.StepSimulator(sts.CharacterClass.IRONCLAD, args.seed, args.ascension)
        for method_name in REQUIRED_STEP_SIMULATOR_METHODS:
            method = getattr(sim, method_name, None)
            require(callable(method), f"StepSimulator.{method_name} is missing or not callable")

        snapshot = require_mapping(sim.snapshot(), "StepSimulator.snapshot()")
        for field_name in REQUIRED_SNAPSHOT_FIELDS:
            require(field_name in snapshot, f"snapshot missing required field: {field_name}")

        require_sequence(snapshot["potions"], "snapshot['potions']")
        require_sequence(snapshot["deck"], "snapshot['deck']")
        require_sequence(snapshot["relics"], "snapshot['relics']")
        for field_name in ("blue_key", "green_key", "red_key"):
            require(isinstance(snapshot[field_name], bool), f"snapshot['{field_name}'] must be bool")

        observation = require_sequence(sim.observation(), "StepSimulator.observation()")
        require(len(observation) > 0, "StepSimulator.observation() returned an empty sequence")

        legal_actions = require_sequence(sim.legal_actions(), "StepSimulator.legal_actions()")

        projection = require_mapping(sim.public_projection(), "StepSimulator.public_projection()")
        require(projection.get("schema_id") == "native-public-projection-v1", "unexpected public_projection schema_id")

        checkpoint = sim.capture_checkpoint()
        restored_snapshot = require_mapping(
            sim.restore_checkpoint(checkpoint),
            "StepSimulator.restore_checkpoint()",
        )
        require(
            restored_snapshot.get("screen_state") == snapshot.get("screen_state"),
            "checkpoint restore changed screen_state during smoke check",
        )

        sim.reset(sts.CharacterClass.IRONCLAD, args.seed, args.ascension)
        reset_snapshot = require_mapping(sim.snapshot(), "StepSimulator.snapshot() after reset")
        require(
            reset_snapshot.get("screen_state") == snapshot.get("screen_state"),
            "reset changed initial screen_state for the same seed",
        )

        print("STSRL native API smoke check passed")
        print(f"python_version: {sys.version.split()[0]}")
        print(f"slaythespire_file: {getattr(sts, '__file__', '<built-in>')}")
        print(f"build_dir: {build_dir.resolve() if build_dir is not None else '<skipped>'}")
        print("import_paths_added:")
        for path in added_paths:
            print(f"  - {path}")
        print(f"seed: {args.seed}")
        print(f"ascension: {args.ascension}")
        print(f"checked_methods: {', '.join(REQUIRED_STEP_SIMULATOR_METHODS)}")
        print(f"checked_snapshot_fields: {', '.join(REQUIRED_SNAPSHOT_FIELDS)}")
        print(f"observation_length: {len(observation)}")
        print(f"legal_actions_count: {len(legal_actions)}")
        return 0
    except Exception as exc:
        print(f"STSRL native API smoke check failed: {exc}", file=sys.stderr)
        traceback.print_exc()
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
