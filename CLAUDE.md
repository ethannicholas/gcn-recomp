# Working notes for this repository

This is the shared GameCube recompilation layer used by the per-game projects
(`~/Source/prime-recomp` and `~/Source/bluestorm-recomp`). It was split out of
bluestorm-recomp on 2026-10-07, and bluestorm-recomp was rebased onto it the next day. The
Android and OpenXR frontends moved here on 2026-10-08, when Prime wanted a headset build, as
`android/`, with the game's own decisions behind `vr::GameHooks`; bluestorm-recomp still
builds its own copies, with its gates written into them, until it moves onto these.

**Nothing game-specific belongs here.** No disc IDs, guest addresses, game names or
per-game heuristics in code; those arrive through `gcn_add_game()` and the game project's
tables. If a change needs to know something about one game, it needs a parameter or a hook,
not a constant. Environment variables, CMake options and macros use the `GCN_` prefix.

**No game code or data**, as in the game projects: never commit or quote the generated C,
disassembly or anything from a disc.

Record what you learn in `docs/`: `diagnostics.md` for the `GCN_*` variables, input
logging and replay, the benchmark and the crash dumps; `graphics.md` for the renderer.
Game-specific findings go in the game project's `docs/dev/`, not here.

Each game project pins a commit of this repository as a submodule. After changing shared
code: commit here, push, then update the submodule pointer in the game project(s) and
commit that too. Commit on `main`, no feature branches.
