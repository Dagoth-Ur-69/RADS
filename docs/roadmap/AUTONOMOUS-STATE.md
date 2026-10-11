# RADS Autonomous State — handoff journal

Last updated: 2026-10-11 (cron run 5)
Branch: `phase0/build-recovery`
HEAD: `7ea4354` chore: ignore benchmark tmp_io scratch files

## What this run changed

**Perf round 2 from the journal — call-frame overhead — landed as `31fce59`.**
fib(28): 274 → **186 ms (-32%)**. RADS now beats CPython on **3/4**
workloads (fib, array_churn, file_io) and Node on 4/4.

The interpreter's environment layer was reworked (src/core/interpreter.c):
1. **Owning bindings**: `env_set`/`env_decl`/`env_list_set` now TAKE
   OWNERSHIP of the value. Previously every parameter bind cloned the arg,
   cloned again inside env_list_set, then the caller freed — 2 clones +
   3 frees per bind, ~832k times for fib(28).
2. **Interned names**: binding names live in an FNV-1a intern set;
   `env_get`/`env_get_ref` walk frames with pointer equality instead of
   strcmp chains (was O(depth × strlen) per lookup).
3. **Node pool**: Environment nodes recycle through a free-list instead of
   malloc/free per bind and per frame pop. Interned names never freed.
4. **Stack arg buffers**: eval_call's `args` arrays use an 8-slot stack
   buffer before heap fallback (all 3 call paths).

The ownership flip required fixes at every call site: identifier assignment
(clones for env since expr result must survive), cruise iterator, try/catch
catch-var, array/struct destructure (borrowed items now cloned), turbo
variable decl, and the param-bind loop. `env_free` releases to the pool.

Also: `ad0891f` RESULTS.md + 4 result JSONs committed; `7ea4354` gitignore
for benchmarks/tmp_io_*.txt scratch.

## Test results

- `make`: green (no new warnings from interpreter.c).
- `run_tests.sh`: **7/8** — the 1 fail is the known pre-existing
  `v0_0_7_comprehensive.rads` (unsupported syntax), unchanged.
- ASAN+UBSAN build (O1) run over tests/*.rads + benchmarks: **no UAF, no
  double-free, no UB reported** after the ownership surgery. (LeakSanitizer
  noise from pre-existing AST/parser/stdlib leaks — not introduced here;
  intern table + node pool intentionally live for the process.)

## Next up (single task for next run)

**Perf round 3: dispatch.** With bindings cheap, fib's remaining 3.2x gap
to CPython (186 vs 58ms) is AST-node dispatch: every statement/expression
re-walks tagged unions via eval_expression's switch, and every return value
is a fresh clone. Options in payoff order:
1. Cache `AST_IDENTIFIER` lookups: eval_call still calls `env_get(name)`
   (which clones the callee Value) on EVERY call — cache the function
   binding on the AST node (functions can't be rebound in practice;
   invalidate on env_set of a VAL_FUNCTION to a node).
2. Return-value path: `interpreter_execute_callback` clones the return
   value; make EXEC_RETURN hand ownership instead.
3. Structural (bigger): compile function bodies to a compact bytecode —
   the order-of-magnitude lever; prototype on fib's shape first.
Measure after each change (`bash benchmarks/run.sh`); revert if the number
doesn't move. Update RESULTS.md + this journal.

## Open blockers

- `git push` still fails: no credentials. **18 commits** now await push
  locally on `phase0/build-recovery`. Needs PAT or `gh auth login` by the
  owner (GitHub account colyzeo@gmail.com).
- `v0_0_7_comprehensive.rads` + several stray tests/*.rads use unsupported
  syntax (`#` comments, `fn`, `let`) — rewrite or implement; candidate work.
- Pre-existing gcc warnings (unused vars/fns in stdlib_net, functools,
  parser.c:648) — cosmetic.
- `.gitignore` line 57 `core` matches `src/core` — use `git add -u` or
  explicit file paths when staging core changes (bit again this run).
- LeakSanitizer reports pre-existing leaks (AST/parser/csv/env stdlib);
  separate hygiene workstream, not blocking.
