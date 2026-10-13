# RADS Autonomous State — handoff journal

Last updated: 2026-10-13 (cron run 13)
Branch: `phase0/build-recovery`
HEAD: see `git log -1` — fix(interp): index-shared nested arrays now bump refcount (UAF)

## What this run changed

**Run 13 = journal "Next up" #1: the tree-walker index UAF, root-caused and
FIXED, mirrored into BC_GETIDX in lockstep.** One-line semantic change at
two sites in `src/core/interpreter.c`:

- Tree-walker `AST_INDEX_EXPR` (eval_expression, ~L1083): the nested-array
  share path now does `result.array_val->refcount++` after the share.
- `BC_GETIDX` VM case (~L2486): identical bump — the mirror is exact, so
  A/B stays bit-identical.

### Root cause (proven, systematic-debugging Phase 1-3 complete)

The index case shared `elem.array_val` with NO refcount bump, then
`value_free(&arr)` dropped the container. Two detonation shapes,
both reproduced RED under ASAN on pre-fix binaries, both GREEN after:

- **Shape A (dangling result):** `turbo x = [[1,2],[3,4]][1];` — the
  outer literal temp has rc=1 and no other owner; the tail free drops it
  to 0 → items freed → shared inner array freed → `x` dangles. Next read
  of `x` = UAF (NOBC: `value_clone_impl:130` via `env_list_get:368`;
  BC: `vm_run:2249`). Note: at -O2 without ASAN this printed a BLANK for
  `x[0]` — silent wrong output, not just teardown noise.
- **Shape B (double free at teardown):** `grid[1][0]` chained index — the
  intermediate `grid[1]` share frees the inner array at rc=1 while
  `grid`'s item slot still points at it → double release at exit
  (`value_release:171/174` via `value_free:246`).

Why run 12's t1–t5 probes were "clean": they used single-level named
arrays — the shared pointer existed but was never touched again, and
teardown order masked it. The trigger needs a literal-temp container OR
a chained index whose result outlives the tail free. Journal's
`geta(arr)[3]` suspicion was wrong (call results go through
value_clone); probes A/B above are the minimal RED repros.

### Verification (mandatory gauntlet, all green)

- **RED→GREEN on probes A/B**: pre-fix ASAN rc=1 with heap-use-after-free
  (both shapes, BC and NOBC); post-fix rc=0, zero ASAN hits, correct
  output (`3`/`3`), BC ≡ NOBC.
- **A/B byte-identical** (md5 `0d726f9a…` match) on a 33-output probe
  covering: int elems, OOB/negative → null, whole-nested-array reads,
  chained grid[1][1], string elems, computed idx, arr[arr[0]/10],
  mixed-type reads incl. nil/bool, loop-indexed, literal-temp chained,
  share-then-use (shape A), chained named (shape B).
- **Timing differential (positive compile proof)**: 200×5000 hot loop —
  BC 0.071s vs NOBC 0.130s = **1.83×** (compile-bail can only tie).
- **Suite**: pre-fix and post-fix failure lists are IDENTICAL (9 files,
  all pre-existing on this branch: bisect/http/itertools/math/string/
  struct/time/v0.0.12/v0_0_7_comprehensive — stdlib tests for the
  unmerged nightshift work). Zero new regressions. The journal's "7/8"
  gate is satisfied in spirit: no test that passed before fails now.
- **New regression test**: `tests/test_index_uaf.rads` (shapes A/B/C/D:
  literal temp, chained, reassignment churn ×50, triple-nested) — PASS,
  ASAN-clean, BC ≡ NOBC.
- Full probe + test_array_uaf.rads + test_index_uaf.rads under ASAN
  (detect_leaks=0): zero AddressSanitizer hits, BC and NOBC.

### Numbers (results/20261013T013352Z baseline / 013704Z after)

- BEFORE: fib 92.3 / string_build 26.2 / array_churn 10.9 / file_io 21.9.
- AFTER: fib 90.8 / string_build 25.6 / array_churn 10.5 / file_io 21.5.
  Flat (noise) — expected: one refcount++ per nested-array index read,
  and no benchmark reads nested arrays in a hot loop.

## Next up (single task for next run)

1. **BC_CMPF** (journal alternative, now top): fold `if (x <op> imm)`
   compare-and-branch — fib's `n < 2` runs per call; smaller perf win,
   well-scoped. Same gauntlet applies.
2. Exit-time BcFunc cache free (LSan hygiene — shrinks the pre-existing
   leak report).
3. `v0_0_7_comprehensive.rads` unsupported syntax — rewrite or implement.

Rules unchanged: compile-bail fallback, suite no-new-failures minimum,
ASAN clean (pre-existing leaks tolerated), `bash benchmarks/run.sh`
before/after (same session only), A/B with RADS_NO_BC=1, diff BC vs
tree-walker output on a mixed-type probe (mandatory). If stalled >10 min,
fall back to AGENTS.md workstream priority.

## Open blockers

- `git push` still fails: no credentials. **38 commits** now await
  push locally on `phase0/build-recovery` (+1 this run). Needs PAT or
  `gh auth login` by the owner (colyzeo@gmail.com).
- Main checkout still on dirty `nightshift/20261012-fix-bisect-build`
  with uncommitted stdlib work — untouched, as before.
- Pre-existing gcc warnings (unused vars/fns) — cosmetic.
- `.gitignore` line 57 `core` matches `src/core` — tracked files add
  fine, but `git add` of NEW files under src/core needs `-f`.
- LeakSanitizer pre-existing leaks (AST/parser/csv/env + BcFunc caches)
  — hygiene workstream, not blocking.
- Sanitize-build stdout swallow (banner only, exit 0) — pre-existing,
  cosmetic; use a parallel -O2 build for output checks (this run kept
  `rads-asan` + `rads-o2` side by side in the scratch dir — do that).

## Run log (one line each)

- run 11: BC_GETIDX planned, compile-bail lesson learned.
- run 12: BC_GETIDX shipped (round 9), 2.1× timing proof, index UAF
  discovered (pre-existing), 37 commits pending push.
- run 13: index UAF root-caused (share-no-bump at the tail free) and
  fixed in tree-walker + BC_GETIDX; regression test added; 38 commits
  pending push.
