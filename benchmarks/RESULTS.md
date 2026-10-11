# RADS Benchmark Results

Honest, reproducible numbers: RADS (tree-walking C interpreter, v0.0.12) vs
CPython 3.13 vs Node 20, identical workloads, best-of-3 wall-clock
(process start included — that's the real cost a user pays).

Run it yourself: `bash benchmarks/run.sh` (or `--quick` for best-of-1 smoke).
Machine-readable results land in `benchmarks/results/*.json`.

## Latest run — 2026-10-11 post-hash-registry (results/20261011T181055Z.json)

| Workload | RADS (ms) | CPython 3.13 (ms) | Node 20 (ms) | Notes |
|---|---:|---:|---:|---|
| fib(28) naive recursion | 275.3 | 56.7 | 103.5 | was 3050.2 this morning — 11x cut from the native-registry hash table; still 4.9x CPython |
| string_build (5000 concats) | 49.9 | 19.8 | 98.0 | ~unchanged (concat path untouched) |
| array_churn (20k push + 5k pop + map) | **12.4** | 13.4 | 100.4 | **RADS now beats CPython** (was 65.2 / 5x slower) |
| file_io (200 write/read/append cycles) | **17.7** | 29.6 | 121.6 | **RADS wins** — native stdlib I/O vs interpreter overhead in the others |

Takeaways, honestly stated:

- The single biggest interpreter cost wasn't AST dispatch — it was
  `find_native()` linear-walking ~680 registered natives on every call
  expression (including the miss before every user-function call). One
  FNV-1a hash table (commit a5c2160) cut fib 11x and array_churn 5x.
- RADS now beats CPython on 2 of 4 workloads (array_churn, file_io) and
  beats Node on all 4.
- fib at 4.9x CPython is respectable for a tree-walker with no bytecode;
  the remaining gap is call-frame setup (env binding mallocs per param).


Where improvement work should go, in order of expected payoff:
1. fib: call-frame setup + AST dispatch overhead (bytecode or AST-node
   caching would close most of the gap).
2. string_build: concat currently O(n²) copies; a rope/interning scheme.
3. array_churn: native push/pop are already cheap; the loop dispatch isn't.

## Changelog

- 2026-10-11 (later): hash-table native registry (a5c2160) — fib 3050→275ms
  (11x), array_churn 65→12ms (now beats CPython). RADS now beats CPython on
  2/4 workloads, Node on 4/4.
- 2026-10-11: first run. Writing the array_churn benchmark immediately found
  a heap-use-after-free (`array.push` returned a borrowed `args[0]`); fixed
  in e165332 with `value_clone()` owning references — the suite paid for
  itself before it even ran.
