# RADS Autonomous State — handoff journal

Last updated: 2026-10-12 (cron run 12)
Branch: `phase0/build-recovery`
HEAD: `b863fe9` perf(round 9): BC_GETIDX — compile arr[i] index reads to a VM opcode

## What this run changed

**Perf round 9, journal option 1 — BC_GETIDX, exactly as planned.** New
opcode `BC_GETIDX`: `arr[i]` index expressions now compile in the BC
subset. Compile side (bc_compile_expr AST_INDEX_EXPR case): compile
array expr, then index expr, emit BC_GETIDX — same eval order as the
tree-walker. VM side: pop idx, then arr; null unless receiver is
VAL_ARRAY and idx is VAL_INT in bounds; string elems strdup'd
(make_string); **nested-array elems shared with NO refcount bump** —
the tree-walker quirk reproduced exactly, not fixed, so A/B stays
bit-identical; scalars copied raw; both operands freed. A line-for-line
mirror of eval_expression's AST_INDEX_EXPR path.

### Verification (mandatory gauntlet, all green)

- **A/B byte-identical** (BC vs RADS_NO_BC=1, md5 match) on a
  33-output probe: int elems, bounds edges, OOB/negative → null,
  non-int idx (float/string/null/bool) → null, non-array receivers →
  null, string elems, complex idx exprs (i+2, arr[arr[i]]), mixed-type
  array reads, loop-indexed reads, index-of-call-result, chained
  grid[1][0], side-effect idx (`st[array.pop(st)]`), empty array.
- **Timing differential (positive compile proof)**: hot loop 200×5000
  `acc = acc + arr[k]` — BC 0.054s vs NOBC 0.114s = **2.1× faster**,
  identical output. A compile-bail ties (tree-walks); it cannot win —
  so main() genuinely compiled with GETIDX in it. (Run-11 lesson
  applied: identity alone can't distinguish compile from bail.)
- Suite **7/8** (known pre-existing `v0_0_7_comprehensive.rads`).
- ASAN/UBSAN: **zero NEW memory errors** — the UAF/leaks the probe
  surfaced are **pre-existing, proven**: identical UAF reproduces on
  clean HEAD (stash + rebuild) and with RADS_NO_BC=1 (VM off, opcode
  unreachable); both crash frames are tree-walker (eval_expression
  L1092 index-case tail, exec_echo). Sanitize builds also swallow
  program stdout (banner only) — also pre-existing at clean HEAD.
- Clean -O2 rebuild after sanitize; re-verified A/B after rebuild.

### Numbers (results/20261012T185637Z baseline / 185900Z after)

- Same-session baseline BEFORE: fib 92.8 / string_build 26.0 /
  array_churn 11.1 / file_io 21.8.
- AFTER: fib 91.4 / string_build 26.2 / array_churn **10.4** /
  file_io 21.8 — all flat (expected: none of the four benchmarks
  index in a hot loop; GETIDX widens the subset, it isn't a
  benchmark-fold — the micro-bench above is the real signal).

## Test results

- `make` green (pre-existing warnings only). Suite 7/8.
- Benchmarks (after): fib 91.4 / string_build 26.2 / array_churn
  10.4 / file_io 21.8 vs CPython 58.5/20.7/14.6/35.3.

## Discovered this run (not fixed — pre-existing, upstream of BC)

- **Tree-walker UAF** at the AST_INDEX_EXPR tail (interpreter.c ~L1091
  value_free of arr): repro = the 33-output probe under
  ASAN. `geta(arr)[3]` is the suspected trigger (return-shared value
  fed to index) but t1–t5 minimal probes were clean — trigger is a
  subtler shape; bisect the probe file when picking this up.
  Heap-use-after-free at value_release L171/L195, freed at
  eval_expression L1092, allocated by make_array_from_list. Reproduces
  on clean HEAD and with RADS_NO_BC=1 — 100% tree-walker, zero BC
  involvement.
- **Sanitize-build stdout swallow**: ASAN builds print banner but no
  program output (pre-existing at clean HEAD; exit 0, runs fine in -O2).
  Likely stdio/ASAN init ordering — cosmetic for CI, worth knowing.

## Next up (single task for next run)

1. **Tree-walker UAF fix** (NEW, discovered this run — highest value):
   the AST_INDEX_EXPR tail free at interpreter.c ~L1091. Root-cause
   the share-no-bump quirk vs value_free of the container: when the
   result shares a nested array pointer with the (about-to-be-freed)
   container's elem, free order decides UAF vs leak. Fix in the
   tree-walker FIRST, then mirror the fixed semantics into BC_GETIDX
   (A/B re-verified). This is a real user-facing memory bug, not perf.
2. Alternative: fold `if (x <op> imm)` compare-and-branch (BC_CMPF) —
   fib's `n < 2` runs per call; smaller win.
3. Alternative: exit-time BcFunc cache free (LSan hygiene).

Rules unchanged: compile-bail fallback, suite 7/8 minimum, ASAN clean
(pre-existing leaks tolerated), `bash benchmarks/run.sh` before/after
(same session only), A/B with RADS_NO_BC=1, diff BC vs tree-walker
output on a mixed-type probe (mandatory). If stalled >10 min, fall
back to AGENTS.md workstream priority.

## Open blockers

- `git push` still fails: no credentials. **37 commits** now await
  push locally on `phase0/build-recovery` (+1 this run). Needs PAT or
  `gh auth login` by the owner (colyzeo@gmail.com).
- Main checkout still on dirty `nightshift/20261012-fix-bisect-build`
  with uncommitted stdlib work — untouched, as before.
- `v0_0_7_comprehensive.rads` unsupported syntax — rewrite or implement.
- Pre-existing gcc warnings (unused vars/fns) — cosmetic.
- `.gitignore` line 57 `core` matches `src/core` — tracked files add
  fine, but `git add` of NEW files under src/core needs `-f`.
- LeakSanitizer pre-existing leaks (AST/parser/csv/env + BcFunc caches)
  — hygiene workstream, not blocking.
