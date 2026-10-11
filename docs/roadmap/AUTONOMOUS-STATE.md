# RADS Autonomous State — handoff journal

Last updated: 2026-10-11 (cron run 6)
Branch: `phase0/build-recovery`
HEAD: `0b98a0f` perf(core): cache callee resolution on call nodes, borrow callee, move returns

## What this run changed

**Perf round 3 from the journal — dispatch — landed as `0b98a0f`.**
fib(28): 186 → **135.7 ms (-27%)**. RADS still beats CPython on **3/4**
workloads (fib gap now 2.3x, was 3.2x) and Node on 4/4.

Changes in `src/core/interpreter.c` (+ `ast.h`/`ast.c` cache fields):

1. **Callee-resolution cache on AST call nodes.** `eval_call` now caches
   the resolved native fn pointer or global `Environment*` binding
   directly on the call node. Repeated calls skip `find_native` (hash)
   and the frame-chain walk entirely.
   Invalidation: per-name generation counters in the intern table
   (`EnvNameEntry.gen`, bumped on any FRESH binding of that name —
   updates to existing bindings are in-place and need no bump) plus a
   global `native_gen` (bumped on every native registration).
   Soundness carve-out: names ever bound into a call frame get
   `frameable = true` and are resolved dynamically forever (a later
   local could shadow the global). Only `obj.method` full-names and
   non-frameable identifiers use the cache.
2. **Borrowed callee.** The callee `Value` is snapshotted (trivially
   copyable — `func_node` is program-lifetime AST) instead of
   `env_get` clone per call.
3. **Move semantics on returns.** `AST_RETURN_STMT` moves its value into
   `current_return_value` (was clone-then-free); the callee's
   `interpreter_execute_callback` moves it out (was clone). Halves the
   copy traffic on every return.

## Test results

- `make`: green.
- `run_tests.sh`: **7/8** — the 1 fail is the known pre-existing
  `v0_0_7_comprehensive.rads`, unchanged.
- Sanitize build (ASAN+UBSAN+LSAN) run over the suite + all
  benchmarks: **no UAF/double-free/UB** (leak noise pre-existing).
- Benchmarks (results/20261011T231003Z.json): fib 135.7 / string_build
  54.1 / array_churn 12.9 / file_io 21.6 vs CPython 58.3/20.6/14.3/35.5
  and Node 105.9/99.4/103.1/120.7.

## Next up (single task for next run)

**Perf round 4: bytecode prototype.** The tree-walk switch is now the
floor — journal options 1+2 are exhausted, option 3 remains: compile
`blast` function bodies to a compact stack bytecode (fib-shaped subset:
literals, identifiers, binary ops, calls, if, return) with a small
dispatch loop, keeping the tree-walker as fallback for everything else.
Prototype first, measure `bash benchmarks/run.sh`, keep only if fib
moves meaningfully (<100ms target). If it stalls >10min, fall back to
the workstream list in AGENTS.md priority order.

## Open blockers

- `git push` still fails: no credentials. **19 commits** now await push
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
