# RADS Autonomous State — handoff journal

Last updated: 2026-10-12 (cron run 4 of the eternal dev loop)
Branch: `phase0/build-recovery`
HEAD: `5bd4b50` bench: add RADS vs CPython vs Node benchmark suite

## What this run changed

**Run focus: Phase 1 workstream 1 (benchmark suite) — delivered, and it
found another showstopper correctness bug on the way (second run in a row
the benchmarks have paid for themselves).**

1. `e165332` — **fix(core): natives must return owning refs (use-after-free).**
   The array_churn benchmark segfaulted: `array.push` did `return args[0];`
   but `eval_call` frees all args after a native returns — the freed Array*
   stayed reachable through the original variable binding. Diagnosed with an
   ASAN build (`gcc -fsanitize=address`, needed `-lcurl -luv -lsqlite3
   -lreadline`). Fix: exported `value_clone()` from interpreter.c (was
   static) and converted every borrowed-return native: array.push,
   sort/sort.desc/sort.reverse/sort.shuffle (+ their empty-array paths),
   functools.identity, test.expect, collections.peek, random.choice,
   secrets.choice. ASAN clean on all repros; `tests/test_array_uaf.rads`
   added + wired into run_tests.sh.
2. `5bd4b50` — **bench: benchmark suite.** `benchmarks/{rads,python,node}/`
   (fib, string_build, array_churn, file_io — identical workloads),
   `run.py` runner (best-of-3 wall clock incl. process startup, JSON
   results, loud failure instead of fake numbers), RESULTS.md with first
   recorded run + ranked optimization targets. Hash-churn workload adapted
   to arrays (no dict type in RADS yet).

### Benchmark facts (2026-10-11 run, results/20261011T164116Z.json)
- fib(28): RADS 3050ms vs PY 57ms vs Node 100ms (tree-walker worst case).
- string_build: 45.8 / 19.1 / 98.5. array_churn: 65.2 / 13.2 / 97.3.
- file_io: RADS **22.0ms beats both** (PY 31.3, Node 118.6) — native io.*
  syscalls beat interpreter overhead. RADS beats Node on 3/4 overall
  (Node's ~90ms startup dominates small workloads).

### Language facts learned (verified against the real binary)
- `array<i32> x` generic-decl syntax does NOT parse; use `turbo arr = [];`.
- `array.map(arr, fn)` takes a function VALUE (named fn or `blast(x){}`),
  not a string. `len()` builtin does not exist; use `array.count(arr)`.
- RADS process startup is ~1-20ms — cheap enough that per-run timing is fair.

## Test results

- `make`: green. `run_tests.sh`: **7/8 pass** (was 6/7; +1 array UAF test).
  The 1 fail is still `v0_0_7_comprehensive.rads` (never-supported syntax)
  — pre-existing, not a regression.
- ASAN build clean on all UAF repro cases + array_churn benchmark.

## Next up (single task for next run)

**Phase 1 workstream 1 continues: benchmark optimization round.** The
suite exists; now attack the worst number honestly: fib 3050ms vs CPython
57ms. Cheap tree-walker wins first — profile `interpreter_execute_callback`
(call-frame env alloc per call) and eval dispatch; measure after each
change with `bash benchmarks/run.sh`; keep RESULTS.md updated with before/
after. Do NOT fake wins: if a change doesn't move the number, revert it.

## Open blockers

- `git push` still fails: no credentials. **13 commits** now await push
  locally on `phase0/build-recovery`. Needs PAT or `gh auth login` by the
  owner (GitHub account colyzeo@gmail.com).
- `v0_0_7_comprehensive.rads` + several stray tests/*.rads use unsupported
  syntax (`#` comments, `fn`, `let`) — rewrite or implement; candidate work.
- Pre-existing gcc warnings (unused vars/fns in stdlib_net, functools,
  parser.c:648 `update_block`) — cosmetic.
- `.gitignore` line 57 `core` matches `src/core` on directory add — use
  `git add -u` or explicit file paths when staging core changes.
