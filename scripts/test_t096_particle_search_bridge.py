"""Deterministic STSRL-006 native particle-to-Search bridge checks."""

from __future__ import annotations

import argparse
import importlib
import importlib.machinery
import pathlib
import sys
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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=pathlib.Path, default=pathlib.Path("build-py"))
    args = parser.parse_args()
    sts = load_module(args.build_dir)

    sim = sts.StepSimulator(sts.CharacterClass.IRONCLAD, 1, 20)
    audit = sim.stsr006_particle_search_audit()
    required = (
        "direct_sampler_parity",
        "value_semantics_labeled",
        "root_work_counters_complete",
        "hidden_particle_diversity",
        "duplicate_occurrence_mapping",
        "frozen_eye_search_compatibility",
        "known_draw_constraint_preserved",
        "unsupported_anchor_fails_closed",
    )
    if audit.get("schema_id") != "native-stsr006-particle-search-audit-v1":
        raise AssertionError("unexpected STSRL-006 audit schema")
    failed = [name for name in required if audit.get(name) is not True]
    if failed:
        raise AssertionError(f"STSRL-006 audit failed: {', '.join(failed)}")

    # Exercise the ordinary path as well.  Depending on the opening hand,
    # Search-v2 may have a mechanically deduplicated card edge; the native
    # bridge now reports an explicit occurrence-equivalence mapping for it.
    for _ in range(64):
        snapshot = sim.snapshot()
        if (
            snapshot.get("screen_state") == "BATTLE"
            and snapshot.get("battle_active") is True
            and snapshot.get("battle_input_state") == "PLAYER_NORMAL"
        ):
            sim.sample_hidden_future_particles_search(17, 0, 1, 1, False)
            break
        actions = sim.legal_actions()
        if not actions:
            break
        sim.step(actions[0])

    print("T096_PARTICLE_SEARCH_BRIDGE_PASS")
    for name in required:
        print(f"{name}=true")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
