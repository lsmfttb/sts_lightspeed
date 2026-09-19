"""Bounded T096 native API smoke; never runs the scientific four-anchor audit."""

from __future__ import annotations

import argparse

import slaythespire


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--ascension", type=int, default=20)
    parser.add_argument("--steps", type=int, default=200)
    parser.add_argument("--particles", type=int, default=8)
    args = parser.parse_args()
    sim = slaythespire.StepSimulator(
        slaythespire.CharacterClass.IRONCLAD, args.seed, args.ascension
    )
    for step_index in range(args.steps):
        snapshot = sim.snapshot()
        if (
            snapshot.get("screen_state") == "BATTLE"
            and snapshot.get("battle_active") is True
            and snapshot.get("battle_input_state") == "PLAYER_NORMAL"
        ):
            projection = sim.t096_public_information_projection()
            particles = sim.sample_hidden_future_particles(17, args.particles)
            if any(
                row["public_information_projection"] != projection
                for row in particles
            ):
                raise AssertionError("T096 native smoke found public projection drift")
            if len({row["hidden_future_fingerprint"] for row in particles}) < 2:
                raise AssertionError("T096 native smoke found no hidden diversity")
            if any(
                "seed" in row["public_information_projection"]
                or "rng" in row["public_information_projection"]
                for row in particles
            ):
                raise AssertionError("T096 native smoke found hidden leakage")
            print(
                "T096_NATIVE_SMOKE_PASS "
                f"step={step_index} particles={len(particles)} "
                f"distinct_hidden={len({row['hidden_future_fingerprint'] for row in particles})}"
            )
            return 0
        actions = sim.legal_actions()
        if not actions:
            break
        sim.step(actions[0])
    raise RuntimeError("bounded smoke did not reach an ordinary Battle decision")


if __name__ == "__main__":
    raise SystemExit(main())
