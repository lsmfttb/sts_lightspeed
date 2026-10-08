# Issue 24 proposal law and supported domain

## Proposal contract

The study sampler is a deterministic public-consistent proposal:

`Q(particle | current public battle observation, observed public history, independent sampler seed, particle index)`

It is not an exact posterior. The trusted backend reconstructs the exposed native root from the fixed run seed and public action history, then checks the reconstructed projection against the executor's public input. The isolated noncombat policy receives immutable JSON bytes only and cannot import the simulator extension. Particle generation derives each indexed seed from the independent sampler seed and particle index; it does not use anchor RNG seeds or counters, the hidden game seed as a proposal input, or the anchor's hidden draw order as a proposal choice.

## Latent fields and public constraints

- Game and Battle random streams are reset from particle-derived seeds, including their counters. The audited streams include the game AI, card, event, math, merchant, misc, monster HP, monster, Neow, potion, relic, shuffle, and treasure streams, plus Battle AI, card, misc, monster HP, potion, and shuffle streams. Game and Battle seed fields are also replaced from the particle seed.
- Hidden draw order is sampled after canonical sorting by public card face. Public known-top cards, deterministic known positions, Frozen Eye's visible full order, and represented insertion constraints are preserved. Unsupported or inconsistent draw constraints fail closed.
- Green and Red Louse `miscInfo` is sampled independently from the legal range (A20: 6–8; Ascension 0–1: 5–7). A current Bite conditions that range on the visible modified damage to the player, including applicable modifiers. With no visible Bite damage, the latent value remains uncertain across the legal range. The private base parameter is omitted from policy input.
- Darkling private damage counters, unrepresented Hexaghost move-cycle state, Runic Dome hidden intents, and other unsupported information states remain fail-closed.
- Battle action labels omit runtime card instance IDs. Public candidate identity and order are checked against the native action surface before a choice can execute.

## Evidence

- `test-public-battle-state-semantics` passes. It varies hidden RNG seeds and counters, hidden draw order, and Louse bite values across public-equivalent anchors; checks indexed particle/future and multi-step transition equivalence; compares B=192 root statistics and choices across independent anchors; and injects anchor-counter dependence as an operative negative control.
- The pinned STSRL `execute_controlled_run` path completed the first-Battle B=192 shared-public search for both exposed seeds (49 and 50), using 32 particles. For each seed, root replay and action identity/order parity passed, the anchor-counter fault control detected its injected dependence, all indexed public sample futures matched, and the B=192 independent-anchor root statistics and selected action matched. The selected public action was executed.
- The bounded runs stopped at the declared 16-decision limit after executing the first-Battle choice. This provides no held-out-seed, win-rate, broad A20, or continuation-strength claim.

The detailed seed traces and source pins are in [`result.json`](result.json) and [`result.md`](result.md). Keep this implementation and its artifacts study-only pending independent review; a reusable core capability requires a separate promotion decision.
