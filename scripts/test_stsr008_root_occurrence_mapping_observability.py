"""Deterministic STSRL-008 root-occurrence mapping diagnostic checks."""

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
    audit = sim.stsr008_root_occurrence_mapping_audit()
    required = (
        "success_completion_reported",
        "success_root_report_semantics_preserved",
        "no_public_surface_classified",
        "multiple_direct_classified",
        "missing_non_card_classified",
        "representative_ineligible_classified",
        "representative_zero_classified",
        "representative_multiple_classified",
        "uncovered_edge_classified",
        "not_reached_absent",
        "later_particle_not_mapping_attempted",
        "snapshot_attempt_isolation",
        "diagnostic_field_whitelist",
    )
    if audit.get("schema_id") != "native-stsr008-root-occurrence-mapping-audit-v1":
        raise AssertionError("unexpected STSRL-008 audit schema")
    failed = [name for name in required if audit.get(name) is not True]
    if failed:
        raise AssertionError(f"STSRL-008 audit failed: {', '.join(failed)}")

    print("STSRL_008_ROOT_OCCURRENCE_MAPPING_OBSERVABILITY_PASS")
    for name in required:
        print(f"{name}=true")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
