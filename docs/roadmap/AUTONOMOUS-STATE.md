# RADS Autonomous State — handoff journal

Last updated: 2026-10-12 (cron run 3 of the eternal dev loop)
Branch: `phase0/build-recovery`
HEAD: `88beadb` fix(core): call-frame scoping

## What this run changed

**Run focus: Phase 1 workstream 1 (benchmark suite) — diverted by a
showstopper correctness bug found while writing the fib benchmark.**

1. `88beadb` — **fix(core): call-frame scoping.** Writing the fib benchmark
   revealed `fib(25)` returned **-575** (correct: 75025). Root cause:
   `interpreter_execute_callback` bound parameters directly into the single
   global env and never restored them — in `fib(n-1) + fib(n-2)` the left
   call clobbered global `n` before the right arg evaluated. Fix: EnvFrame
   push/pop; params/locals declare into the frame (`env_decl`), assignment
   (`env_set`) updates nearest visible binding (globals still mutable from
   functions), frame locals shadow globals.
2. Same commit — **parser forward-progress guard**: invalid input (e.g. `fn`
   keyword) could loop emitting the same error forever (~135MB stderr in 10s
   — disk-fill risk). `parser_parse` now advances when an iteration consumes
   zero tokens (135MB → 610 bytes).
3. `tests/test_recursion.rads` added + wired into `run_tests.sh` (fib values,
   param-shadowing of globals, 50-deep recursion).

### Language facts learned (verified against the real binary)
- Real syntax: `blast f(i32 n) -> i32`, `turbo i32 x = ...`, `loop (...)`,
  `cruise (i in 0..n)`, `//` comments only (`#` is a parse error!), top-level
  statements need `;`. `fn`/`let` from the tests/ dir are NOT supported
  (several tests/*.rads outside run_tests.sh fail to parse — pre-existing).
- No dict/map type: only arrays. Hash churn benchmark must be adapted
  (array-of-pairs with str functions, or an interned-array workload).
- `examples/01-basics/*.rads` are the best syntax reference.

## Test results

- `make`: green. `run_tests.sh`: **6/7 pass** (was 5/6; +1 new recursion
  test). The 1 fail is still `v0_0_7_comprehensive.rads` (never-supported
  syntax) — pre-existing, not a regression.
- Verified no regressions via git stash A/B: identical pass/fail sets
  before/after the change (plus the new green test).
- Broader tests/*.rads outside the runner (test_string/math/bisect/itertools/
  v0.0.12) fail to parse pre-existing (`#` comments etc.) — unchanged.

## Next up (single task for next run)

**Phase 1 workstream 1 (retry): benchmark suite.** The blocker is now fixed —
fib recursion is correct. Create `benchmarks/` with RADS + CPython 3.13 +
Node 20 workloads (fib(30), string build via concat loop, array push churn,
file I/O); skip hash-table churn or adapt to arrays (no dict type). Runner
`benchmarks/run.sh` + machine-readable JSON + `benchmarks/RESULTS.md`.
RADS is a tree-walker — expect it slow; the honest numbers ARE the point.

## Open blockers

- `git push` still fails: no credentials (gh CLI unauthed). **10 commits**
  now await push locally on `phase0/build-recovery`. Needs PAT or
  `gh auth login` by the owner (GitHub account colyzeo@gmail.com).
- `v0_0_7_comprehensive.rads` + several stray tests/*.rads use unsupported
  syntax (`#` comments, `fn`, `let`) — rewrite or implement; candidate work.
- Pre-existing gcc warnings (unused vars/fns in stdlib_net, functools,
  parser.c:648 `update_block`) — cosmetic.
- `.gitignore` line 57 `core` matches `src/core` on directory add — use
  explicit file paths or `git add -u` when staging core changes.
