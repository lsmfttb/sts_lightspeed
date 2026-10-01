"""Deterministic STSRL-009 configuration-aware root-mapping checks."""

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
    audit = sim.stsr009_configuration_aware_root_mapping_audit()
    required = (
        "potion_use_excluded",
        "potion_discard_excluded",
        "public_action_order_preserved",
        "searched_actions_preserved",
        "no_fake_values",
        "mapping_schema_versioned",
        "diagnostic_v2_counts_correct",
        "required_missing_non_card_fails_closed",
        "enabled_potion_missing_discard_fails_closed",
        "uncovered_edge_fails_closed",
        "search_surface_and_work_unchanged",
        "all_search_edges_covered",
    )
    if audit.get("schema_id") != "native-stsr009-configuration-aware-root-mapping-audit-v1":
        raise AssertionError("unexpected STSRL-009 audit schema")
    failed = [name for name in required if audit.get(name) is not True]
    if failed:
        raise AssertionError(f"STSRL-009 audit failed: {', '.join(failed)}")

    print("STSRL_009_CONFIGURATION_AWARE_ROOT_MAPPING_PASS")
    for name in required:
        print(f"{name}=true")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
