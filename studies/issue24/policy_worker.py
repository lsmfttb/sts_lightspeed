"""JSON-bytes-only noncombat policy process for Issue 24."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import sys
from pathlib import Path
from typing import Any


def _canonical(value: object) -> bytes:
    return json.dumps(
        value,
        ensure_ascii=False,
        allow_nan=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")


class PublicNonCombatPolicy:
    def __init__(self, driver_seed: int) -> None:
        from sts_combat_rl.sim.non_combat_policy import ExpertNonCombatDriver

        self._driver = ExpertNonCombatDriver(seed=driver_seed)

    def __call__(self, public_input: bytes) -> dict[str, Any]:
        if type(public_input) is not bytes:
            raise TypeError("policy input must be immutable JSON bytes")
        document = json.loads(public_input)
        context = document["decision_context"]
        screen = str(context["screen_state"])
        if screen == "BATTLE":
            raise ValueError("Battle decisions belong to the separate public native backend")
        from sts_combat_rl.sim.policy_contract import DecisionContext

        decision_context = DecisionContext(
            screen_state=screen,
            snapshot_features=list(context["snapshot_features"]),
            legal_action_features=[list(row) for row in context["legal_action_features"]],
            legal_action_kinds=list(context["legal_action_kinds"]),
            eligible_action_indices=[int(index) for index in context["eligible_action_indices"]],
            snapshot_metadata=dict(context["snapshot_metadata"]),
            legal_action_metadata=[dict(row) for row in context["legal_action_metadata"]],
            tactical_state=dict(context["tactical_state"]),
            tactical_legal_actions=[dict(row) for row in context["tactical_legal_actions"]],
            tactical_feature_schema_id=str(context["tactical_feature_schema_id"]),
            public_run_context=dict(context["public_run_context"]),
        )
        decision = self._driver.select_action(decision_context)
        return {
            "selected_index": int(decision.legal_action_index),
            "policy_role": "expert_non_combat",
            "reason": decision.reason,
            "worker_pid": os.getpid(),
            "input_type": "bytes",
            "input_sha256": hashlib.sha256(public_input).hexdigest(),
            "simulator_extension_importable": importlib.util.find_spec("slaythespire") is not None,
        }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--st-srl-root", type=Path, required=True)
    parser.add_argument("--driver-seed", type=int, required=True)
    args = parser.parse_args()

    sys.path.insert(0, str(args.st_srl_root / "src"))
    if importlib.util.find_spec("slaythespire") is not None:
        print("native simulator unexpectedly importable in policy process", file=sys.stderr)
        return 3
    policy = PublicNonCombatPolicy(args.driver_seed)
    for line in sys.stdin.buffer:
        try:
            public_input = bytes(line.rstrip(b"\r\n"))
            result = policy(public_input)
            sys.stdout.buffer.write(_canonical(result) + b"\n")
            sys.stdout.buffer.flush()
        except Exception as exc:
            failure = {
                "error": "public_noncombat_policy_failed",
                "exception_type": type(exc).__name__,
            }
            sys.stdout.buffer.write(_canonical(failure) + b"\n")
            sys.stdout.buffer.flush()
            return 4
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
