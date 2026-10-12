# RADS Autonomous State — handoff journal

Last updated: 2026-10-12 (cron run 7)
Branch: `phase0/build-recovery`
HEAD: `1c81445` perf(core): bytecode VM fast path for blast function bodies

## What this run changed

**Perf round 4 from the journal — the bytecode prototype — landed as
`1c81445` and it WORKS.** fib(28): 135.7 → **100.6 ms (-26%)**; vs the
run-5 baseline of 186 ms it's -46%. Gap to CPython: 2.3x → **1.6x**.
RADS beats CPython 3/4, Node 4/4 (fib now beats Node outright).

Design (all in `src/core/interpreter.c`, ~450 lines + ast.h/ast.c cache
field `function_decl.bc_cache`):

1. **Subset compiler.** `blast` bodies using only block/if/return/var-decl/
   assign/expression-statements over literals, identifiers, binary ops and
   identifier-callee calls compile once (first invocation) to a compact
   stack bytecode (16 opcodes). Slotted locals replace the linked-list
   frame: params + locals get fixed slots, lookup is an array index.
   Anything else bails at compile time → `bc_cache = (void*)1` →
   tree-walks forever. Fully transparent fallback.
2. **Dispatch loop** (`vm_run`): computed-goto-free plain switch, fixed
   Value stack, inlined int fast paths (GETL trivial-copy, JMPF int test,
   binop via shared `binop_apply`). BC_CALL reuses the exact tree-walker
   resolution: `resolve_callee_cached` + `invoke_callee` (extracted from
   `eval_call` in this run — the callee cache from round 3 applies to
   bytecode call sites too).
3. **`RADS_NO_BC=1`** env kill-switch for A/B benchmarking.
4. Note: `src/vm/{vm.c,vm.h}` is an older dead orphan (own Value type,
   wired to nothing) — candidate for archive/removal next hygiene pass.

Also this run: restored tracked `tools/rads-mask/tests/*.rads` that showed
as deleted in git status (stray; nothing in the build deletes them — watch
for recurrence).

## Test results

- `make`: green (0 errors; pre-existing stdlib unused-param warnings only).
- `run_tests.sh`: **7/8** — the 1 fail is the known pre-existing
  `v0_0_7_comprehensive.rads`, unchanged.
- Sanitize build (ASAN+UBSAN+LSAN): all 4 benchmarks + full suite run
  clean (leak noise pre-existing).
- Benchmarks (results/20261012T024458Z.json): fib **100.6** / string_build
  58.0 / array_churn 13.3 / file_io 23.2 vs CPython 62.8/21.1/16.3/36.4
  and Node 112.5/110.9/112.2/132.6.
- A/B verified: RADS_NO_BC=1 fib ≈ 152ms vs 104ms enabled (wall clock).

## Next up (single task for next run)

**Perf round 5: widen the bytecode subset to loops.** `while` (loop_stmt)
and `cruise` (range for-loops) in the BC compiler + opcodes
(BC_LOOP / BC_GETI iteration), so array_churn's hot loops and general
user code compile too. Same rules: compile-bail fallback, suite green,
ASAN clean, `bash benchmarks/run.sh` before/after. Stretch: string
concat (OP_ADD on strings) inline in BC_BIN to move string_build.
If it stalls >10 min, fall back to the AGENTS.md workstream priority.

## Open blockers

- `git push` still fails: no credentials. **20 commits** now await push
  locally on `phase0/build-recovery`. Needs PAT or `gh auth login` by
  the owner (GitHub account colyzeo@gmail.com).
- `v0_0_7_comprehensive.rads` + stray tests/*.rads use unsupported
  syntax (`#`, `fn`, `let`) — rewrite or implement; candidate work.
- Pre-existing gcc warnings (unused vars/fns in stdlib_net, functools,
  parser.c:648) — cosmetic.
- `.gitignore` line 57 `core` matches `src/core` — `git add -f` for
  core changes (hit again this run, expected).
- LeakSanitizer pre-existing leaks (AST/parser/csv/env stdlib) —
  hygiene workstream, not blocking.
- Watch: `tools/rads-mask/tests/*.rads` showed as deleted twice now
  (restored both times); find the deleter if it recurs.
