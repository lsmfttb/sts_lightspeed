"""Run the bounded Issue 21 native-v3 public-policy firewall study."""

from __future__ import annotations

import argparse
import atexit
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
from typing import Any, Mapping, Sequence


ISSUE21_SCHEMA = "issue21-v3-public-policy-input-v1"
NATIVE_SCHEMA = "native-public-projection-v3"
NATIVE_BASE = "a655dbb264b2274c54acf0426cd5df19a8c8ec96"
LEGACY_BASE = "3037b75eca4bd73fa70d018ffd4442a1f2d65628"
DRIVER_SEED = 21021
GAME_SEED = 49
ASCENSION = 20


def canonical_bytes(value: object) -> bytes:
    return json.dumps(
        value,
        ensure_ascii=False,
        allow_nan=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")


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
    if "bits=" in label:
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
    if canonical_bytes(public_rows) != canonical_bytes(adapted_rows):
        raise ValueError("V3_ADAPTER_GAP: native candidate identity/order differs from adapter")


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
    screen = str(_value(projection.get("screen_identity")) or "(unknown)")
    boss = _field(projection.get("visible_act_boss"))
    if screen == "MAP_SCREEN" and boss.get("availability") != "available":
        reason = str(boss.get("reason", "visible act boss identity unavailable"))
        return (
            "PUBLIC_SCREEN_COVERAGE_GAP screen=MAP_SCREEN "
            f"field=visible_act_boss availability={boss.get('availability')} reason={reason}"
        )

    payload_field = _field(projection.get("screen_payload"))
    if payload_field.get("availability") != "available":
        return (
            f"PUBLIC_SCREEN_COVERAGE_GAP screen={screen} field=screen_payload "
            f"availability={payload_field.get('availability')} "
            f"reason={payload_field.get('reason', 'payload unavailable')}"
        )
    payload = _field(payload_field.get("value"))
    status = payload.get("coverage_status")
    if status != "supported":
        return (
            f"PUBLIC_SCREEN_COVERAGE_GAP screen={screen} "
            f"field=screen_payload.coverage_status value={status} "
            f"reason={payload.get('reason', 'screen coverage is not complete')}"
        )
    candidates = _field(projection.get("candidate_actions"))
    if candidates.get("availability") != "available":
        return (
            f"PUBLIC_SCREEN_COVERAGE_GAP screen={screen} field=candidate_actions "
            f"availability={candidates.get('availability')} "
            f"reason={candidates.get('reason', 'ordered public candidates unavailable')}"
        )
    if screen == "BATTLE":
        battle = _field(public_context.get("native_public_battle_observation"))
        if battle.get("information_regime") != "normal_public":
            return "PUBLIC_SCREEN_COVERAGE_GAP screen=BATTLE field=information_regime"
        if battle.get("information_fidelity") != "supported":
            return (
                "PUBLIC_SCREEN_COVERAGE_GAP screen=BATTLE field=information_fidelity "
                f"value={battle.get('information_fidelity')}"
            )
    return None


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
        "schema_id": "issue21-v3-public-run-context-v1",
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
        tactical_feature_schema_id="issue21-v3-public-derived-v1",
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
        "schema_id": ISSUE21_SCHEMA,
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
            "schema_id": "issue21-run-progress-v1",
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

    def __init__(self, base_adapter: Any, *, mechanics_fixture: bool = False) -> None:
        self._base = base_adapter
        self._sim = base_adapter._sim
        self._mechanics_fixture = mechanics_fixture
        self._pending: dict[str, Any] | None = None
        self.mapping_trace: list[dict[str, Any]] = []
        self.mechanics_prelude_steps = 0

    def __getattr__(self, name: str) -> Any:
        return getattr(self._base, name)

    def reset(self, seed: int | None = None) -> Any:
        snapshot = self._base.reset(seed)
        self._pending = None
        self.mapping_trace = []
        self.mechanics_prelude_steps = 0
        if not self._mechanics_fixture:
            return snapshot
        for _ in range(512):
            projection = dict(self._sim.public_projection())
            raw = dict(self._sim.snapshot())
            if (
                projection.get("screen_identity", {}).get("value") == "BATTLE"
                and raw.get("battle_active") is True
                and raw.get("battle_input_state") == "PLAYER_NORMAL"
            ):
                return self._base._snapshot(raw)
            candidates = _candidate_rows(projection)
            actions = self._base.legal_actions(snapshot)
            _assert_candidate_parity(projection, actions)
            if not candidates or not actions:
                raise V3FirewallError("mechanics-only fixture found no legal setup action")
            # This is fixture construction, not a policy decision. It is kept
            # separate from the ordinary-seed run and labeled in all output.
            transition = self._base.step(actions[0])
            snapshot = transition.snapshot
            self.mechanics_prelude_steps += 1
        raise V3FirewallError("mechanics-only fixture did not reach a Battle root")

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

    def choose(self, public_input: bytes) -> dict[str, Any]:
        if self.process.poll() is not None:
            detail = self.process.stderr.read().decode("utf-8", errors="replace")
            raise RuntimeError(f"public policy worker exited: {detail[-400:]}")
        assert self.process.stdin is not None and self.process.stdout is not None
        self.last_public_input = bytes(public_input)
        self.process.stdin.write(public_input + b"\n")
        self.process.stdin.flush()
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
    ):
        self.worker = worker
        self.provenance = controller_provenance
        self.arm_selection = arm_selection
        self.journal = journal
        self.run_label = run_label
        self.events: list[dict[str, Any]] = []

    def reset_for_run(self, _simulator_seed: int | None) -> None:
        # The game seed is intentionally not forwarded to the policy process.
        return None

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
                    "policy_callback_invoked": False,
                    "blocker": blocker,
                }
            )
            raise ValueError(blocker)

        public_input = _encode_input(context)
        response = self.worker.choose(public_input)
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
            arm(public_input, candidates, selected_index)
        event = {
            "step_index": step_index,
            "screen": screen,
            "coverage_status": "supported",
            "policy_callback_invoked": True,
            "policy_role": response.get("policy_role"),
            "policy_worker_pid": response.get("worker_pid"),
            "policy_input_type": response.get("input_type"),
            "policy_input_sha256": hashlib.sha256(public_input).hexdigest(),
            "simulator_extension_importable_in_policy_process": response.get(
                "simulator_extension_importable"
            ),
            "selected_public_ordinal": selected_index,
            "selected_public_identity": candidates[selected_index],
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
            )
        return ControllerDecision(
            selected_index=selected_index,
            provenance=self.provenance,
            reason=str(response.get("reason", "public policy callback")),
            metadata={"issue21_policy_role": response.get("policy_role")},
        )


