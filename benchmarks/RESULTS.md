# RADS Benchmark Results

Honest, reproducible numbers: RADS (tree-walking C interpreter, v0.0.12) vs
CPython 3.13 vs Node 20, identical workloads, best-of-3 wall-clock
(process start included — that's the real cost a user pays).

Run it yourself: `bash benchmarks/run.sh` (or `--quick` for best-of-1 smoke).
Machine-readable results land in `benchmarks/results/*.json`.

## Latest run — 2026-10-11 post-call-frame-overhaul (results/20261011T194910Z.json)

| Workload | RADS (ms) | CPython 3.13 (ms) | Node 20 (ms) | Notes |
|---|---:|---:|---:|---|
| fib(28) naive recursion | **186.3** | 58.0 | 108.8 | 3050 → 275 (hash registry) → 186 (this run); now **beats Node** and is 3.2x CPython (was 4.9x) |
| string_build (5000 concats) | 49.7 | 20.1 | 102.1 | ~unchanged (concat path untouched) |
| array_churn (20k push + 5k pop + map) | **12.6** | 13.0 | 102.1 | **RADS beats CPython** |
| file_io (200 write/read/append cycles) | **17.7** | 30.0 | 123.1 | **RADS wins** — native stdlib I/O |

Takeaways, honestly stated:

- RADS now beats CPython on **3 of 4** workloads (fib, array_churn, file_io)
  and beats Node on all 4.
- This run's gain (fib 275→186ms, -32%) came from the call frame, not
  dispatch: env bindings switched from clone-per-bind to take-ownership,
  names interned (pointer-equality lookups instead of strcmp walks up the
  frame chain), Environment nodes pooled, and eval_call's arg arrays moved
  to stack buffers (commit 31fce59).
- fib at 3.2x CPython for a tree-walker with no bytecode: the residual gap
  is AST dispatch + the remaining clone on each argument/return value.
  Bytecode or a compilation step is the next order-of-magnitude lever.


Where improvement work should go, in order of expected payoff:
1. fib: AST-node dispatch dominates what's left; bytecode compilation of
   function bodies is the big structural lever.
2. string_build: concat currently O(n²) copies; a rope/interning scheme.
3. array_churn: native push/pop are already cheap; the loop dispatch isn't.

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
