> **Research integration status (branch `spire/main`): RETIRED.** This experimental branch is not the authoritative STSRL simulator line. Use `lsmfttb/STSRL:main` and its manifest-pinned `lsmfttb/sts_lightspeed:stsrl/main` instead. The discarded `native-public-projection-v3` implementation is retained for reproducibility at `archive/spire-v3-2026-10-09` (`d1dcd6534ec4a1f38ac1f7f916f01a3f931fcfd0`). Do not extend or restore v3 screen coverage without separately justified new science and explicit Planner authorization. See `AGENTS.md` in this branch.

# sts_lightspeed

For tree search and simulation of the popular rogue-like deckbuilder game Slay The Spire

**Features**
* c++ 17 compiled with gcc
* Standalone
* Designed to be 100% RNG accurate*
* Playable in console
* Speed: 1M random playouts in 5s with 16 threads
* Loading from save files (loading into combat currently only supported)
* Tree Search (best result, knowing the state of the game's rng)

**Planned Features**
* Tree search of possible game outcomes (not given the state of rng)

**Implementation Progress**
* All enemies
* All relics
* All Ironclad cards
* All colorless cards
* Everything outside of combat / all acts

**Getting Started**
* The project was built with Clion2021 and the [mingw64 toolchain](https://www.msys2.org/) on Windows 10
* The main target creates a simulator of the game that can be played in console.
* The test target creates a program with various commands that can be run, including random simulation
* Click the star button at the top of the repo :)

**Build tips**
* If your build fails with an error about not-return-only `constexpr` methods, ensure your compiler supports c++17.
* If CLion shows an error about not finding python libs when loading the cmake project, try opening CLion from the msys2 shell.