class PoisonPrivilegedInput:
    def __getattribute__(self, name: str) -> Any:
        if name.startswith("__"):
            return object.__getattribute__(self, name)
        raise AssertionError(f"policy wrapper accessed poisoned privileged input: {name}")

    def __iter__(self):
        raise AssertionError("policy wrapper iterated poisoned privileged input")


def _same_private_free_public_context_from_witness(
    projection: Mapping[str, Any],
    battle: Mapping[str, Any],
    history: Sequence[Mapping[str, Any]],
    action_space: Any,
) -> Any:
    envelope = {
        "native_public_projection": _plain(projection),
        "native_public_battle_observation": _plain(battle),
    }
    public_context = _public_run_context(envelope, history, include_candidates=True)
    return _decision_context(public_context, action_space)


def _verify_witness(native_witness: Mapping[str, Any], action_space: Any) -> dict[str, Any]:
    witnesses = native_witness.get("policy_input_witnesses")
    if not isinstance(witnesses, list) or len(witnesses) != 2:
        raise AssertionError("native hidden-future witness must contain two samples")
    if native_witness.get("native_sampled_hidden_futures_differ") is not True:
        raise AssertionError("native sampler did not prove two distinct hidden futures")
    if native_witness.get("native_v3_projection_equal_across_hidden_futures") is not True:
        raise AssertionError("native v3 projection changed across hidden futures")
    if native_witness.get("native_battle_observation_equal_across_hidden_futures") is not True:
        raise AssertionError("native Battle public observation changed across hidden futures")
    payloads = []
    for witness in witnesses:
        projection = witness["native_public_projection"]
        battle = witness["native_public_battle_observation"]
        history = witness["public_history"]
        context = _same_private_free_public_context_from_witness(
            projection, battle, history, action_space
        )
        payloads.append(_encode_input(context))
    if payloads[0] != payloads[1]:
        raise AssertionError("serialized policy input changed across hidden futures")
    document = json.loads(payloads[0])
    if document.get("public_history") != witnesses[0].get("public_history"):
        raise AssertionError("append-only public history was not preserved in policy input")
    return {
        "distinct_private_futures": True,
        "native_v3_projection_equal": True,
        "native_battle_observation_equal": True,
        "serialized_policy_input_equal": True,
        "public_history_entries": len(document["public_history"]),
        "policy_input_sha256": hashlib.sha256(payloads[0]).hexdigest(),
        "policy_input_bytes": len(payloads[0]),
        "candidate_count": len(document["ordered_public_legal_candidates"]),
        "derived_feature_count": len(document["derived_features"]),
    }


