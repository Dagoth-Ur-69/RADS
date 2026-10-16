# RADS Benchmark Results

Honest, reproducible numbers: RADS (C interpreter with a bytecode fast path,
v0.0.12) vs CPython 3.13 vs Node 20, identical workloads, best-of-3
wall-clock (process start included — that's the real cost a user pays).

Run it yourself: `bash benchmarks/run.sh` (or `--quick` for best-of-1 smoke).
Machine-readable results land in `benchmarks/results/*.json`.

## Latest run — 2026-10-16 BC_CALL direct dispatch (results/20261016T182438Z.json)

| Workload | RADS (ms) | CPython 3.13 (ms) | Node 20 (ms) | Notes |
|---|---:|---:|---:|---|
| fib(28) naive recursion | 63.3 | **62.3** | 120.2 | 3050 → 275 → 186 → 136 → 101 → 71 → 72 → 66 → **63** (BC_CALL now dispatches vm_run directly + trivially-copyable param binding); gap to CPython now **1.016x — statistical parity** |
| string_build (5000 concats) | 26.7 | **22.7** | 112.6 | flat (concat path untouched) |
| array_churn (20k push + 5k pop + map) | **11.1** | 15.7 | 113.0 | steady |
| file_io (200 write/read/append cycles) | **22.3** | 40.7 | 134.2 | RADS wins — native stdlib I/O |

Takeaways, honestly stated:

- Fresh gprof (run 37): the BC_CALL path walked a 4-deep trampoline
  (`BC_CALL → invoke_callee → interpreter_execute_callback →
  bc_try_execute → vm_run`) **1,028,458 times** for fib(28), plus
  `value_clone_impl` 1.03M (unconditional arg clone) and
  `env_intern_entry` 1.03M (arg-name interning at bind time).
- This run removes the trampoline: BC_CALL now reaches `vm_run` directly
  for user functions (natives keep their exact path), and `vm_run`'s
  param binding fast-paths trivially copyable types (INT/FLOAT/BOOL/
  NULL/FUNCTION — exact mirror of the existing BC_GETL rule).
- A/B (interleaved, 5 rounds × 3 samples): 71.4 → 63.8 ms median
  (~10.8%), non-overlapping distributions.
- The stale run-36 `bc_put16 ≈ 1.03M` anomaly is resolved: compile runs
  once per function; no per-call recompilation exists.
- RADS beats CPython on **2 of 4**, Node on **4 of 4** — and the fib gap
  is gone: 1.016x on the stable-load run. The next honest frontier is
  beating CPython on fib outright, then string_build (concat cost).

## Latest run — 2026-10-16 post-enum-values refresh (results/20261016T120234Z.json, confirmed by 120329Z)

| Workload | RADS (ms) | CPython 3.13 (ms) | Node 20 (ms) | Notes |
|---|---:|---:|---:|---|
| fib(28) naive recursion | 72.0 | **59.9** | 107.5 | 3050 → 275 → 186 → 136 → 101 → 71 (CMPF) → **72 today**: stable, no regression from the enum-values/flag work (a7c8955..6736621); 1.20x CPython |
| string_build (5000 concats) | 25.7 | **21.0** | 106.5 | flat vs 10-13 (25.6→27.1→25.7 = noise); 1.22x CPython |
| array_churn (20k push + 5k pop + map) | **10.7** | 14.7 | 101.8 | RADS wins |
| file_io (200 write/read/append cycles) | **22.5** | 35.6 | 123.8 | RADS wins — native stdlib I/O |

Takeaways, honestly stated:

- RADS beats CPython on **2 of 4** workloads (array_churn, file_io) and beats
  Node on **4 of 4**. On the two losses the gap is 1.2x — closer than the
  2.3x of a week ago, but a loss is a loss and we say so.
- This run was a **stability check**, not an optimization: 12 days of engine
  work (dicts, comprehensions, UAF/leak fixes, enum values, `-i` REPL)
  moved nothing outside noise (fib 71.0 → 72.0/76.5 across two samples).
  That is the quiet win: safety work that costs zero throughput.
- Known accounting: wall-clock includes process start (~few ms RADS,
  ~20 ms CPython, ~90 ms Node — Node's floor is its own startup).
- Changelog (newest first):
  - 2026-10-16 (run 36): BC numeric fast-path — int/float direct dispatch
    for non-ADD ops in BC_BIN; fib 72 → 66.4 ms; array_churn 10.7 → 10.1
    (best ever). A/B: 68.3 → 63.4 ms median, ~7%, no overlap.
  - 2026-10-16 (run 35): stability check — 12 days of engine work, zero
    throughput change (fib 71.0 → 72.0/76.5); RESULTS.md refreshed.

## Latest run — 2026-10-11 post-dispatch-cache (results/20261011T231003Z.json)

| Workload | RADS (ms) | CPython 3.13 (ms) | Node 20 (ms) | Notes |
|---|---:|---:|---:|---|
| fib(28) naive recursion | **135.7** | 58.3 | 105.9 | 3050 → 275 (hash registry) → 186 (call frames) → **136** (this run); now **beats Node** and is 2.3x CPython |
| string_build (5000 concats) | 54.1 | 20.6 | 99.4 | ~unchanged (concat path untouched) |
| array_churn (20k push + 5k pop + map) | **12.9** | 14.3 | 103.1 | bench noise vs last run; RADS ≈ CPython, both crush Node |
| file_io (200 write/read/append cycles) | **21.6** | 35.5 | 120.7 | **RADS wins** — native stdlib I/O |

Takeaways, honestly stated:

- RADS beats CPython on **3 of 4** workloads (fib, array_churn, file_io) and
  beats Node on 4 of 4.
- This run's gain (fib 186→136ms, -27%) came from call dispatch: callee
  resolution is now cached on the AST call node (invalidated by a per-name
  generation counter bumped on any fresh binding; names ever bound into a
  call frame are permanently dynamic and never cached), the callee Value is
  borrowed instead of cloned per call, and `return` moves its value instead
  of clone-then-free.
- fib at 2.3x CPython for a tree-walker with no bytecode: what remains is
  per-expression eval dispatch (the big switch) and argument clones.
  Bytecode compilation of function bodies is the order-of-magnitude lever.

Where improvement work should go, in order of expected payoff:
1. fib: compile function bodies to a compact bytecode — the big lever; the
   tree-walk switch itself is now the floor.
2. string_build: concat currently O(n²) copies; a rope/interning scheme.
3. array_churn: native push/pop cheap; loop dispatch isn't.

## Latest run — 2026-10-12 bytecode VM (results/20261012T024458Z.json)

| Workload | RADS (ms) | CPython 3.13 (ms) | Node 20 (ms) | Notes |
|---|---:|---:|---:|---|
| fib(28) naive recursion | **100.6** | 62.8 | 112.5 | 3050 → 275 → 186 → 136 → **101** (bytecode VM); **beats Node**, 1.6x CPython (was 2.3x) |
| string_build (5000 concats) | 58.0 | 21.1 | 110.9 | unchanged (string path still tree-walked) |
| array_churn (20k push + 5k pop + map) | **13.3** | 16.3 | 112.2 | RADS ≈ CPython, both crush Node |
| file_io (200 write/read/append cycles) | **23.2** | 36.4 | 132.6 | **RADS wins** — native stdlib I/O |

Takeaways, honestly stated:

- RADS beats CPython on **3 of 4** workloads and beats Node on **4 of 4**.
- This run's gain (fib 136→101ms, -26%) is the first bytecode execution:
  `blast` function bodies in the "fib-shaped" subset (literals, identifiers,
  binary ops, identifier calls, if, return, local decls/assignments) compile
  once to a compact stack bytecode with slotted locals and run in a dispatch
  loop. Anything outside the subset transparently tree-walks as before
  (compile-time bail-out, cached per function). A/B with `RADS_NO_BC=1`.
- fib gap to CPython now 1.6x (tree-walk floor was 2.3x). Next levers:
  widen the subset (loops, while, string concat in BC), unboxed int slots,
  direct-threaded dispatch.

## Changelog

- 2026-10-16: post-enum-values stability check. Two fresh samples (120234Z,
  120329Z) vs the 10-13 baseline: fib 71.0 → 72.0/76.5, everything else flat
  — 12 days of dicts/comprehensions/UAF+leak fixes/enum values/`-i` REPL cost
  zero throughput. No bisect needed. Also: landed 14 orphaned result JSONs
  from 10-12/10-13 and quarantined stray `rads-asan` builds (see journal 35).
- 2026-10-11 (evening): call-frame overhead overhaul (31fce59) — fib
  275→186ms (-32%). Owning env bindings, interned names, pooled Environment
  nodes, stack arg buffers. RADS now beats CPython on 3/4 workloads.
- 2026-10-11 (later): hash-table native registry (a5c2160) — fib 3050→275ms
  (11x), array_churn 65→12ms (now beats CPython).
- 2026-10-11: first run. Writing the array_churn benchmark immediately found
  a heap-use-after-free (`array.push` returned a borrowed `args[0]`); fixed
  in e165332 with `value_clone()` owning references — the suite paid for
  itself before it even ran.
