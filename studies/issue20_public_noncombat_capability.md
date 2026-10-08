# Issue #20: Native public noncombat capability

Base: `spire/main@ab2b11bc3b5b6c6b68d9d855bc9545e9aca62a28`.

`StepSimulator.publicProjection()` now returns schema `native-public-projection-v3`. The new screen payloads expose current simulator-rendered choices and bind each one to its exact ordinal in the ordered legal-action list plus its sanitized action identity. These bindings contain no action bits; the native handles remain with the executor.

## Screen and field coverage

| Surface | Status | Public fields and fail-closed boundary |
| --- | --- | --- |
| Map graph | Supported fields | Coordinates, outgoing edges, current location, and source-owned room symbols. Hidden rooms remain `?`; no raw room or event identities are emitted. |
| Immediate map routes | Supported | Legal destinations carry candidate index and sanitized action identity. The test checks multiple legal branches and destination/graph consistency. |
| Act boss identity | Unavailable; Map screen is partial | `GameContext` does not track boss-name reveal state. Reading `boss` or `secondBoss` could disclose hidden selection. |
| Event choices | Conditional support | Current identity, visible options, and explicit phase when tracked. Descriptions come from `ConsoleSimulator::printEventActions` and are supported only when every numbered option maps uniquely to a current legal event action. Events with no separate phase counter report `not_applicable`. Match and Keep, invalid/non-screen event identities, out-of-range Cursed Tome phases, and renderer/action mismatches are unsupported with a reason. |
| Rewards | Supported for recognized current actions | Cards include ID, name, type, rarity, and upgrade state; relic, potion, gold, key, skip, and potion use/discard candidates expose their current values and candidate bindings. Unknown or mismatched candidates fail closed. Multiple gold candidates now retain their native reward indices. |
| Battle | Supported; unchanged | Existing `public_battle_state()` semantics and action identities are checked against the pre-existing Battle semantics test. |
| Boss relic, card select, treasure, rest, shop | Unsupported | Each screen reports an explicit reason; legal-action availability alone is not claimed as observation support. |

## Verification and evidence

Release configure and focused build used:

```text
cmake -S . -B build-issue20 -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build-issue20 --target test-public-noncombat-projection -j2
build-issue20/test-public-noncombat-projection
cmake --build build-issue20 --target test-public-battle-state-semantics -j2
build-issue20/test-public-battle-state-semantics
```

Both executables passed. The CMake policy setting is needed because the pinned json submodule has a pre-3.5 minimum-version declaration and the installed CMake has removed that compatibility mode.

The executable study fixture uses real A20 Ironclad starts at seeds 49 and 50. Seed 49 exposes four Neow options, then three actual card offers (`Finesse`, `Discovery`, `Deep Breath`) and skip, then a 58-node map graph with four legal starting routes. Seed 50 presents a different set of Neow options. It also checks unknown-room privacy, unique ordered associations, hidden-RNG/seed/second-boss noninterference, and equality of Battle public state across public-consistent hidden futures. A focused native `Rewards`-container fixture verifies two distinct gold amounts (17 and 41), Anchor, Fire Potion, and Sapphire Key/relic removal; it executes both indexed gold candidates to check their effects. Synthetic phase fixtures check Dead Adventurer phase reporting and fail-closed handling of an out-of-range Cursed Tome phase.

The captured deterministic evidence is [issue20_public_noncombat_capability.json](issue20_public_noncombat_capability.json). It makes no run-outcome or policy-performance claim.

## Limitations

- Map coverage remains partial until native state tracks whether the Act boss identity has become public.
- Event text reuses the simulator's existing human-facing renderer. Runtime candidate matching prevents unsupported/mismatched option sets from appearing supported; this task does not independently audit every event's underlying game mechanics.
- The fixed generated reward path exercises card offers and skip. Gold, relic, potion, and key serialization/effect mappings are checked with a deterministic native container fixture rather than a generated combat-reward path.
- Boss relic, card-select, treasure, rest, and shop choice semantics remain unsupported and must be treated as such by downstream decision code.
