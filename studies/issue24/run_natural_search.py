"""Run the Issue 25 coverage frontier through the pinned STSRL path."""

from __future__ import annotations

import argparse
import atexit
import hashlib
import json
import os
from pathlib import Path
import select
import subprocess
import sys
import time
from types import SimpleNamespace
from typing import Any, Mapping, Sequence


ISSUE24_SCHEMA = "issue24-v1-public-policy-input"
NATIVE_SCHEMA = "native-public-projection-v3"
NATIVE_BASE = "d1dcd6534ec4a1f38ac1f7f916f01a3f931fcfd0"
ISSUE24_SOURCE = "c6021fb4a59d6ef73db6cdca1de981957937078d"
LEGACY_BASE = "3037b75eca4bd73fa70d018ffd4442a1f2d65628"
PUBLIC_ADAPTER_SOURCE = "77771184827de85a0125177b852e762d2d1372dd"
BASELINE_SOURCE = "9a2792e1e02157124b4f90edc91b7ad8765d5d10"
DRIVER_SEED = 21021
GAME_SEEDS = (49, 50)
ASCENSION = 20
MAX_DECISIONS = 96
MAX_RUN_SECONDS = 180.0
NATIVE_DECISION_TIMEOUT = MAX_RUN_SECONDS


def canonical_bytes(value: object) -> bytes:
    return json.dumps(
        value,
        ensure_ascii=False,
        allow_nan=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")


def _display_check(value: object) -> str:
    return "yes" if value is True else "no" if value is False else "not reached"


def _native_failure_evidence(stderr: str) -> dict[str, Any]:
    evidence: dict[str, Any] = {}
    for line in stderr.splitlines():
        if line.startswith("NATIVE_STAGE "):
            evidence["native_execution_last_stage"] = line.split(maxsplit=1)[1]
        elif line.startswith("PUBLIC_ROOT_REPLAY_VALIDATED "):
            evidence["root_replay_validated"] = True
            before_identity, marker, identity = line.partition(" baseline_identity=")
            for token in before_identity.split():
                if token.startswith("decisions="):
                    evidence["replayed_decision_count"] = int(token.split("=", 1)[1])
                elif token.startswith("candidates="):
                    evidence["root_candidate_count"] = int(token.split("=", 1)[1])
                elif token.startswith("baseline_ordinal="):
                    evidence["baseline_public_ordinal"] = int(token.split("=", 1)[1])
                elif token == "baseline_action_accepted=true":
                    evidence["baseline_action_accepted"] = True
            if marker and identity:
                evidence["baseline_public_action"] = json.loads(identity)
        elif line.startswith("PRIVATE_ANCHOR_COUNTER_FAULT_CHANGED_PUBLIC_TRANSITION=true"):
            evidence["private_anchor_counter_fault_changed_public_transition"] = True
        elif line.startswith("PUBLIC_SAMPLE_POOL_EQUAL=true "):
            evidence["public_sample_pool_equal"] = True
        elif line.startswith("ANCHOR_INVARIANCE_BLOCKED: public-consistent sampler depends on hidden anchor"):
            evidence["public_sample_pool_equal"] = False
        elif line.startswith("PUBLIC_SEARCH_AUDIT_COMPLETED "):
            audit: dict[str, Any] = {"completed": True}
            for token in line.split()[1:]:
                key, separator, value = token.partition("=")
                if not separator:
                    continue
                audit[key] = value == "true" if value in ("true", "false") else int(value)
            evidence["search_audit"] = audit
    return evidence


def _plain(value: Any) -> Any:
    """Copy only JSON-native values; reject native Python objects."""

    if value is None or isinstance(value, (str, int, float, bool)):
        if isinstance(value, float) and not (float("-inf") < value < float("inf")):
            raise ValueError("non-finite public projection value")
        return value
    if isinstance(value, Mapping):
        return {str(key): _plain(item) for key, item in value.items()}
    if isinstance(value, Sequence) and not isinstance(value, (str, bytes, bytearray)):
        return [_plain(item) for item in value]
    raise TypeError(f"non-JSON value crossed public input builder: {type(value).__name__}")


def _field(value: object) -> Mapping[str, Any]:
    return value if isinstance(value, Mapping) else {}


def _value(field: object) -> Any:
    item = _field(field)
    return item.get("value") if item.get("availability") == "available" else None


def _resources(projection: Mapping[str, Any]) -> dict[str, Any]:
    outer = _field(projection.get("persistent_resources"))
    values = _field(outer.get("value")) if outer.get("availability") == "available" else {}
    return {
        name: _value(values.get(name))
        for name in (
            "current_hp",
            "max_hp",
            "gold",
            "potion_count",
            "potion_capacity",
        )
    }


def _candidate_rows(projection: Mapping[str, Any]) -> list[dict[str, Any]]:
    field = _field(projection.get("candidate_actions"))
    if field.get("availability") != "available" or not isinstance(field.get("value"), list):
        raise ValueError("V3_ADAPTER_GAP: native ordered public candidates unavailable")
    rows = [_plain(row) for row in field["value"]]
    for ordinal, row in enumerate(rows):
        if not isinstance(row, dict):
            raise ValueError(f"V3_ADAPTER_GAP: candidate {ordinal} is not a mapping")
        if set(row) != {"scope", "kind", "idx1", "idx2", "idx3", "label"}:
            raise ValueError(f"V3_ADAPTER_GAP: candidate {ordinal} schema changed")
        if any(key in row for key in ("bits", "native", "action_id")):
            raise ValueError(f"V3_ADAPTER_GAP: privileged action identity at {ordinal}")
    encoded = [canonical_bytes(row) for row in rows]
    if len(set(encoded)) != len(encoded):
        raise ValueError("V3_ADAPTER_GAP: native public candidate identities are ambiguous")
    return rows


def _public_action_identity(action: Any) -> dict[str, Any]:
    raw = action.raw
    scope = str(raw["scope"])
    kind = str(action.kind)
    idx1 = int(raw["idx1"])
    idx2 = int(raw["idx2"])
    idx3 = int(raw["idx3"])
    label = str(action.label)
    if scope == "battle" or "bits=" in label:
        label = f"{scope}.{kind} idx1={idx1} idx2={idx2} idx3={idx3}"
    return {
        "scope": scope,
        "kind": kind,
        "idx1": idx1,
        "idx2": idx2,
        "idx3": idx3,
        "label": label,
    }


def _assert_candidate_parity(projection: Mapping[str, Any], actions: Sequence[Any]) -> None:
    public_rows = _candidate_rows(projection)
    adapted_rows = [_public_action_identity(action) for action in actions]
    if len(public_rows) != len(adapted_rows):
        raise ValueError(
            "V3_ADAPTER_GAP: native candidate count differs from adapter "
            f"(native={len(public_rows)}, adapter={len(adapted_rows)})"
        )
    for ordinal, (public_row, adapted_row) in enumerate(zip(public_rows, adapted_rows)):
        differing_fields = sorted(
            key for key in public_row if public_row.get(key) != adapted_row.get(key)
        )
        if differing_fields:
            # Keep the durable failure boundary useful without logging action values.
            raise ValueError(
                "V3_ADAPTER_GAP: native candidate identity/order differs from adapter "
                f"(ordinal={ordinal}, differing_fields={','.join(differing_fields)})"
            )


def _unavailable(reason: str) -> dict[str, Any]:
    return {"availability": "unavailable", "reason": reason}


def _available(value: Any, source: str) -> dict[str, Any]:
    return {"availability": "available", "source": source, "value": _plain(value)}


def _candidate_items(rows: Sequence[Mapping[str, Any]]) -> list[dict[str, Any]]:
    return [
        {
            "index": index,
            "kind": str(row["kind"]),
            "label": str(row["label"]),
            "identity": dict(row),
            "parameters": {key: int(row[key]) for key in ("idx1", "idx2", "idx3")},
        }
        for index, row in enumerate(rows)
    ]


def coverage_blocker(public_context: Mapping[str, Any]) -> str | None:
    projection = _field(public_context.get("native_public_projection"))
    screen_field = _field(projection.get("screen_identity"))
    screen_value = _value(screen_field)
    if not isinstance(screen_value, str) or not screen_value:
        return (
            "FIREWALL_OR_MAPPING_FAILURE screen=(unknown) field=screen_identity "
            f"source={screen_field.get('source', 'native-public-projection-v3')} "
            f"availability={screen_field.get('availability', 'missing')} "
            f"reason={screen_field.get('reason', 'public screen identity unavailable')}"
        )
    screen = screen_value
    category = (
        "PUBLIC_BATTLE_PROPOSAL_GAP"
        if screen == "BATTLE"
        else "PUBLIC_NONCOMBAT_COVERAGE_GAP"
    )
    boss = _field(projection.get("visible_act_boss"))
    if screen == "MAP_SCREEN" and boss.get("availability") != "available":
        reason = str(boss.get("reason", "visible act boss identity unavailable"))
        return (
            "PUBLIC_NONCOMBAT_COVERAGE_GAP screen=MAP_SCREEN "
            f"field=visible_act_boss source={boss.get('source', 'native-public-projection-v3.visible_act_boss')} "
            f"availability={boss.get('availability', 'missing')} reason={reason}"
        )

    payload_field = _field(projection.get("screen_payload"))
    if payload_field.get("availability") != "available":
        return (
            f"{category} screen={screen} field=screen_payload "
            f"source={payload_field.get('source', 'native-public-projection-v3.screen_payload')} "
            f"availability={payload_field.get('availability', 'missing')} "
            f"reason={payload_field.get('reason', 'payload unavailable')}"
        )
    payload = _field(payload_field.get("value"))
    status = payload.get("coverage_status")
    if status != "supported":
        return (
            f"{category} screen={screen} field=screen_payload.coverage_status "
            f"source={payload.get('source', payload_field.get('source', 'native-public-projection-v3.screen_payload'))} "
            f"value={status} "
            f"reason={payload.get('reason', 'screen coverage is not complete')}"
        )
    candidates = _field(projection.get("candidate_actions"))
    if candidates.get("availability") != "available":
        return (
            f"{category} screen={screen} field=candidate_actions "
            f"source={candidates.get('source', 'native-public-projection-v3.candidate_actions')} "
            f"availability={candidates.get('availability', 'missing')} "
            f"reason={candidates.get('reason', 'ordered public candidates unavailable')}"
        )
    if screen == "BATTLE":
        battle = _field(public_context.get("native_public_battle_observation"))
        if battle.get("information_regime") != "normal_public":
            return (
                "PUBLIC_BATTLE_PROPOSAL_GAP screen=BATTLE field=information_regime "
                f"source={battle.get('schema_id', 'native-public-battle-state-v1')} "
                f"value={battle.get('information_regime', 'missing')}"
            )
        if battle.get("information_fidelity") != "supported":
            return (
                "PUBLIC_BATTLE_PROPOSAL_GAP screen=BATTLE field=information_fidelity "
                f"source={battle.get('schema_id', 'native-public-battle-state-v1')} "
                f"value={battle.get('information_fidelity', 'missing')}"
            )
    return None


def _public_fact(value: Any, source: str | None, unavailable_reason: str) -> dict[str, Any]:
    if source is None:
        return _unavailable(unavailable_reason)
    return _available(value, source)


def _public_boundary_evidence(public_context: Mapping[str, Any]) -> dict[str, Any]:
    projection = _field(public_context.get("native_public_projection"))
    battle = _field(public_context.get("native_public_battle_observation"))
    screen = _value(projection.get("screen_identity"))
    candidates = _field(projection.get("candidate_actions"))
    candidate_rows = candidates.get("value") if candidates.get("availability") == "available" else None
    resources_outer = _field(projection.get("persistent_resources"))
    resources = _field(resources_outer.get("value"))
    resource_names = (
        "current_hp",
        "max_hp",
        "gold",
        "potion_count",
        "potion_capacity",
    )
    battle_public = (
        battle.get("schema_id") == "native-public-battle-state-v1"
        and battle.get("information_regime") == "normal_public"
    )
    battle_source = "native-public-battle-state-v1" if battle_public else None
    outside_battle = f"{screen or '(unknown)'} has no verified public battle field"
    return {
        "screen_identity": _plain(projection.get("screen_identity", _unavailable("missing"))),
        "ordered_public_legal_identities": (
            _plain(candidate_rows) if isinstance(candidate_rows, list) else None
        ),
        "candidate_actions_availability": _plain(candidates) if candidates else _unavailable("missing"),
        "screen_payload": _plain(projection.get("screen_payload", _unavailable("missing"))),
        "battle_information_regime": _public_fact(
            battle.get("information_regime"),
            f"{battle_source}.information_regime" if battle_source else None,
            outside_battle,
        ),
        "battle_information_fidelity": _public_fact(
            battle.get("information_fidelity"),
            f"{battle_source}.information_fidelity" if battle_source else None,
            outside_battle,
        ),
        "act": _public_fact(
            battle.get("act"), f"{battle_source}.act" if battle_public else None,
            "act is not exposed by the verified public projection at this screen",
        ),
        "floor": _public_fact(
            battle.get("floor_num"), f"{battle_source}.floor_num" if battle_public else None,
            "floor is not exposed by the verified public projection at this screen",
        ),
        "encounter": _public_fact(
            battle.get("encounter_id"), f"{battle_source}.encounter_id" if battle_public else None,
            outside_battle,
        ),
        "battle_turn": _public_fact(
            battle.get("turn"), f"{battle_source}.turn" if battle_public else None,
            outside_battle,
        ),
        "public_resources": {
            name: _plain(resources.get(name, _unavailable("field missing from public resource projection")))
            for name in resource_names
        },
    }


def _stop_classification(blocker: str | None, terminal: bool, decision_count: int) -> str:
    if blocker:
        if blocker.startswith("WALL_BUDGET"):
            return "WALL_BUDGET"
        if blocker.startswith("PUBLIC_NONCOMBAT_COVERAGE_GAP"):
            return "PUBLIC_NONCOMBAT_COVERAGE_GAP"
        if blocker.startswith("PUBLIC_BATTLE_PROPOSAL_GAP") or any(
            marker in blocker
            for marker in (
                "ANCHOR_INVARIANCE_BLOCKED",
                "ANCHOR_INDEPENDENCE_UNSUPPORTED",
                "cannot represent private monster future counters",
            )
        ):
            return "PUBLIC_BATTLE_PROPOSAL_GAP"
        return "FIREWALL_OR_MAPPING_FAILURE"
    if terminal:
        return "NATIVE_TERMINAL"
    if decision_count >= MAX_DECISIONS:
        return "FIXED_DECISION_BUDGET"
    return "FIREWALL_OR_MAPPING_FAILURE"


def _public_run_context(
    envelope: Mapping[str, Any],
    history: Sequence[Mapping[str, Any]],
    *,
    include_candidates: bool,
) -> dict[str, Any]:
    projection = _field(envelope.get("native_public_projection"))
    screen_field = _field(projection.get("screen_identity"))
    screen_value = _value(screen_field)
    rows = _candidate_rows(projection) if include_candidates else []
    resources_outer = _field(projection.get("persistent_resources"))
    resources_value = _field(resources_outer.get("value"))
    current_map_node = projection.get("current_map_node")
    visible_graph = projection.get("visible_map_graph")
    legal_routes = projection.get("immediately_legal_routes")

    result = {
        "schema_id": "issue24-v3-public-run-context-v1",
        "schema_version": 1,
        "source_projection_schema_id": projection.get("schema_id"),
        "projection_status": "available",
        "native_public_projection": _plain(projection),
        "native_public_battle_observation": _plain(
            envelope.get("native_public_battle_observation")
        ),
        "current": {
            "screen": _plain(screen_field),
            "location": {
                "act": _unavailable("native v3 policy context does not read raw act fields"),
                "floor": _unavailable("native v3 policy context does not read raw floor fields"),
                "room_type": _plain(screen_field),
                "map_node": _plain(current_map_node),
            },
            "result": _unavailable("no public terminal result at this decision boundary"),
        },
        "visible_act_boss": _plain(projection.get("visible_act_boss")),
        "map": {
            "visible_map_graph": _plain(visible_graph),
            "current_node": _plain(current_map_node),
            "immediately_legal_routes": _plain(legal_routes),
        },
        "persistent_resources": {
            "availability": resources_outer.get("availability"),
            "fields": _plain(resources_value),
            **(
                {"reason": resources_outer["reason"]}
                if "reason" in resources_outer
                else {}
            ),
        },
        "screen_payload": _plain(projection.get("screen_payload")),
        "candidate_actions": (
            {"availability": "available", "items": _candidate_items(rows)}
            if include_candidates
            else _unavailable("post-transition candidate actions are not included")
        ),
        "history": [_plain(item) for item in history],
    }
    return _plain(result)


def _decision_context(
    public_context: Mapping[str, Any],
    action_space: Any,
    *,
    action_stubs: Sequence[Any] | None = None,
) -> Any:
    from sts_combat_rl.sim.action_space import action_space_for_screen, eligible_indices
    from sts_combat_rl.sim.contract import SimulatorAction
    from sts_combat_rl.sim.policy_contract import DecisionContext

    projection = _field(public_context.get("native_public_projection"))
    screen = str(_value(projection.get("screen_identity")) or "(unknown)")
    rows = _candidate_rows(projection)
    if action_stubs is None:
        action_stubs = [
            SimulatorAction(
                action_id=f"public:{index}",
                label=str(row["label"]),
                kind=str(row["kind"]),
                raw={key: int(row[key]) for key in ("idx1", "idx2", "idx3")},
            )
            for index, row in enumerate(rows)
        ]
    effective = action_space_for_screen(
        action_space,
        screen_state=screen,
        battle_active=(screen == "BATTLE"),
    )
    eligible = eligible_indices(list(action_stubs), effective)
    resources = _resources(projection)
    numeric_resources = {
        key: value
        for key, value in resources.items()
        if isinstance(value, (int, float)) and not isinstance(value, bool)
    }
    kinds = [str(row["kind"]) for row in rows]
    metadata = [
        {key: int(row[key]) for key in ("idx1", "idx2", "idx3")}
        for row in rows
    ]
    public_actions = [
        {"kind": str(row["kind"]), "identity": dict(row)} for row in rows
    ]
    features = [
        float(numeric_resources.get(name, 0.0))
        for name in ("current_hp", "max_hp", "gold", "potion_count", "potion_capacity")
    ]
    legal_features = [
        [float(row["idx1"]), float(row["idx2"]), float(row["idx3"])]
        for row in rows
    ]
    return DecisionContext(
        screen_state=screen,
        snapshot_features=features,
        legal_action_features=legal_features,
        legal_action_kinds=kinds,
        eligible_action_indices=eligible,
        snapshot_metadata={
            key: numeric_resources[key]
            for key in ("potion_count", "potion_capacity")
            if key in numeric_resources
        },
        legal_action_metadata=metadata,
        tactical_state={"scalars": numeric_resources},
        tactical_legal_actions=public_actions,
        tactical_feature_schema_id="issue24-v3-public-derived-v1",
        public_run_context=dict(public_context),
    )


def _input_document(context: Any) -> dict[str, Any]:
    public_context = _plain(context.public_run_context)
    projection = public_context["native_public_projection"]
    battle = public_context.get("native_public_battle_observation")
    candidates = _candidate_rows(projection)
    decision_context = {
        "screen_state": str(context.screen_state),
        "snapshot_features": list(context.snapshot_features),
        "legal_action_features": [list(row) for row in context.legal_action_features],
        "legal_action_kinds": list(context.legal_action_kinds),
        "eligible_action_indices": list(context.eligible_action_indices),
        "snapshot_metadata": dict(context.snapshot_metadata),
        "legal_action_metadata": [dict(row) for row in context.legal_action_metadata],
        "tactical_state": _plain(context.tactical_state),
        "tactical_legal_actions": [
            _plain(row) for row in context.tactical_legal_actions
        ],
        "tactical_feature_schema_id": str(context.tactical_feature_schema_id),
        "public_run_context": public_context,
    }
    return {
        "schema_id": ISSUE24_SCHEMA,
        "source_projection_schema_id": projection.get("schema_id"),
        "native_public_projection": _plain(projection),
        "native_public_battle_observation": _plain(battle),
        "ordered_public_legal_candidates": candidates,
        "derived_features": {
            "screen_state": str(context.screen_state),
            "candidate_count": len(candidates),
            "resource_scalars": _resources(projection),
            "snapshot_features": list(context.snapshot_features),
            "legal_action_features": [list(row) for row in context.legal_action_features],
            "legal_action_kinds": list(context.legal_action_kinds),
            "eligible_action_indices": list(context.eligible_action_indices),
            "history_count": len(public_context.get("history", [])),
        },
        "decision_context": decision_context,
        "public_history": _plain(public_context.get("history", [])),
    }


def _encode_input(context: Any) -> bytes:
    document = _plain(_input_document(context))
    return canonical_bytes(document)


class V3FirewallError(ValueError):
    pass


class RunJournal:
    """Persist the last reached study boundary, including on an exception."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.state: dict[str, Any] = {
            "schema_id": "issue25-run-progress-v1",
            "status": "running",
            "last_completed_boundary": "startup",
        }
        self.write()
        atexit.register(self.mark_incomplete_if_running)
        self._old_excepthook = sys.excepthook
        sys.excepthook = self._exception_hook

    def write(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        temp_path = self.path.with_suffix(self.path.suffix + ".tmp")
        temp_path.write_text(
            json.dumps(self.state, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        os.replace(temp_path, self.path)

    def checkpoint(self, boundary: str, **details: Any) -> None:
        self.state.update(
            status="running",
            last_completed_boundary=boundary,
            details=_plain(details),
        )
        self.write()

    def complete(self) -> None:
        self.state["status"] = "complete"
        self.write()

    def mark_incomplete_if_running(self) -> None:
        if self.state.get("status") == "running":
            self.state["status"] = "incomplete_or_interrupted"
            self.write()

    def _exception_hook(self, exc_type: type[BaseException], exc: BaseException, tb: Any) -> None:
        self.state.update(
            status="failed",
            failure={"type": exc_type.__name__, "message": str(exc)[:1000]},
        )
        self.write()
        self._old_excepthook(exc_type, exc, tb)


class V3LightSpeedAdapter:
    """Study adapter that keeps opaque native handles on the executor side."""

    def __init__(self, base_adapter: Any) -> None:
        self._base = base_adapter
        self._sim = base_adapter._sim
        self._pending: dict[str, Any] | None = None
        self.mapping_trace: list[dict[str, Any]] = []

    def __getattr__(self, name: str) -> Any:
        return getattr(self._base, name)

    def reset(self, seed: int | None = None) -> Any:
        snapshot = self._base.reset(seed)
        self._pending = None
        self.mapping_trace = []
        return snapshot

    def legal_actions(self, snapshot: Any) -> list[Any]:
        return self._base.legal_actions(snapshot)

    def public_projection(self, snapshot: Any) -> dict[str, Any]:
        self._base._assert_snapshot_is_current(snapshot)
        projection = dict(self._sim.public_projection())
        if projection.get("schema_id") != NATIVE_SCHEMA:
            raise V3FirewallError("V3_ADAPTER_GAP: wrong native public projection schema")
        screen = _value(projection.get("screen_identity"))
        battle = None
        if screen == "BATTLE":
            battle = dict(self._sim.public_battle_state())
        return {
            "native_public_projection": _plain(projection),
            "native_public_battle_observation": _plain(battle),
        }

    def arm_public_selection(
        self,
        projection_json: bytes,
        candidates: Sequence[Mapping[str, Any]],
        selected_index: int,
    ) -> None:
        current = dict(self._sim.public_projection())
        current_json = canonical_bytes(_plain(current))
        expected_document = json.loads(projection_json)
        expected_projection = expected_document["native_public_projection"]
        expected_json = canonical_bytes(expected_projection)
        if current_json != expected_json:
            raise V3FirewallError("STALE_PUBLIC_ACTION: native state changed before execution")
        current_candidates = _candidate_rows(current)
        if canonical_bytes(current_candidates) != canonical_bytes(list(candidates)):
            raise V3FirewallError("PUBLIC_ACTION_MISMATCH: ordered candidate identities changed")
        if selected_index < 0 or selected_index >= len(current_candidates):
            raise V3FirewallError("PUBLIC_ACTION_MISMATCH: public ordinal is outside candidates")
        self._pending = {
            "projection_json": current_json,
            "candidate_bytes": canonical_bytes(current_candidates),
            "selected_index": selected_index,
            "selected_identity": current_candidates[selected_index],
        }

    def step(self, action: Any) -> Any:
        pending = self._pending
        if pending is None:
            raise V3FirewallError("PUBLIC_ACTION_MISMATCH: no controller selection was armed")
        current = dict(self._sim.public_projection())
        candidates = _candidate_rows(current)
        if canonical_bytes(_plain(current)) != pending["projection_json"]:
            self._pending = None
            raise V3FirewallError("STALE_PUBLIC_ACTION: projected state changed before step")
        if canonical_bytes(candidates) != pending["candidate_bytes"]:
            self._pending = None
            raise V3FirewallError("STALE_PUBLIC_ACTION: native action order changed before step")
        actual = _public_action_identity(action)
        expected = pending["selected_identity"]
        if canonical_bytes(actual) != canonical_bytes(expected):
            self._pending = None
            raise V3FirewallError("PUBLIC_ACTION_MISMATCH: selected ordinal maps to another action")
        selected_index = pending["selected_index"]
        if canonical_bytes(candidates[selected_index]) != canonical_bytes(actual):
            self._pending = None
            raise V3FirewallError("PUBLIC_ACTION_MISMATCH: native action differs from selected identity")

        transition = self._base.step(action)
        self.mapping_trace.append(
            {
                "selected_public_ordinal": selected_index,
                "selected_public_identity": _plain(expected),
                "executed_legal_identity_matches": True,
            }
        )
        self._pending = None
        return transition

    def close(self) -> None:
        self._base.close()


class PolicyWorker:
    def __init__(self, worker_script: Path, st_srl_root: Path, driver_seed: int) -> None:
        self.last_public_input: bytes | None = None
        self.process = subprocess.Popen(
            [
                sys.executable,
                "-I",
                str(worker_script),
                "--st-srl-root",
                str(st_srl_root),
                "--driver-seed",
                str(driver_seed),
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=False,
            bufsize=0,
        )

    def choose(self, public_input: bytes, *, timeout: float) -> dict[str, Any]:
        if self.process.poll() is not None:
            detail = self.process.stderr.read().decode("utf-8", errors="replace")
            raise RuntimeError(f"public policy worker exited: {detail[-400:]}")
        assert self.process.stdin is not None and self.process.stdout is not None
        self.last_public_input = bytes(public_input)
        self.process.stdin.write(public_input + b"\n")
        self.process.stdin.flush()
        readable, _, _ = select.select([self.process.stdout], [], [], max(0.0, timeout))
        if not readable:
            self.process.kill()
            self.process.wait(timeout=5)
            raise ValueError(f"WALL_BUDGET: public policy worker exhausted the remaining {timeout:.3f}s")
        line = self.process.stdout.readline()
        if not line:
            detail = ""
            if self.process.stderr is not None:
                detail = self.process.stderr.read().decode("utf-8", errors="replace")
            raise RuntimeError(f"public policy worker returned no response: {detail[-400:]}")
        response = json.loads(line)
        if "error" in response:
            raise RuntimeError(
                f"public policy worker callback failed: {response.get('exception_type')}"
            )
        return response

    def close(self) -> None:
        if self.process.stdin is not None:
            self.process.stdin.close()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            self.process.wait(timeout=5)


class FirewalledController:
    def __init__(
        self,
        worker: PolicyWorker,
        controller_provenance: Any,
        *,
        arm_selection: bool = True,
        journal: RunJournal | None = None,
        run_label: str = "controlled_run",
        native_search: Path,
        ascension: int = ASCENSION,
        max_run_seconds: float = MAX_RUN_SECONDS,
    ):
        self.worker = worker
        self.provenance = controller_provenance
        self.arm_selection = arm_selection
        self.journal = journal
        self.run_label = run_label
        self.native_search = native_search.resolve()
        self.ascension = ascension
        self.max_run_seconds = max_run_seconds
        self.run_started = time.monotonic()
        self.simulator_seed: int | None = None
        self.battle_decisions = 0
        self.events: list[dict[str, Any]] = []
        self.last_native_failure_evidence: dict[str, Any] = {}
        self.last_public_selection: tuple[bytes, list[dict[str, Any]], int] | None = None

    def reset_for_run(self, simulator_seed: int | None) -> None:
        # Only the trusted native search backend receives the fixed game seed.
        # The isolated normal-public policy process receives JSON bytes only.
        self.simulator_seed = simulator_seed
        self.run_started = time.monotonic()
        self.battle_decisions = 0

    def _remaining_seconds(self) -> float:
        return self.max_run_seconds - (time.monotonic() - self.run_started)

    def _record_wall_budget(
        self,
        step_index: int,
        screen: str,
        public_context: Mapping[str, Any],
        detail: str,
    ) -> None:
        event = {
            "step_index": step_index,
            "screen": screen,
            "coverage_status": "wall_budget",
            "stop_classification": "WALL_BUDGET",
            "controller_decision_completed": False,
            "policy_callback_invoked": False,
            "native_execution_parity": "not_attempted",
            "public_boundary": _public_boundary_evidence(public_context),
            "blocker": detail,
        }
        self.events.append(event)
        if self.journal is not None:
            self.journal.checkpoint(
                f"{self.run_label}_wall_budget",
                step_index=step_index,
                screen=screen,
                blocker=detail,
                policy_invoked=False,
            )

    def _native_decision(
        self,
        public_input: bytes,
        mode: str,
        *,
        timeout: float,
    ) -> dict[str, Any]:
        if self.simulator_seed is None:
            raise ValueError("NATURAL_SEARCH_INTEGRATION_BLOCKED: trusted backend lacks run seed")
        started = time.monotonic()
        self.last_native_failure_evidence = {}
        try:
            result = subprocess.run(
                [
                    str(self.native_search),
                    "--mode",
                    mode,
                    "--seed",
                    str(self.simulator_seed),
                    "--ascension",
                    str(self.ascension),
                ],
                input=public_input,
                capture_output=True,
                timeout=max(0.001, timeout),
                check=False,
            )
        except subprocess.TimeoutExpired as exc:
            stderr = exc.stderr.decode("utf-8", errors="replace").strip() if exc.stderr else ""
            raise ValueError(
                f"WALL_BUDGET: trusted native {mode} exhausted the remaining "
                f"{timeout:.3f}s; "
                f"stderr={stderr[:1200]!r}"
            ) from exc
        if result.returncode != 0:
            detail = result.stderr.decode("utf-8", errors="replace").strip()
            stdout = result.stdout.decode("utf-8", errors="replace").strip()
            self.last_native_failure_evidence = _native_failure_evidence(detail)
            raise ValueError(
                f"NATURAL_SEARCH_INTEGRATION_BLOCKED: native {mode} exited "
                f"{result.returncode}; stderr={detail[:1200]!r}; stdout={stdout[:1200]!r}"
            )
        try:
            response = json.loads(result.stdout)
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise ValueError("NATURAL_SEARCH_INTEGRATION_BLOCKED: native backend returned invalid JSON") from exc
        response["policy_role"] = (
            "shared_public_search" if mode == "search" else "issue17_public_tactical_heuristic"
        )
        if "selected_index" not in response and "selected_public_ordinal" in response:
            response["selected_index"] = int(response["selected_public_ordinal"])
        response["reason"] = response.get("selected_action_mode", mode)
        response["worker_pid"] = None
        response["input_type"] = "bytes"
        response["simulator_extension_importable"] = None
        response["policy_worker_invoked"] = False
        response["trusted_native_backend_invoked"] = True
        response["native_backend_wall_seconds"] = time.monotonic() - started
        return response

    def select_action(
        self,
        adapter: Any,
        snapshot: Any,
        actions: Sequence[Any],
        context: Any,
        step_index: int,
    ) -> Any:
        from sts_combat_rl.sim.controller_contract import ControllerDecision

        public_context = context.public_run_context
        screen = str(context.screen_state)
        remaining = self._remaining_seconds()
        if remaining <= 0:
            blocker = f"WALL_BUDGET: run reached {self.max_run_seconds:.0f}s before decision {step_index}"
            self._record_wall_budget(step_index, screen, public_context, blocker)
            raise ValueError(blocker)
        blocker = coverage_blocker(public_context)
        if blocker is not None:
            if self.journal is not None:
                self.journal.checkpoint(
                    f"{self.run_label}_coverage_gap",
                    step_index=step_index,
                    screen=screen,
                    blocker=blocker,
                    policy_invoked=False,
                )
            self.events.append(
                {
                    "step_index": step_index,
                    "screen": screen,
                    "coverage_status": "partial_or_unsupported",
                    "stop_classification": blocker.split(" ", 1)[0],
                    "policy_callback_invoked": False,
                    "controller_decision_completed": False,
                    "native_execution_parity": "not_attempted",
                    "public_boundary": _public_boundary_evidence(public_context),
                    "blocker": blocker,
                }
            )
            raise ValueError(blocker)

        public_input = _encode_input(context)
        remaining = self._remaining_seconds()
        if remaining <= 0:
            blocker = f"WALL_BUDGET: run reached {self.max_run_seconds:.0f}s before decision {step_index}"
            self._record_wall_budget(step_index, screen, public_context, blocker)
            raise ValueError(blocker)
        callback_started = time.monotonic()
        if screen == "BATTLE":
            mode = "search" if self.battle_decisions == 0 else "heuristic"
            try:
                response = self._native_decision(public_input, mode, timeout=remaining)
            except ValueError as exc:
                failure = str(exc)
                event = {
                    "step_index": step_index,
                    "screen": screen,
                    "coverage_status": "native_backend_blocked",
                    "stop_classification": _stop_classification(failure, False, step_index),
                    "controller_decision_completed": False,
                    "native_execution_parity": "not_attempted",
                    "public_boundary": _public_boundary_evidence(public_context),
                    "policy_input_type": "bytes",
                    "policy_input_sha256": hashlib.sha256(public_input).hexdigest(),
                    "native_backend_mode": mode,
                    "native_backend_invoked": True,
                    "native_backend_failure": failure,
                    "native_execution_last_stage": self.last_native_failure_evidence.get(
                        "native_execution_last_stage"
                    ),
                    "native_root_replay_validated": self.last_native_failure_evidence.get(
                        "root_replay_validated", False
                    ),
                    "native_root_candidate_count": self.last_native_failure_evidence.get(
                        "root_candidate_count"
                    ),
                    "native_baseline_public_ordinal": self.last_native_failure_evidence.get(
                        "baseline_public_ordinal"
                    ),
                    "native_baseline_public_action": self.last_native_failure_evidence.get(
                        "baseline_public_action"
                    ),
                    "native_private_anchor_counter_fault_changed_public_transition": self.last_native_failure_evidence.get(
                        "private_anchor_counter_fault_changed_public_transition", False
                    ),
                    "native_public_sample_pool_equal": self.last_native_failure_evidence.get(
                        "public_sample_pool_equal"
                    ),
                    "native_search_audit": self.last_native_failure_evidence.get("search_audit"),
                    "policy_worker_invoked": False,
                    "simulator_extension_importable_in_policy_process": None,
                }
                self.events.append(event)
                if self.journal is not None:
                    self.journal.checkpoint(
                        f"{self.run_label}_native_backend_blocked",
                        step_index=step_index,
                        screen=screen,
                        mode=mode,
                        policy_input_sha256=event["policy_input_sha256"],
                        failure=failure,
                    )
                raise
            self.battle_decisions += 1
        else:
            try:
                response = self.worker.choose(public_input, timeout=remaining)
            except ValueError as exc:
                failure = str(exc)
                if failure.startswith("WALL_BUDGET"):
                    self._record_wall_budget(step_index, screen, public_context, failure)
                raise
            response["policy_worker_invoked"] = True
            response["trusted_native_backend_invoked"] = False
        selected_index = response.get("selected_index")
        candidates = _candidate_rows(public_context["native_public_projection"])
        if isinstance(selected_index, bool) or not isinstance(selected_index, int):
            raise ValueError("PUBLIC_ACTION_MISMATCH: policy returned a non-integer ordinal")
        if selected_index not in context.eligible_action_indices:
            raise ValueError("PUBLIC_ACTION_MISMATCH: public ordinal is not eligible")
        if selected_index < 0 or selected_index >= len(candidates):
            raise ValueError("PUBLIC_ACTION_MISMATCH: public ordinal is outside native candidates")
        if self.arm_selection:
            arm = getattr(adapter, "arm_public_selection", None)
            if not callable(arm):
                raise ValueError("V3_ADAPTER_GAP: trusted executor adapter lacks public mapping")
            if response.get("selected_action_mode") == "shared_public_tree_B192":
                poisoned = json.loads(canonical_bytes(candidates))
                poisoned[0]["label"] = str(poisoned[0].get("label", "")) + "__mismatch_probe"
                try:
                    arm(public_input, poisoned, selected_index)
                except V3FirewallError as exc:
                    if "PUBLIC_ACTION_MISMATCH" not in str(exc):
                        raise
                else:
                    raise ValueError("PUBLIC_ACTION_MISMATCH: mismatched candidate-order probe was accepted")
            arm(public_input, candidates, selected_index)
            self.last_public_selection = (
                bytes(public_input),
                json.loads(canonical_bytes(candidates)),
                selected_index,
            )
        event = {
            "step_index": step_index,
            "screen": screen,
            "coverage_status": "supported",
            "controller_decision_completed": True,
            "policy_callback_invoked": response.get("policy_worker_invoked") is True,
            "policy_role": response.get("policy_role"),
            "policy_worker_pid": response.get("worker_pid"),
            "policy_worker_invoked": response.get("policy_worker_invoked"),
            "trusted_native_backend_invoked": response.get("trusted_native_backend_invoked"),
            "policy_input_type": response.get("input_type"),
            "policy_input_sha256": hashlib.sha256(public_input).hexdigest(),
            "simulator_extension_importable_in_policy_process": response.get(
                "simulator_extension_importable"
            ),
            "selected_public_ordinal": selected_index,
            "selected_public_identity": candidates[selected_index],
            "selected_action_mode": response.get("selected_action_mode"),
            "native_backend_evidence": response.get("shared_public_search"),
            "native_anchor_invariance": response.get("anchor_invariance"),
            "native_root_candidate_count": response.get("all_public_candidate_count"),
            "native_root_replay_validated": (
                response.get("root_public_projection_equal_to_executor") is True
                and response.get("root_public_battle_state_equal_to_executor") is True
            ),
            "native_private_anchor_counter_fault_changed_public_transition": _field(
                response.get("anchor_invariance")
            ).get("private_counter_fault_control_passed") is True,
            "native_public_sample_pool_equal": _field(
                response.get("anchor_invariance")
            ).get("public_sample_pool_equal_for_all_particles"),
            "baseline_public_action": response.get("baseline_public_action"),
            "baseline_public_ordinal": response.get("baseline_public_ordinal"),
            "native_backend_wall_seconds": response.get("native_backend_wall_seconds"),
            "controller_callback_wall_seconds": time.monotonic() - callback_started,
        }
        self.events.append(event)
        if self.journal is not None:
            self.journal.checkpoint(
                f"{self.run_label}_public_decision",
                step_index=step_index,
                screen=screen,
                policy_role=response.get("policy_role"),
                policy_input_sha256=event["policy_input_sha256"],
                selected_public_ordinal=selected_index,
                selected_action_mode=event["selected_action_mode"],
                native_backend_invoked=event["trusted_native_backend_invoked"],
            )
        return ControllerDecision(
            selected_index=selected_index,
            provenance=self.provenance,
            reason=str(response.get("reason", "public policy callback")),
            metadata={"issue24_policy_role": response.get("policy_role")},
        )


def _legacy_revision(st_srl_root: Path) -> str:
    result = subprocess.run(
        ["git", "-C", str(st_srl_root), "rev-parse", "HEAD"],
        check=True,
        capture_output=True,
        text=True,
        env=_isolated_git_env(),
    )
    return result.stdout.strip()


def _isolated_git_env() -> dict[str, str]:
    env = os.environ.copy()
    for name in ("GIT_DIR", "GIT_WORK_TREE", "ISSUE25_GIT_DIR", "ISSUE25_GIT_WORK_TREE"):
        env.pop(name, None)
    return env


def _native_git_env() -> dict[str, str]:
    env = _isolated_git_env()
    git_dir = os.environ.get("ISSUE25_GIT_DIR")
    work_tree = os.environ.get("ISSUE25_GIT_WORK_TREE")
    if git_dir:
        env["GIT_DIR"] = git_dir
    if work_tree:
        env["GIT_WORK_TREE"] = work_tree
    return env


def verify_unsupported_screen_fixture(provenance: Any, native_search: Path) -> dict[str, Any]:
    class CallbackProbe:
        called = False

        def choose(self, public_input: bytes, *, timeout: float) -> dict[str, Any]:
            del public_input, timeout
            self.called = True
            raise AssertionError("unsupported fixture invoked the policy callback")

    worker = CallbackProbe()
    controller = FirewalledController(
        worker,
        provenance,
        native_search=native_search,
        run_label="issue25_unsupported_screen_fixture",
    )
    projection = {
        "schema_id": NATIVE_SCHEMA,
        "screen_identity": _available("SHOP_ROOM", "GameContext::screenState"),
        "visible_act_boss": _unavailable("not applicable outside MAP_SCREEN"),
        "screen_payload": _available(
            {
                "coverage_status": "unsupported",
                "source": "StepSimulator::publicProjection screen coverage and choices",
                "reason": "shop choice coverage is not part of this capability",
            },
            "StepSimulator::publicProjection screen coverage and choices",
        ),
        "candidate_actions": _available(
            [
                {
                    "scope": "game",
                    "kind": "map",
                    "idx1": 0,
                    "idx2": 0,
                    "idx3": 0,
                    "label": "game.map idx1=0 idx2=0 idx3=0",
                }
            ],
            "StepSimulator::legalActions",
        ),
        "persistent_resources": _unavailable("fixture has no resources"),
    }
    public_context = {
        "native_public_projection": projection,
        "native_public_battle_observation": None,
    }
    context = SimpleNamespace(
        screen_state="SHOP_ROOM",
        public_run_context=public_context,
    )
    try:
        controller.select_action(None, None, (), context, 0)
    except ValueError as exc:
        blocker = str(exc)
    else:
        raise AssertionError("unsupported fixture did not fail closed")
    event = controller.events[-1] if controller.events else {}
    if (
        not blocker.startswith("PUBLIC_NONCOMBAT_COVERAGE_GAP")
        or worker.called
        or event.get("policy_callback_invoked") is not False
        or event.get("controller_decision_completed") is not False
        or "selected_public_ordinal" in event
    ):
        raise AssertionError("unsupported fixture reached policy selection")
    return {
        "screen": "SHOP_ROOM",
        "stop_classification": blocker.split(" ", 1)[0],
        "blocker": blocker,
        "policy_callback_invoked": worker.called,
        "selected_action": "selected_public_ordinal" in event,
        "passed": True,
    }


def _require_clean_legacy_worktree(st_srl_root: Path) -> None:
    result = subprocess.run(
        [
            "git",
            "-c",
            "core.autocrlf=true",
            "-C",
            str(st_srl_root),
            "status",
            "--porcelain",
            "--untracked-files=all",
        ],
        check=True,
        capture_output=True,
        text=True,
        env=_isolated_git_env(),
    )
    if result.stdout.strip():
        raise RuntimeError("legacy STSRL source worktree must be clean at the pinned revision")


def _atomic_json(path: Path, value: Mapping[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_suffix(path.suffix + ".tmp")
    temp.write_text(
        json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    os.replace(temp, path)


def _summarize_run(
    run: Any,
    controller: FirewalledController,
    adapter: V3LightSpeedAdapter,
    *,
    game_seed: int,
    driver_seed: int,
    run_wall_seconds: float,
) -> dict[str, Any]:
    events = [dict(event) for event in controller.events]
    events_by_step = {int(event["step_index"]): event for event in events}
    integrity_problems: list[str] = []
    if len(adapter.mapping_trace) != len(run.steps):
        integrity_problems.append(
            f"mapping count differs from executed steps "
            f"(mapping={len(adapter.mapping_trace)}, steps={len(run.steps)})"
        )
    if not all(item.get("executed_legal_identity_matches") is True for item in adapter.mapping_trace):
        integrity_problems.append("selected/executed public identity mismatch")

    decisions_by_screen: dict[str, int] = {}
    decisions_by_mode: dict[str, int] = {}
    step_rows: list[dict[str, Any]] = []
    cumulative_simulations = 0
    cumulative_native_action_steps = 0
    cumulative_native_wall_seconds = 0.0
    cumulative_controller_callback_seconds = 0.0
    search_work_available = False
    for step in run.steps:
        screen = str(step.screen_state)
        decisions_by_screen[screen] = decisions_by_screen.get(screen, 0) + 1
        event = events_by_step.get(int(step.step_index), {})
        mapping_index = int(step.step_index)
        mapping = (
            adapter.mapping_trace[mapping_index]
            if mapping_index < len(adapter.mapping_trace)
            else {}
        )
        mode = str(event.get("selected_action_mode") or event.get("policy_role") or "unknown")
        decisions_by_mode[mode] = decisions_by_mode.get(mode, 0) + 1
        evidence = _field(event.get("native_backend_evidence"))
        if isinstance(evidence.get("simulations"), int):
            cumulative_simulations += int(evidence["simulations"])
            search_work_available = True
        if isinstance(evidence.get("native_public_action_steps"), int):
            cumulative_native_action_steps += int(evidence["native_public_action_steps"])
            search_work_available = True
        native_wall = event.get("native_backend_wall_seconds")
        if isinstance(native_wall, (int, float)) and not isinstance(native_wall, bool):
            cumulative_native_wall_seconds += float(native_wall)
        callback_wall = event.get("controller_callback_wall_seconds")
        if isinstance(callback_wall, (int, float)) and not isinstance(callback_wall, bool):
            cumulative_controller_callback_seconds += float(callback_wall)
        public_boundary = _public_boundary_evidence(step.public_run_context)
        step_rows.append(
            {
                "step_index": int(step.step_index),
                "screen": screen,
                "public_boundary": public_boundary,
                "ordered_public_legal_identities": public_boundary[
                    "ordered_public_legal_identities"
                ],
                "controller_mode": mode,
                "selected_public_ordinal": event.get("selected_public_ordinal"),
                "selected_public_action": event.get("selected_public_identity"),
                "executed_public_action": mapping.get("selected_public_identity"),
                "native_execution_parity": mapping.get("executed_legal_identity_matches", False),
                "native_execution_parity_source": "V3LightSpeedAdapter.step public identity check",
                "terminal_after_step": bool(step.terminal_after_step),
                "cumulative_work": {
                    "controlled_actions_executed": _available(
                        int(step.step_index) + 1,
                        "STSRL execute_controlled_run executed step index",
                    ),
                    "shared_public_search_simulations": (
                        _available(cumulative_simulations, "trusted native shared-public search response")
                        if search_work_available
                        else _unavailable("no shared public search completed by this decision")
                    ),
                    "native_public_search_action_steps": (
                        _available(cumulative_native_action_steps, "trusted native shared-public search response")
                        if search_work_available
                        else _unavailable("no shared public search completed by this decision")
                    ),
                    "native_backend_wall_seconds": _available(
                        round(cumulative_native_wall_seconds, 6),
                        "study monotonic timer around trusted native backend calls",
                    ),
                    "controller_callback_wall_seconds": _available(
                        round(cumulative_controller_callback_seconds, 6),
                        "study monotonic timer around controller callbacks",
                    ),
                },
            }
        )

    search_events = [
        event for event in events
        if event.get("selected_action_mode") == "shared_public_tree_B192"
    ]
    if search_events:
        if len(search_events) != 1:
            integrity_problems.append("more than one shared-public search decision completed")
        search_event = search_events[0]
        search_step = next(
            (step for step in run.steps if int(step.step_index) == int(search_event["step_index"])),
            None,
        )
        if search_step is None or search_step.screen_state != "BATTLE":
            integrity_problems.append("shared-public search action was not an executed Battle step")
        elif int(search_step.step_index) < len(adapter.mapping_trace):
            mapping = adapter.mapping_trace[int(search_step.step_index)]
            if (
                mapping.get("selected_public_ordinal") != search_event.get("selected_public_ordinal")
                or mapping.get("selected_public_identity") != search_event.get("selected_public_identity")
            ):
                integrity_problems.append("search choice differs from executed public identity")
        evidence = _field(search_event.get("native_backend_evidence"))
        if evidence.get("simulations") != 192 or evidence.get("particle_count") != 32:
            integrity_problems.append("shared-public search evidence differs from fixed B=192/32 budget")

    policy_events = [event for event in events if event.get("policy_worker_invoked") is True]
    policy_process_isolated = bool(policy_events) and all(
        event.get("simulator_extension_importable_in_policy_process") is False
        for event in policy_events
    )
    if policy_events and not policy_process_isolated:
        integrity_problems.append("policy worker imported the native simulator")

    blocker = run.problems[0] if run.problems else None
    if blocker is None and integrity_problems:
        blocker = (
            "FIREWALL_OR_MAPPING_FAILURE screen=(run) field=execution_integrity "
            f"source=STSRL controlled-run/native public adapter reason={' ; '.join(integrity_problems)}"
        )
    if blocker is None and run_wall_seconds >= MAX_RUN_SECONDS:
        blocker = (
            f"WALL_BUDGET: run reached {MAX_RUN_SECONDS:.0f}s "
            "before another controller boundary"
        )
    native_failures = [
        str(event.get("native_backend_failure", ""))
        for event in events
        if event.get("native_backend_mode") == "search"
    ]
    native_search_attempts = [
        event for event in events if event.get("native_backend_mode") == "search"
    ]
    native_search_event = native_search_attempts[0] if native_search_attempts else {}
    native_failure_text = "\n".join(native_failures)
    invariance_rows = [
        _field(event.get("native_anchor_invariance"))
        for event in events
        if event.get("selected_action_mode") == "shared_public_tree_B192"
    ]
    stop_classification = _stop_classification(blocker, bool(run.terminal), len(run.steps))
    stop_event = next(
        (
            event for event in reversed(events)
            if event.get("controller_decision_completed") is False
        ),
        None,
    )
    stop_boundary = (
        stop_event.get("public_boundary")
        if stop_event is not None
        else _public_boundary_evidence(run.public_run_context)
        if run.public_run_context
        else None
    )
    if not events and run.public_run_context:
        stop_boundary = _public_boundary_evidence(run.public_run_context)
    boundary_screen = _value(_field(_field(stop_boundary).get("screen_identity")))
    stop_screen = str(
        (stop_event or {}).get("screen")
        or boundary_screen
        or (run.steps[-1].next_screen_state if run.steps else "(unknown)")
    )
    stop_detail = blocker
    if stop_classification == "PUBLIC_BATTLE_PROPOSAL_GAP" and blocker:
        if blocker.startswith("PUBLIC_BATTLE_PROPOSAL_GAP"):
            stop_detail = blocker
        elif "ANCHOR_INDEPENDENCE_UNSUPPORTED:" in blocker:
            reason = blocker.split("ANCHOR_INDEPENDENCE_UNSUPPORTED:", 1)[1].split(";", 1)[0].strip()
            stop_detail = (
                "PUBLIC_BATTLE_PROPOSAL_GAP screen=BATTLE field=draw_order_constraints "
                "source=StepSimulator::samplePublicConsistentHiddenFutureFromParticleSeed "
                f"reason={reason}"
            )
        else:
            stop_detail = (
                "PUBLIC_BATTLE_PROPOSAL_GAP screen=BATTLE field=proposal_support "
                f"source=native shared-public sampler reason={blocker}"
            )
    elif stop_classification == "FIXED_DECISION_BUDGET":
        stop_detail = (
            f"FIXED_DECISION_BUDGET screen={stop_screen} field=controller_decisions "
            f"source=Issue 25 per-seed budget value={MAX_DECISIONS}"
        )
    elif stop_classification == "WALL_BUDGET":
        stop_detail = (
            f"WALL_BUDGET screen={stop_screen} field=elapsed_seconds "
            f"source=study monotonic clock limit={MAX_RUN_SECONDS:.0f} "
            f"observed={run_wall_seconds:.3f}"
        )
    elif stop_classification == "NATIVE_TERMINAL":
        stop_detail = (
            f"NATIVE_TERMINAL screen={stop_screen} field=outcome "
            f"source=execute_controlled_run value={run.outcome}"
        )
    elif stop_classification == "FIREWALL_OR_MAPPING_FAILURE" and blocker and not blocker.startswith(
        "FIREWALL_OR_MAPPING_FAILURE"
    ):
        stop_detail = (
            f"FIREWALL_OR_MAPPING_FAILURE screen={stop_screen} "
            "field=ordered_public_identity_or_execution "
            f"source=V3LightSpeedAdapter/execute_controlled_run reason={blocker}"
        )

    outcomes: list[str] = []
    if search_events:
        outcomes.append("NATURAL_PUBLIC_SEARCH_EXECUTED")
    outcomes.append(stop_classification)

    return {
        "seed": game_seed,
        "ascension": ASCENSION,
        "player_class": "IRONCLAD",
        "driver_seed": driver_seed,
        "steps_executed": len(run.steps),
        "controller_decisions_by_mode": decisions_by_mode,
        "screens_with_policy_decisions": decisions_by_screen,
        "decision_screen_coverage": [
            f"{screen}×{count}" for screen, count in decisions_by_screen.items()
        ],
        "run_wall_seconds": round(run_wall_seconds, 6),
        "first_battle_search_executed": bool(search_events),
        "first_battle_search_attempted": any(
            event.get("screen") == "BATTLE"
            and (
                event.get("native_backend_mode") == "search"
                or event.get("selected_action_mode") == "shared_public_tree_B192"
            )
            for event in events
        ),
        "first_battle_root_replay_validated": (
            native_search_event.get("native_root_replay_validated") is True
            or "PUBLIC_ROOT_REPLAY_VALIDATED" in native_failure_text
            or any(
                event.get("screen") == "BATTLE"
                and event.get("selected_action_mode") == "shared_public_tree_B192"
                for event in events
            )
        ),
        "first_battle_search_audit_completed": (
            "PUBLIC_SEARCH_AUDIT_COMPLETED" in native_failure_text
            or any(
                event.get("screen") == "BATTLE"
                and event.get("selected_action_mode") == "shared_public_tree_B192"
                for event in events
            )
        ),
        "private_anchor_counter_fault_changed_public_transition": (
            native_search_event.get("native_private_anchor_counter_fault_changed_public_transition") is True
            or "PRIVATE_ANCHOR_COUNTER_FAULT_CHANGED_PUBLIC_TRANSITION=true" in native_failure_text
            or any(row.get("private_counter_fault_control_passed") is True for row in invariance_rows)
        ),
        "public_sample_pool_equal": (
            native_search_event.get("native_public_sample_pool_equal")
            if native_search_event.get("native_public_sample_pool_equal") is not None
            else (
                any(row.get("public_sample_pool_equal_for_all_particles") is True for row in invariance_rows)
                if invariance_rows
                else None
            )
        ),
        "first_battle_root_candidate_count": native_search_event.get(
            "native_root_candidate_count",
            search_events[0].get("native_root_candidate_count") if search_events else None,
        ),
        "first_battle_public_baseline_ordinal": native_search_event.get(
            "native_baseline_public_ordinal",
            search_events[0].get("baseline_public_ordinal") if search_events else None,
        ),
        "first_battle_public_baseline_action": native_search_event.get(
            "native_baseline_public_action",
            search_events[0].get("baseline_public_action") if search_events else None,
        ),
        "first_battle_search_audit": native_search_event.get("native_search_audit"),
        "first_battle_search": search_events[0] if search_events else None,
        "first_meaningful_blocker": stop_detail,
        "raw_stop_detail": blocker,
        "stop_classification": stop_classification,
        "stop_disposition": stop_classification,
        "stop_boundary": stop_boundary,
        "outcomes": list(dict.fromkeys(outcomes)),
        "terminal": bool(run.terminal),
        "native_outcome": str(run.outcome),
        "steps": step_rows,
        "decisions": events,
        "native_action_mapping_count": len(adapter.mapping_trace),
        "all_selected_identities_executed": (
            len(adapter.mapping_trace) == len(run.steps)
            and all(item.get("executed_legal_identity_matches") is True for item in adapter.mapping_trace)
        ),
        "policy_process_isolated_from_simulator_extension": (
            policy_process_isolated if policy_events else None
        ),
        "policy_worker_decision_count": len(policy_events),
        "public_history_count": len(run.public_history),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--st-srl-root", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, default=Path("build-issue25"))
    parser.add_argument("--native-search", type=Path, default=Path("build-issue25/study-issue24-natural-search"))
    parser.add_argument("--output", type=Path, default=Path("studies/issue25/result.json"))
    args = parser.parse_args()
    st_srl_root = args.st_srl_root.resolve()
    build_dir = args.build_dir.resolve()
    native_search = args.native_search.resolve()
    output_path = args.output.resolve()
    journal = RunJournal(output_path.with_name(output_path.stem + ".progress.json"))

    native_revision = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        check=True,
        capture_output=True,
        text=True,
        env=_native_git_env(),
    ).stdout.strip()
    native_base_is_ancestor = subprocess.run(
        ["git", "merge-base", "--is-ancestor", NATIVE_BASE, native_revision],
        check=False,
        env=_native_git_env(),
    )
    if native_base_is_ancestor.returncode != 0:
        raise RuntimeError(
            f"native source mismatch: {native_revision} is not based on {NATIVE_BASE}"
        )
    issue24_source_is_ancestor = subprocess.run(
        ["git", "merge-base", "--is-ancestor", ISSUE24_SOURCE, native_revision],
        check=False,
        env=_native_git_env(),
    )
    if issue24_source_is_ancestor.returncode != 0:
        raise RuntimeError(
            f"native source mismatch: {native_revision} does not include reviewed source {ISSUE24_SOURCE}"
        )
    legacy_revision = _legacy_revision(st_srl_root)
    if legacy_revision != LEGACY_BASE:
        raise RuntimeError(f"legacy source mismatch: expected {LEGACY_BASE}, found {legacy_revision}")
    _require_clean_legacy_worktree(st_srl_root)
    if not native_search.is_file():
        raise FileNotFoundError(f"native search backend not found: {native_search}")
    journal.checkpoint(
        "source_revisions_validated",
        native_base=NATIVE_BASE,
        issue24_source=ISSUE24_SOURCE,
        native_source=native_revision,
        legacy_base=legacy_revision,
        legacy_worktree_clean=True,
        seeds=list(GAME_SEEDS),
        max_decisions_per_run=MAX_DECISIONS,
        max_wall_seconds_per_run=MAX_RUN_SECONDS,
    )

    sys.path.insert(0, str(build_dir))
    sys.path.insert(0, str(st_srl_root / "src"))
    import slaythespire as sts
    from sts_combat_rl.sim.action_space import ActionSpaceConfig
    from sts_combat_rl.sim.controller_contract import ControllerProvenance
    from sts_combat_rl.sim.controlled_run import execute_controlled_run
    from sts_combat_rl.sim.lightspeed import LightSpeedAdapter
    import sts_combat_rl.sim.controlled_run as controlled_run

    coverage_fixture = verify_unsupported_screen_fixture(
        ControllerProvenance(
            kind="study_fixture",
            name="issue25_unsupported_screen_fail_closed",
            config={},
        ),
        native_search,
    )
    journal.checkpoint("unsupported_screen_fixture_passed", fixture=coverage_fixture)

    def read_v3_projection(adapter: Any, snapshot: Any) -> dict[str, Any]:
        return adapter.public_projection(snapshot)

    def build_v3_context(
        raw_snapshot: object,
        actions: Sequence[Any],
        projection: Mapping[str, Any] | None,
        history: Sequence[Mapping[str, Any]] = (),
        include_candidates: bool = True,
    ) -> dict[str, Any]:
        del raw_snapshot
        if projection is None:
            raise ValueError("V3_ADAPTER_GAP: native v3 projection is unavailable")
        envelope = _plain(projection)
        native_projection = _field(envelope.get("native_public_projection"))
        if native_projection.get("schema_id") != NATIVE_SCHEMA:
            raise ValueError("V3_ADAPTER_GAP: expected native-public-projection-v3")
        if include_candidates:
            _assert_candidate_parity(native_projection, actions)
        return _public_run_context(envelope, history, include_candidates=include_candidates)

    def build_v3_decision_context(
        raw_snapshot: object,
        actions: Sequence[Any],
        active_action_space: Any,
        *,
        public_run_context: Mapping[str, Any] | None = None,
    ) -> Any:
        del raw_snapshot
        context = _field(public_run_context)
        if coverage_blocker(context) is not None:
            projection = _field(context.get("native_public_projection"))
            screen = str(_value(projection.get("screen_identity")) or "(unknown)")
            return SimpleNamespace(
                screen_state=screen,
                public_run_context=dict(context),
            )
        return _decision_context(_field(public_run_context), active_action_space)

    controlled_run.read_native_public_projection = read_v3_projection
    controlled_run.build_public_run_context = build_v3_context
    controlled_run.build_decision_context = build_v3_decision_context

    action_space = ActionSpaceConfig.include_all()
    run_summaries: list[dict[str, Any]] = []
    for game_seed in GAME_SEEDS:
        # Preserve the exact seeded ExpertNonCombatDriver configuration from
        # Issue #22 so both natural runs follow the cited four-decision path.
        driver_seed = DRIVER_SEED
        journal.checkpoint("seed_run_starting", seed=game_seed, driver_seed=driver_seed)
        worker = PolicyWorker(
            Path(__file__).with_name("policy_worker.py"),
            st_srl_root,
            driver_seed,
        )
        adapter = V3LightSpeedAdapter(
            LightSpeedAdapter(
                seed=game_seed,
                ascension=ASCENSION,
                player_class="IRONCLAD",
                module=sts,
            )
        )
        provenance = ControllerProvenance(
            kind="study_public_firewall",
            name="issue25_natural_shared_public_search_coverage_frontier",
            config={
                "study_issue": 25,
                "native_projection_schema": NATIVE_SCHEMA,
                "native_base_commit": NATIVE_BASE,
                "issue24_source_commit": ISSUE24_SOURCE,
                "native_source_commit": native_revision,
                "legacy_executor": "execute_controlled_run",
                "legacy_commit": LEGACY_BASE,
                "public_adapter_source_commit": PUBLIC_ADAPTER_SOURCE,
                "public_tactical_baseline_source_commit": BASELINE_SOURCE,
                "policy_input": "canonical_json_bytes_v1",
                "policy_process_isolated": True,
                "simulator_seed_sent_to_policy_worker": False,
                "driver_seed": driver_seed,
                "search_sampler_seed": "0x6e6f726d70756231",
                "search_tree_seed": "0x7368617265647472",
                "search_budget": 192,
                "search_particles": 32,
                "max_decisions_per_run": MAX_DECISIONS,
                "max_wall_seconds_per_run": MAX_RUN_SECONDS,
                "information_regime": "normal_public",
            },
        )
        controller = FirewalledController(
            worker,
            provenance,
            journal=journal,
            run_label=f"seed{game_seed}",
            native_search=native_search,
            ascension=ASCENSION,
        )

        def after_transition(step: Any) -> None:
            mapping = adapter.mapping_trace[-1] if adapter.mapping_trace else {}
            elapsed = time.monotonic() - controller.run_started
            journal.checkpoint(
                "controlled_action_executed",
                seed=game_seed,
                step_index=int(step.step_index),
                screen=str(step.screen_state),
                next_screen=str(step.next_screen_state),
                executed_legal_identity_matches=mapping.get("executed_legal_identity_matches"),
                run_wall_seconds=elapsed,
            )
            if elapsed >= MAX_RUN_SECONDS:
                blocker = (
                    f"WALL_BUDGET: run reached {MAX_RUN_SECONDS:.0f}s after "
                    f"controlled decision {int(step.step_index)}"
                )
                unavailable = "wall budget stopped before the next public projection"
                controller.events.append(
                    {
                        "step_index": int(step.step_index) + 1,
                        "screen": str(step.next_screen_state),
                        "coverage_status": "wall_budget",
                        "stop_classification": "WALL_BUDGET",
                        "controller_decision_completed": False,
                        "policy_callback_invoked": False,
                        "native_execution_parity": "not_attempted_at_next_boundary",
                        "blocker": blocker,
                        "public_boundary": {
                            "screen_identity": _available(
                                str(step.next_screen_state),
                                "ControlledRunStep.next_screen_state",
                            ),
                            "ordered_public_legal_identities": None,
                            "candidate_actions_availability": _unavailable(unavailable),
                            "screen_payload": _unavailable(unavailable),
                            "act": _unavailable(unavailable),
                            "floor": _unavailable(unavailable),
                            "encounter": _unavailable(unavailable),
                            "battle_turn": _unavailable(unavailable),
                            "public_resources": {
                                name: _unavailable(unavailable)
                                for name in (
                                    "current_hp",
                                    "max_hp",
                                    "gold",
                                    "potion_count",
                                    "potion_capacity",
                                )
                            },
                        },
                    }
                )
                journal.checkpoint(
                    "wall_budget_reached",
                    seed=game_seed,
                    step_index=int(step.step_index) + 1,
                    run_wall_seconds=elapsed,
                    blocker=blocker,
                )
                raise ValueError(blocker)
        try:
            run = execute_controlled_run(
                adapter,
                controller,
                seed=game_seed,
                max_steps=MAX_DECISIONS,
                action_space=action_space,
                after_transition=after_transition,
            )
            run_wall_seconds = max(0.0, time.monotonic() - controller.run_started)
            summary = _summarize_run(
                run,
                controller,
                adapter,
                game_seed=game_seed,
                driver_seed=driver_seed,
                run_wall_seconds=run_wall_seconds,
            )
            if controller.last_public_selection is not None:
                stale_input, stale_candidates, stale_index = controller.last_public_selection
                try:
                    adapter.arm_public_selection(stale_input, stale_candidates, stale_index)
                except V3FirewallError as exc:
                    summary["stale_cross_state_selection_rejected"] = "STALE_PUBLIC_ACTION" in str(exc)
                    if not summary["stale_cross_state_selection_rejected"]:
                        raise AssertionError(f"unexpected stale-action guard result: {exc}") from exc
                else:
                    raise AssertionError(f"seed {game_seed} accepted an action from an earlier state")
            else:
                summary["stale_cross_state_selection_rejected"] = False
            run_summaries.append(summary)
            journal.checkpoint(
                "seed_run_complete",
                seed=game_seed,
                executed_steps=summary["steps_executed"],
                first_battle_search_executed=summary["first_battle_search_executed"],
                stop_disposition=summary["stop_disposition"],
            )
        finally:
            adapter.close()
            worker.close()

    outcomes = list(dict.fromkeys(
        outcome for summary in run_summaries for outcome in summary["outcomes"]
    ))
    limitations = [
        "Two fixed A20 Ironclad seeds only; no held-out seeds or win-rate/continuation-strength claim.",
        "Only the first actual Battle decision uses B=192 shared-public search; later Battle decisions use the named Issue 17 public tactical heuristic.",
        "The result is a two-seed reachability and coverage diagnostic; it does not establish broad A20 performance or a general posterior model.",
        "Unsupported Runic Dome intents, Darkling private damage, and unrepresented Hexaghost move-cycle state remain fail-closed.",
        "Insertion states with known top prefixes, baseline anchors, before-anchor relations, or unrepresented draw-card runtime changes remain fail-closed.",
        "The 180-second guard bounds policy-worker and native-search subprocess calls and stops at the next controlled transition boundary; a synchronous simulator transition cannot be interrupted mid-call.",
    ]
    report = {
        "schema_id": "issue25-public-run-coverage-frontier-v1",
        "disposition": "STUDY_ONLY",
        "coverage_fixture": coverage_fixture,
        "source_revisions": {
            "native_base": NATIVE_BASE,
            "issue24_source": ISSUE24_SOURCE,
            "native": native_revision,
            "legacy_stsrl": LEGACY_BASE,
            "public_input_adapter": PUBLIC_ADAPTER_SOURCE,
            "public_tactical_baseline": BASELINE_SOURCE,
        },
        "seeds": list(GAME_SEEDS),
        "ascension": ASCENSION,
        "player_class": "IRONCLAD",
        "fixed_budget": {
            "max_decisions_per_run": MAX_DECISIONS,
            "max_wall_seconds_per_run": MAX_RUN_SECONDS,
            "native_decision_timeout_seconds": NATIVE_DECISION_TIMEOUT,
            "search_simulations_per_actual_first_battle_decision": 192,
            "particle_count": 32,
            "anchor_invariance_audit_simulations": 192,
            "driver_seed": DRIVER_SEED,
            "sampler_seed": "0x6e6f726d70756231",
            "search_seed": "0x7368617265647472",
        },
        "controlled_run_executor": "sts_combat_rl.sim.controlled_run.execute_controlled_run",
        "adapter": "sts_combat_rl.sim.lightspeed.LightSpeedAdapter with native-public-projection-v3 study firewall",
        "policy_information_regime": "normal_public",
        "proposal_law": "Q(particle | current_public_observation, observed_public_history, sampler_seed, particle_index)",
        "proposal_semantics": "reproducible public-consistent proposal; not an exact posterior",
        "proposal_is_exact_posterior": False,
        "policy_process_isolation": {
            "noncombat_policy": "isolated Python process receiving immutable JSON bytes only",
            "native_search": "separate trusted backend receives public JSON bytes plus fixed run seed for root replay",
            "simulator_extension_imported_by_policy_worker": False,
        },
        "outcomes": outcomes,
        "proposed_follow_up": (
            "Keep this implementation and its artifacts study-only pending independent review. "
            "Any reusable core capability requires a separate promotion decision."
        ),
        "proposed_code_disposition": "study_only_pending_independent_review",
        "seed_runs": run_summaries,
        "limitations": limitations,
        "retention": {"downstream_consumer": "Issue #25 independent Reviewer"},
    }
    _atomic_json(output_path, report)
    markdown = [
        "# Issue 25: public-only run coverage frontier",
        "",
        f"Disposition: **STUDY_ONLY**. Outcomes: `{', '.join(outcomes)}`.",
        "",
        f"Provenance: reviewed Issue 24 source `{ISSUE24_SOURCE}`; native source `{native_revision}` (base `{NATIVE_BASE}`); STSRL executor `{LEGACY_BASE}`; public adapter `{PUBLIC_ADAPTER_SOURCE}`; tactical baseline `{BASELINE_SOURCE}`.",
        "",
        "| Seed | Decisions | Screens | Controller modes | Wall seconds | Stop classification | First blocker |",
        "|---:|---:|---|---|---:|---|---|",
    ]
    for summary in run_summaries:
        screens = ", ".join(summary["decision_screen_coverage"])
        modes = ", ".join(
            f"{mode}×{count}"
            for mode, count in summary["controller_decisions_by_mode"].items()
        )
        markdown.append(
            f"| {summary['seed']} | {summary['steps_executed']} | {screens} | {modes} | "
            f"{summary['run_wall_seconds']:.3f} | `{summary['stop_classification']}` | "
            f"{summary['first_meaningful_blocker'] or 'declared limit/terminal'} |"
        )
    markdown.extend(
        [
            "",
            f"Each fixed seed is capped at {MAX_DECISIONS} controller decisions and {MAX_RUN_SECONDS:.0f} seconds. The unsupported Shop fixture passed with no policy callback or selected action; it is excluded from natural-run progress. Search counts as executed only if native root replay, the 32-particle public-consistent sampler, anchor invariance, and fixed B=192 search all complete.",
            "",
            "The sampler law is a public-consistent proposal Q, not an exact posterior. This study stays disposable until independent review; any core promotion is a separate decision.",
            "",
            "See `result.json` for the per-decision public identity/resource trace, fidelity and availability provenance, cumulative grounded work, and exact stop boundary. `result.progress.json` records the last durable execution boundary.",
            "",
        ]
    )
    output_path.with_suffix(".md").write_text("\n".join(markdown), encoding="utf-8")
    journal.complete()
    print(json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
