"""Deterministic STSRL-007 bridge-stage observability contract checks."""

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
    audit = sim.stsr007_particle_search_stage_audit()
    required = (
        "success_all_stages_completed",
        "success_output_sanitized",
        "mapping_failure_attributed",
        "search_setup_failure_attributed",
        "search_execution_failure_attributed",
        "sanitized_report_failure_attributed",
        "early_public_failure_stops_later_stages",
    )
    if audit.get("schema_id") != "native-stsr007-particle-search-stage-audit-v1":
        raise AssertionError("unexpected STSRL-007 audit schema")
    failed = [name for name in required if audit.get(name) is not True]
    if failed:
        raise AssertionError(f"STSRL-007 audit failed: {', '.join(failed)}")

    fresh = sts.StepSimulator(sts.CharacterClass.IRONCLAD, 7, 20)
    initial = fresh.last_particle_search_stage_diagnostics()
    if (
        initial.get("schema_id") != "native-particle-search-stage-observability-v1"
        or initial.get("attempt_status") != "not_attempted"
        or initial.get("particles") != []
    ):
        raise AssertionError("stage diagnostics must start in not_attempted state")

    print("STSRL_007_PARTICLE_SEARCH_STAGE_OBSERVABILITY_PASS")
    for name in required:
        print(f"{name}=true")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
