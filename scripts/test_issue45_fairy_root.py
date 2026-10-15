"""Replay the captured 27-action prefix, then test one root (no episode run)."""
import argparse
import json
from pathlib import Path

from test_stsr008_root_occurrence_mapping_observability import load_module


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    sts = load_module(args.build_dir)
    fixture = json.loads(Path(__file__).with_name("issue45-fairy-root.json").read_text())
    sim = sts.StepSimulator(sts.CharacterClass.IRONCLAD, fixture["seed"], fixture["ascension"])
    for identity in fixture["actions"]:
        scope, bits = identity.split(":")
        matches = [a for a in sim.legal_actions() if a.scope == scope and a.bits == int(bits)]
        assert len(matches) == 1, identity
        sim.step(matches[0])
    assert len(fixture["actions"]) == fixture["decision"] == 27
    assert sim.snapshot() == fixture["expected_snapshot"], "captured root differs"
    checkpoint = sim.capture_checkpoint()
    on = sim.battle_search_v2(400, True)
    assert sim.snapshot() == fixture["expected_snapshot"]
    sim.restore_checkpoint(checkpoint)
    off = sim.battle_search_v2(400, False)
    assert sim.snapshot() == fixture["expected_snapshot"]
    assert on["unmapped_search_edge_count"] == 0
    assert on["root_visits"] == 400
    discard = [r for r in on["root_rows"] if r["kind"] == "potion_discard"]
    assert len(discard) == 1
    assert discard[0]["idx1"] == 0 and discard[0]["idx2"] == 6
    assert discard[0]["bits"] == 537264128
    assert discard[0]["search_tree_present"] and discard[0]["visits"] > 0
    assert sum(r["visits"] for r in on["root_rows"]) == 400
    assert off["unmapped_search_edge_count"] == 0
    assert [r for r in off["root_rows"] if r["kind"] == "end_turn"][0]["visits"] == 400
    args.output.write_text(json.dumps({"on": on, "off": off, "prefix_actions": 27}, indent=2) + "\n")
    print("ISSUE45_CAPTURED_FAIRY_ROOT_PASS")


if __name__ == "__main__":
    main()