def _verify_poisoned_inputs(
    adapter: V3LightSpeedAdapter,
    snapshot: Any,
    actions: Sequence[Any],
    context: Any,
    worker: PolicyWorker,
    provenance: Any,
) -> dict[str, Any]:
    controller = FirewalledController(worker, provenance, arm_selection=False)
    decision = controller.select_action(
        PoisonPrivilegedInput(),
        PoisonPrivilegedInput(),
        PoisonPrivilegedInput(),
        context,
        0,
    )
    if decision.selected_index not in context.eligible_action_indices:
        raise AssertionError("poisoned-input policy returned an invalid public ordinal")
    if not actions or not isinstance(adapter, V3LightSpeedAdapter) or snapshot is None:
        raise AssertionError("poisoned-input fixture was not grounded in native v3 state")
    event = controller.events[-1]
    if event.get("simulator_extension_importable_in_policy_process") is not False:
        raise AssertionError("native simulator module was importable in policy process")
    if event.get("policy_input_type") != "bytes":
        raise AssertionError("policy callback did not receive immutable serialized bytes")
    return {
        "poisoned_adapter_snapshot_actions_not_read": True,
        "callback_argument_type": event["policy_input_type"],
        "worker_pid": event["policy_worker_pid"],
        "simulator_extension_importable": False,
        "selected_public_ordinal": decision.selected_index,
    }


def _assert_stale_selection_rejected(
    adapter: V3LightSpeedAdapter,
    old_public_input: bytes,
    old_candidates: Sequence[Mapping[str, Any]],
    selected_index: int,
) -> None:
    try:
        adapter.arm_public_selection(old_public_input, old_candidates, selected_index)
    except V3FirewallError as exc:
        if "STALE_PUBLIC_ACTION" not in str(exc):
            raise
        return
    raise AssertionError("cross-state stale public action was accepted")


