# STSRL Native API Smoke Check

This fork provides a lightweight smoke check for the native Python API surface
currently required by STSRL:

```bash
python scripts/stsrl_api_smoke.py --build-dir build-py
```

The script searches `build-py` for the compiled `slaythespire` pybind extension,
imports it, constructs `StepSimulator(CharacterClass.IRONCLAD, seed,
ascension)`, and validates the required STSRL method names and snapshot
resource identity fields.

## Clean Checkout Example

From a clean checkout, build the pybind module first:

```bash
git submodule update --init --recursive
cmake -S . -B build-py -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build-py --target slaythespire
python scripts/stsrl_api_smoke.py --build-dir build-py
```

The `CMAKE_POLICY_VERSION_MINIMUM` flag keeps current CMake 4.x versions
compatible with the older `json` submodule CMake metadata.

If the module already exists somewhere else, point the smoke check at that
directory:

```bash
python scripts/stsrl_api_smoke.py --module-dir /path/to/module-dir --build-dir ""
```

The command exits nonzero if a required module, method, or snapshot field is
missing.

## Checked Surface

The smoke check asserts these `StepSimulator` methods are callable:

- `reset`
- `snapshot`
- `observation`
- `legal_actions`
- `step`
- `capture_checkpoint`
- `restore_checkpoint`
- `public_projection`
- `battle_search`
- `legal_battle_start_encounters`
- `rebuild_battle_start`

It also checks these snapshot fields:

- `potions`
- `deck`
- `relics`
- `blue_key`
- `green_key`
- `red_key`

`battle_search`, `legal_battle_start_encounters`, and `rebuild_battle_start`
require an active battle context, so this smoke check verifies that the methods
exist without running battle-specific oracle behavior.

## Acceptance Boundary

This script is a fork-side convenience check for early maintainer feedback. It
does not replace STSRL's authoritative source acceptance path. STSRL acceptance
still requires a reviewed STSRL pull request, an exact
`docs/sts_lightspeed_source_manifest.json` commit pin, and a passing
`scripts/verify_lightspeed_source.sh` run from the STSRL repository.
