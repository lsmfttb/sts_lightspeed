"""Isolated JSON-only public policy process used by Issue 21."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
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


class PublicPolicyCallback:
    """The callable policy boundary accepts one immutable serialized value."""

    def __init__(self, driver_seed: int) -> None:
        from sts_combat_rl.sim.non_combat_policy import ExpertNonCombatDriver

        self._expert = ExpertNonCombatDriver(driver_seed)

    def __call__(self, public_input: bytes) -> dict[str, Any]:
        if type(public_input) is not bytes:
            raise TypeError("public policy input must be immutable bytes")
        document = json.loads(public_input)
        context_document = document["decision_context"]
        screen = str(context_document["screen_state"])

        if screen == "BATTLE":
            candidates = document["ordered_public_legal_candidates"]
            eligible = [int(index) for index in context_document["eligible_action_indices"]]
            if not eligible:
                raise ValueError("public Battle input has no eligible candidates")
            # Input-isolation smoke only: stable choice from public candidate
            # identities. The Issue 17 public heuristic/search comparison is a
            # separate native mechanics-only fixture.
            selected = max(
                eligible,
                key=lambda index: hashlib.sha256(_canonical(candidates[index])).digest(),
            )
            role = "battle_input_probe"
            reason = "stable public-identity hash smoke; no strength claim"
        else:
            from sts_combat_rl.sim.policy_contract import DecisionContext

            context = DecisionContext(
                screen_state=str(context_document["screen_state"]),
                snapshot_features=list(context_document["snapshot_features"]),
                legal_action_features=[
                    list(row) for row in context_document["legal_action_features"]
                ],
                legal_action_kinds=list(context_document["legal_action_kinds"]),
                eligible_action_indices=[
                    int(index) for index in context_document["eligible_action_indices"]
                ],
                snapshot_metadata=dict(context_document["snapshot_metadata"]),
                legal_action_metadata=[
                    dict(row) for row in context_document["legal_action_metadata"]
                ],
                tactical_state=dict(context_document["tactical_state"]),
                tactical_legal_actions=[
                    dict(row) for row in context_document["tactical_legal_actions"]
                ],
                tactical_feature_schema_id=str(
                    context_document["tactical_feature_schema_id"]
                ),
                public_run_context=dict(context_document["public_run_context"]),
            )
            decision = self._expert.select_action(context)
            selected = int(decision.legal_action_index)
            role = "expert_non_combat"
            reason = decision.reason

        return {
            "selected_index": selected,
            "policy_role": role,
            "reason": reason,
            "worker_pid": __import__("os").getpid(),
            "input_type": "bytes",
            "input_sha256": hashlib.sha256(public_input).hexdigest(),
            "simulator_extension_importable": importlib.util.find_spec(
                "slaythespire"
            )
            is not None,
        }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--st-srl-root", type=Path, required=True)
    parser.add_argument("--driver-seed", type=int, required=True)
    args = parser.parse_args()

    # Isolated mode ignores PYTHONPATH/user site. Add only the named legacy
    # policy source; the native extension build directory is never added.
    sys.path.insert(0, str(args.st_srl_root / "src"))
    if importlib.util.find_spec("slaythespire") is not None:
        print("native simulator unexpectedly importable in policy process", file=sys.stderr)
        return 3
    callback = PublicPolicyCallback(args.driver_seed)

    for line in sys.stdin.buffer:
        try:
            public_input = bytes(line.rstrip(b"\r\n"))
            result = callback(public_input)
            sys.stdout.buffer.write(_canonical(result) + b"\n")
            sys.stdout.buffer.flush()
        except Exception as exc:  # report only a coarse worker boundary
            failure = {
                "error": "public_policy_callback_failed",
                "exception_type": type(exc).__name__,
            }
            sys.stdout.buffer.write(_canonical(failure) + b"\n")
            sys.stdout.buffer.flush()
            return 4
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
