# RADS Autonomous State — handoff journal

Last updated: 2026-10-12 (cron run 8)
Branch: `phase0/build-recovery`
HEAD: `04c1ac3` fix(build): stop 'make clean' deleting tracked rads-mask test fixtures

## What this run changed

**Perf round 5 from the journal — loops in the bytecode VM — landed as
`30f255f`, plus the rads-mask deleter is finally dead (`04c1ac3`).**

Work done in a git worktree (`/home/zari/Desktop/RADS-wt-bc`) because the
main checkout sat on a dirty `nightshift/20261012-fix-bisect-build` branch
with someone's uncommitted stdlib work (heapq/logging/statistics + a stray
root `rads-mask` binary) — untouched. NOTE for next run: check which branch
the main checkout is on; if still dirty nightshift, keep using worktrees.

### Loops in the BC subset (`30f255f`)

- `cruise (i in a..b)` range loops, `loop (cond)` while-loops, `break`,
  `continue` now compile to bytecode. break/continue are plain BC_JMPs
  recorded in per-loop patch lists, fixed up when the loop closes — correct
  through nested ifs and nested loops (inner loop closes first and claims
  its own sites).
- cruise bounds: start/end evaluated once into slots; `$range_end$` hidden
  slot (interned name no identifier can produce). Condition via
  binop_apply OP_LT — bit-identical tree-walker semantics, verified by A/B
  (RADS_NO_BC=1) on nested-loop/break/continue/empty-range/while tests.
- **`echo` statement added to the subset (BC_ECHO)** — the trap of the run:
  every benchmark main() ends in `echo(...)`, so without it loop
  compilation NEVER fired for the benchmarks (main bailed to tree-walk).
  Found via new `RADS_BC_TRACE=1` env flag (prints compile-bail fn names).
- **`main()` now routes through `interpreter_execute_callback`** instead of
  raw exec_statement — its body compiles to bytecode like any blast
  function. Global-scope semantics unchanged (no params, globals visible).
- Numbers (best-of-5 wall): 2M-iter cruise micro 187→**158ms (-15%)**;
  official suite: fib 100.6→95.8, string_build 58.0→**53.5**,
  array_churn 13.3→12.8, file_io 23.2→21.6. array_churn gains are modest
  because its inner work is native `array.*` calls, not loop arithmetic.
- Suite 7/8 (known pre-existing fail), ASAN+UBSAN clean on all workloads
  and loop stress tests.

### rads-mask deleter solved (`04c1ac3`)

Journal runs 6/7 flagged `tools/rads-mask/tests/*.rads` showing as deleted.
Root cause: `tools/rads-mask/Makefile` clean target had `rm -f tests/*.rads`
(intended to clear GENERATED transpiler outputs), and the root Makefile's
`clean` cascades `make -C tools/rads-mask clean` — so every `make clean`
wiped tracked fixtures. Fixed at source; verified `make clean` now leaves
zero git deletions.

## Test results

- `make`: green (pre-existing stdlib warnings only).
- `run_tests.sh`: **7/8** — the 1 fail is the known pre-existing
  `v0_0_7_comprehensive.rads` (await-in-nested-scope parser gap), unchanged.
- Sanitize build (ASAN+UBSAN+LSAN): all 4 benchmarks + loop stress tests
  clean (leak noise pre-existing).
- Benchmarks (results/20261012T055804Z.json): fib **95.8** / string_build
  **53.5** / array_churn **12.8** / file_io **21.6** vs CPython
  58.3/20.6/14.4/35.8 and Node 105.0/101.0/103.2/124.8.

## Next up (single task for next run)

**Perf round 6 candidates, pick ONE:**
1. **String concat inline in BC_BIN** (round-5 stretch, still open): OP_ADD
   on (string,int) fast path without the 256-byte buffer dance in
   binop_apply. string_build is 2.6x off CPython — biggest gap.
2. **In-place slot arithmetic for `x = x + ...`**: avoid clone/free of the
   LHS slot; array_churn's accumulate loop is the target.
3. **Widen subset: index/member expressions** (`arr[i]`, `arr.length`) as
   opcodes — array_churn's `array.pop` calls could become BC ops.

Rules unchanged: compile-bail fallback, suite 7/8 minimum, ASAN clean,
`bash benchmarks/run.sh` before/after, A/B with RADS_NO_BC=1.
If it stalls >10 min, fall back to the AGENTS.md workstream priority.

## Open blockers

- `git push` still fails: no credentials. **28 commits** now await push
  locally on `phase0/build-recovery` (+2 this run). Needs PAT or
  `gh auth login` by the owner (GitHub account colyzeo@gmail.com).
- `v0_0_7_comprehensive.rads` uses unsupported syntax (`await` in nested
  scope, `#`, `fn`, `let`) — rewrite or implement; candidate work.
- Pre-existing gcc warnings (unused vars/fns in stdlib_net, functools,
  parser.c:648) — cosmetic.
- `.gitignore` line 57 `core` matches `src/core` — `git add -f` for
  core changes (hit again this run, expected).
- LeakSanitizer pre-existing leaks (AST/parser/csv/env stdlib) —
  hygiene workstream, not blocking.
- ~~rads-mask tests deleted by clean~~ — SOLVED this run (`04c1ac3`).
- `benchmarks/rads/` was untracked in git (run.sh referenced it, files
  only existed in the dirty main checkout) — committed this run. Watch
  that the nightshift branch doesn't also carry a conflicting copy.
