# RADS Benchmark Results

Honest, reproducible numbers: RADS (tree-walking C interpreter, v0.0.12) vs
CPython 3.13 vs Node 20, identical workloads, best-of-3 wall-clock
(process start included — that's the real cost a user pays).

Run it yourself: `bash benchmarks/run.sh` (or `--quick` for best-of-1 smoke).
Machine-readable results land in `benchmarks/results/*.json`.

## Latest run — 2026-10-11 (dev laptop, results/20261011T164116Z.json)

| Workload | RADS (ms) | CPython 3.13 (ms) | Node 20 (ms) | Notes |
|---|---:|---:|---:|---|
| fib(28) naive recursion | 3050.2 | 57.0 | 100.0 | RADS 54x slower than CPython — expected for a tree-walker; the optimization target |
| string_build (5000 concats) | 45.8 | 19.1 | 98.5 | |
| array_churn (20k push + 5k pop + map) | 65.2 | 13.2 | 97.3 | |
| file_io (200 write/read/append cycles) | **22.0** | 31.3 | 118.6 | **RADS wins** — native stdlib I/O vs interpreter overhead in the others |

Takeaways, honestly stated:

- RADS beats Node on 3 of 4 workloads here (Node's ~90 ms process startup
  dominates small workloads — same effect would hit RADS if its startup
  weren't ~1 ms).
- CPython is 5–10x faster on compute-bound loops; that is the cost of a pure
  tree-walking interpreter with no bytecode, no inline caches. fib is the
  worst case (~54x) because every call frame is heap-allocated AST dispatch.
- RADS wins file I/O: the workload is dominated by syscalls, which go
  straight through native `io.*` functions.

Where improvement work should go, in order of expected payoff:
1. fib: call-frame setup + AST dispatch overhead (bytecode or AST-node
   caching would close most of the gap).
2. string_build: concat currently O(n²) copies; a rope/interning scheme.
3. array_churn: native push/pop are already cheap; the loop dispatch isn't.

## Changelog

- 2026-10-11: first run. Writing the array_churn benchmark immediately found
  a heap-use-after-free (`array.push` returned a borrowed `args[0]`); fixed
  in e165332 with `value_clone()` owning references — the suite paid for
  itself before it even ran.