def _legacy_revision(st_srl_root: Path) -> str:
    result = subprocess.run(
        ["git", "-C", str(st_srl_root), "rev-parse", "HEAD"],
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout.strip()


def _require_clean_legacy_worktree(st_srl_root: Path) -> None:
    result = subprocess.run(
        ["git", "-C", str(st_srl_root), "status", "--porcelain", "--untracked-files=all"],
        check=True,
        capture_output=True,
        text=True,
    )
    if result.stdout.strip():
        raise RuntimeError("legacy STSRL source worktree must be clean at the pinned revision")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--st-srl-root", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, default=Path("build-py"))
    parser.add_argument(
        "--native-witness",
        type=Path,
        default=Path("studies/issue21/native-witness.json"),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("studies/issue21/result.json"),
    )
    args = parser.parse_args()
    st_srl_root = args.st_srl_root.resolve()
    build_dir = args.build_dir.resolve()
    witness_path = args.native_witness.resolve()
    output_path = args.output.resolve()
    journal = RunJournal(output_path.with_name(output_path.stem + ".progress.json"))
    if _legacy_revision(st_srl_root) != LEGACY_BASE:
        raise RuntimeError(
            f"legacy source lineage mismatch: expected {LEGACY_BASE}, "
            f"found {_legacy_revision(st_srl_root)}"
        )
    _require_clean_legacy_worktree(st_srl_root)
    journal.checkpoint(
        "source_revisions_validated",
        native_base=NATIVE_BASE,
        legacy_base=LEGACY_BASE,
        legacy_worktree_clean=True,
    )
    sys.path.insert(0, str(build_dir))
    sys.path.insert(0, str(st_srl_root / "src"))

    import slaythespire as sts
    from sts_combat_rl.sim.action_space import ActionSpaceConfig
    from sts_combat_rl.sim.controller_contract import ControllerProvenance
    from sts_combat_rl.sim.controlled_run import execute_controlled_run
    from sts_combat_rl.sim.lightspeed import LightSpeedAdapter
    import sts_combat_rl.sim.controlled_run as controlled_run

    action_space = ActionSpaceConfig.initial_no_potions()
    native_witness = json.loads(witness_path.read_text(encoding="utf-8"))
    invariance = _verify_witness(native_witness, action_space)
    journal.checkpoint(
        "native_hidden_future_invariance_verified",
        distinct_hidden_futures=True,
        public_projection_equal=True,
        policy_input_equal=True,
    )

    # Replace only the old v1 projection/context translators in the imported
    # legacy executor. The controlled-run loop, lifecycle, history append,
    # selected-index validation, and simulator step remain the actual STSRL code.
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
        return _decision_context(context, active_action_space)

    controlled_run.read_native_public_projection = read_v3_projection
    controlled_run.build_public_run_context = build_v3_context
    controlled_run.build_decision_context = build_v3_decision_context

    worker = PolicyWorker(
        Path(__file__).with_name("policy_worker.py"),
        st_srl_root,
        DRIVER_SEED,
    )
    provenance = ControllerProvenance(
        kind="study_public_firewall",
        name="issue21_v3_public_policy_process",
        config={
            "native_projection_schema": NATIVE_SCHEMA,
            "legacy_executor": "execute_controlled_run",
            "legacy_commit": LEGACY_BASE,
            "policy_input": "canonical_json_bytes_v1",
            "policy_process_isolated": True,
            "simulator_seed_sent_to_policy": False,
            "driver_seed": DRIVER_SEED,
            "information_regime": "normal_public",
        },
    )

    ordinary_adapter = V3LightSpeedAdapter(
        LightSpeedAdapter(
            seed=GAME_SEED,
            ascension=ASCENSION,
            player_class="IRONCLAD",
            module=sts,
        )
    )
    ordinary_controller = FirewalledController(
        worker, provenance, journal=journal, run_label="seed49"
    )
    try:
        ordinary_run = execute_controlled_run(
            ordinary_adapter,
            ordinary_controller,
            seed=GAME_SEED,
            max_steps=8,
            action_space=action_space,
        )
        journal.checkpoint(
            "seed49_controlled_run_reached_first_coverage_gap",
            executed_steps=len(ordinary_run.steps),
            policy_decisions=sum(
                event.get("policy_callback_invoked") is True
                for event in ordinary_controller.events
            ),
            blocker=ordinary_run.problems[0] if ordinary_run.problems else "none",
        )

        if (
            len(ordinary_run.steps) < 1
            or len(ordinary_controller.events) != len(ordinary_run.steps) + 1
            or ordinary_controller.events[-1].get("policy_callback_invoked") is not False
        ):
            raise AssertionError(
                "ordinary run did not stop at its first partial screen: "
                f"steps={len(ordinary_run.steps)} "
                f"events={ordinary_controller.events!r} "
                f"problems={ordinary_run.problems!r}"
            )
        if ordinary_run.steps[0].screen_state != "EVENT_SCREEN":
            raise AssertionError("seed 49 did not begin at the expected supported Event screen")
        first_policy_input = ordinary_controller.events[0]
        if first_policy_input.get("policy_role") != "expert_non_combat":
            raise AssertionError("real ExpertNonCombatDriver was not used for the supported Event")
        blocker = ordinary_run.problems[0] if ordinary_run.problems else ""
        if "screen=MAP_SCREEN" not in blocker or "field=visible_act_boss" not in blocker:
            raise AssertionError(f"ordinary run did not fail closed at the Map boss gap: {blocker}")
        if len(ordinary_adapter.mapping_trace) != len(ordinary_run.steps):
            raise AssertionError("supported decisions did not traverse native executor mapping")

        # The executor has advanced to Map. Reusing the Event policy bytes must
        # be rejected as stale before any native action can be stepped.
        stale_input = worker.last_public_input
        if not isinstance(stale_input, bytes):
            raise AssertionError("public policy worker did not retain its last public input for stale test")
        stale_doc = json.loads(stale_input)
        _assert_stale_selection_rejected(
            ordinary_adapter,
            stale_input,
            stale_doc["ordered_public_legal_candidates"],
            int(ordinary_controller.events[-2]["selected_public_ordinal"]),
        )

        seed50_worker = PolicyWorker(
            Path(__file__).with_name("policy_worker.py"),
            st_srl_root,
            DRIVER_SEED,
        )
        seed50_adapter = V3LightSpeedAdapter(
            LightSpeedAdapter(
                seed=50,
                ascension=ASCENSION,
                player_class="IRONCLAD",
                module=sts,
            )
        )
        try:
            seed50_controller = FirewalledController(
                seed50_worker,
                provenance,
                journal=journal,
                run_label="seed50",
            )
            seed50_run = execute_controlled_run(
                seed50_adapter,
                seed50_controller,
                seed=50,
                max_steps=8,
                action_space=action_space,
            )
            if (
                not seed50_run.steps
                or seed50_run.steps[0].screen_state != "EVENT_SCREEN"
                or len(seed50_controller.events) != len(seed50_run.steps) + 1
                or seed50_controller.events[-1].get("policy_callback_invoked") is not False
            ):
                raise AssertionError("seed 50 did not stop at its first partial screen")
            seed50_blocker = seed50_run.problems[0] if seed50_run.problems else ""
            if "screen=MAP_SCREEN" not in seed50_blocker or "field=visible_act_boss" not in seed50_blocker:
                raise AssertionError(f"seed 50 did not fail closed at the Map boss gap: {seed50_blocker}")
            if len(seed50_adapter.mapping_trace) != len(seed50_run.steps):
                raise AssertionError("seed 50 public decisions bypassed native action mapping")
            seed50_summary = {
                "steps_executed": len(seed50_run.steps),
                "screens_with_policy_decisions": [step.screen_state for step in seed50_run.steps],
                "first_fail_closed_boundary": seed50_blocker,
                "policy_decision_count": len(seed50_run.steps),
                "policy_input_sha256": seed50_controller.events[0]["policy_input_sha256"],
                "public_action_mapping_count": len(seed50_adapter.mapping_trace),
                "policy_decisions": [
                    {
                        key: event[key]
                        for key in (
                            "step_index",
                            "screen",
                            "policy_role",
                            "policy_input_sha256",
                            "selected_public_ordinal",
                            "selected_public_identity",
                        )
                    }
                    for event in seed50_controller.events
                    if event.get("policy_callback_invoked") is True
                ],
                "public_action_mappings": seed50_adapter.mapping_trace,
            }
            journal.checkpoint(
                "seed50_controlled_run_reached_first_coverage_gap",
                executed_steps=len(seed50_run.steps),
                policy_decisions=seed50_summary["policy_decision_count"],
                blocker=seed50_blocker,
            )
        finally:
            seed50_adapter.close()
            seed50_worker.close()

        # Build a real supported Battle context using only the separately named
        # mechanics fixture. It exists for action mapping and policy-capability
        # evidence; its four first-legal setup transitions are not policy work.
        fixture_adapter = V3LightSpeedAdapter(
            LightSpeedAdapter(
                seed=GAME_SEED,
                ascension=ASCENSION,
                player_class="IRONCLAD",
                module=sts,
            ),
            mechanics_fixture=True,
        )
        fixture_snapshot = fixture_adapter.reset(GAME_SEED)
        fixture_projection = fixture_adapter.public_projection(fixture_snapshot)
        fixture_actions = fixture_adapter.legal_actions(fixture_snapshot)
        fixture_public_context = build_v3_context(
            fixture_snapshot.raw,
            fixture_actions,
            fixture_projection,
            history=(),
            include_candidates=True,
        )
        fixture_context = build_v3_decision_context(
            fixture_snapshot.raw,
            fixture_actions,
            action_space,
            public_run_context=fixture_public_context,
        )
        poison_evidence = _verify_poisoned_inputs(
            fixture_adapter,
            fixture_snapshot,
            fixture_actions,
            fixture_context,
            worker,
            provenance,
        )

        # One actual controller choice flows through the same authoritative
        # execute_controlled_run lifecycle, selected-index validation, and step.
        fixture_controller = FirewalledController(
            worker, provenance, journal=journal, run_label="mechanics_fixture"
        )
        fixture_run = execute_controlled_run(
            fixture_adapter,
            fixture_controller,
            seed=GAME_SEED,
            max_steps=1,
            action_space=action_space,
        )
        if len(fixture_run.steps) != 1 or len(fixture_adapter.mapping_trace) != 1:
            raise AssertionError("Battle mechanics fixture did not execute one public decision")
        if fixture_adapter.mapping_trace[0]["executed_legal_identity_matches"] is not True:
            raise AssertionError("selected public identity did not map to executed legal action")
        if fixture_controller.events[0].get("policy_role") != "battle_input_probe":
            raise AssertionError("Battle fixture did not call the JSON-only public probe")
        journal.checkpoint(
            "mechanics_only_battle_mapping_and_search_verified",
            fixture_prelude_steps=fixture_adapter.mechanics_prelude_steps,
            controlled_decisions=len(fixture_run.steps),
            shared_public_search_simulations=native_witness["shared_public_search"]["simulations"],
        )

        # Mismatched public identity/order fails before stepping.
        current_native = dict(fixture_adapter._sim.public_projection())
        malformed_candidates = _candidate_rows(current_native)
        malformed_candidates[0] = dict(malformed_candidates[-1])
        try:
            fixture_adapter.arm_public_selection(
                canonical_bytes(
                    {
                        "native_public_projection": current_native,
                        "ordered_public_legal_candidates": _candidate_rows(current_native),
                    }
                ),
                malformed_candidates,
                0,
            )
        except V3FirewallError as exc:
            if "PUBLIC_ACTION_MISMATCH" not in str(exc):
                raise
        else:
            raise AssertionError("mismatched public candidate order was accepted")

        fixture_current_snapshot = fixture_adapter._base._snapshot(
            dict(fixture_adapter._sim.snapshot())
        )
        fixture_current_actions = fixture_adapter.legal_actions(fixture_current_snapshot)
        current_candidates = _candidate_rows(current_native)
        current_battle = dict(fixture_adapter._sim.public_battle_state())
        current_policy_input = canonical_bytes(
            {
                "native_public_projection": current_native,
                "native_public_battle_observation": current_battle,
                "ordered_public_legal_candidates": current_candidates,
            }
        )
        fixture_adapter.arm_public_selection(
            current_policy_input,
            current_candidates,
            0,
        )
        stale_native_action = fixture_current_actions[0]
        # A trusted backend transition occurs after arming. The adapter must
        # refuse to execute the previously selected native handle afterward.
        fixture_adapter._base.step(stale_native_action)
        try:
            fixture_adapter.step(stale_native_action)
        except V3FirewallError as exc:
            if "STALE_PUBLIC_ACTION" not in str(exc):
                raise
        else:
            raise AssertionError("state changed after arming but stale native action was executed")

        report = {
            "schema_id": "issue21-public-run-firewall-result-v1",
            "study_outcome": "PUBLIC_RUN_FIREWALL_ESTABLISHED",
            "disposition": "STUDY_ONLY",
            "native_base_commit": NATIVE_BASE,
            "legacy_base_commit": LEGACY_BASE,
            "source_reuse": {
                "executor": "sts_combat_rl.sim.controlled_run.execute_controlled_run",
                "adapter": "sts_combat_rl.sim.lightspeed.LightSpeedAdapter",
                "noncombat_policy": "sts_combat_rl.sim.non_combat_policy.ExpertNonCombatDriver",
                "battle_baseline_and_shared_tree": (
                    "Issue 17 study-only source at "
                    "9a2792e1e02157124b4f90edc91b7ad8765d5d10"
                ),
                "permanent_core_promotion": False,
            },
            "normal_public_seed": GAME_SEED,
            "normal_public_seeds": [GAME_SEED, 50],
            "ascension": ASCENSION,
            "driver_seed": DRIVER_SEED,
            "normal_public_run": {
                "steps_executed": len(ordinary_run.steps),
                "screens_with_policy_decisions": [step.screen_state for step in ordinary_run.steps],
                "supported_event_policy": first_policy_input["policy_role"],
                "policy_input_sha256": first_policy_input["policy_input_sha256"],
                "policy_process_pid": first_policy_input["policy_worker_pid"],
                "policy_process_has_simulator_import": first_policy_input[
                    "simulator_extension_importable_in_policy_process"
                ],
                "first_fail_closed_boundary": blocker,
                "map_projection_coverage_status": _field(
                    _field(dict(ordinary_adapter._sim.public_projection()).get("screen_payload")).get("value")
                ).get("coverage_status"),
                "public_action_mapping": ordinary_adapter.mapping_trace[0],
                "policy_decisions": [
                    {
                        key: event[key]
                        for key in (
                            "step_index",
                            "screen",
                            "policy_role",
                            "policy_input_sha256",
                            "selected_public_ordinal",
                            "selected_public_identity",
                        )
                    }
                    for event in ordinary_controller.events
                    if event.get("policy_callback_invoked") is True
                ],
                "public_action_mappings": ordinary_adapter.mapping_trace,
                "stale_previous_action_rejected_at_map": True,
                "supported_policy_decision_count": len(ordinary_run.steps),
            },
            "seed_50_normal_public_run": seed50_summary,
            "mechanics_only_fixture": {
                "prelude_steps": fixture_adapter.mechanics_prelude_steps,
                "prelude_is_not_policy_evidence": True,
                "controlled_run_steps": len(fixture_run.steps),
                "policy_role": fixture_controller.events[0]["policy_role"],
                "public_action_mapping": fixture_adapter.mapping_trace[0],
                "bounded_public_battle_comparison": {
                    "baseline": "Issue 17 public tactical heuristic",
                    "root_action_mapping": native_witness["root_action_mapping"],
                    "shared_public_search": native_witness["shared_public_search"],
                    "shared_public_search_wall_seconds": native_witness[
                        "shared_public_search_wall_seconds"
                    ],
                },
            },
            "screen_to_policy_input_provenance": [
                {
                    "screen": "EVENT_SCREEN",
                    "coverage": "supported",
                    "policy": "seeded ExpertNonCombatDriver",
                    "input": "canonical JSON bytes in isolated worker; native extension unavailable",
                    "seed_count": 2,
                    "decision_count": 2,
                },
                {
                    "screen": "REWARDS",
                    "coverage": "supported",
                    "policy": "seeded ExpertNonCombatDriver",
                    "input": "canonical JSON bytes in isolated worker; native extension unavailable",
                    "seed_count": 2,
                    "decision_count": 4,
                },
                {
                    "screen": "MAP_SCREEN",
                    "coverage": "partial",
                    "policy_invoked": False,
                    "first_missing_field": "visible_act_boss",
                },
                {
                    "screen": "BATTLE",
                    "coverage": "supported mechanics-only fixture",
                    "policy": "public-input probe for executor firewall; separate Issue 17 baseline and B=192 search evidence above",
                    "normal_seed_decisions": 0,
                },
            ],
            "ordinary_run_disposition_counts": {
                "coverage_stops": 2,
                "partial_screen_stops": 2,
                "unsupported_screen_stops": 0,
                "terminal_runs": 0,
                "timed_out_runs": 0,
            },
            "firewall_tests": {
                "hidden_future_input_invariance": invariance,
                "poisoned_privileged_arguments": poison_evidence,
                "mismatched_identity_rejected": True,
                "stale_cross_state_identity_rejected": True,
                "stale_after_arming_rejected": True,
                "selected_public_identity_equals_executed_legal_identity": True,
            },
            "limitations": [
                "ordinary A20 run stops at MAP_SCREEN because visible_act_boss is unavailable",
                "mechanics-only Battle fixture uses four first-legal setup transitions; these are not policy decisions",
                "no complete run, win-rate, or A20 strength claim",
            ],
        }
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_text(
            json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        journal.complete()
        print(json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True))
        print("ISSUE21_PUBLIC_RUN_FIREWALL_PASS")
        return 0
    finally:
        ordinary_adapter.close()
        worker.close()


if __name__ == "__main__":
    raise SystemExit(main())
