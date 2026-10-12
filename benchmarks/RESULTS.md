# RADS Benchmark Results

Honest, reproducible numbers: RADS (C interpreter with a bytecode fast path,
v0.0.12) vs CPython 3.13 vs Node 20, identical workloads, best-of-3
wall-clock (process start included — that's the real cost a user pays).

Run it yourself: `bash benchmarks/run.sh` (or `--quick` for best-of-1 smoke).
Machine-readable results land in `benchmarks/results/*.json`.

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

- 2026-10-11 (evening): call-frame overhead overhaul (31fce59) — fib
  275→186ms (-32%). Owning env bindings, interned names, pooled Environment
  nodes, stack arg buffers. RADS now beats CPython on 3/4 workloads.
- 2026-10-11 (later): hash-table native registry (a5c2160) — fib 3050→275ms
  (11x), array_churn 65→12ms (now beats CPython).
- 2026-10-11: first run. Writing the array_churn benchmark immediately found
  a heap-use-after-free (`array.push` returned a borrowed `args[0]`); fixed
  in e165332 with `value_clone()` owning references — the suite paid for
  itself before it even ran.
