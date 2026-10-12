# RADS Autonomous State — handoff journal

Last updated: 2026-10-12 (cron run 10)
Branch: `phase0/build-recovery`
HEAD: `694de5f` perf(core): array literals + dotted calls in the BC subset (BC_MKARR, BC_CALLM)

## What this run changed

**Perf round 7, journal option 1 first — then a correction.** Pre-flight
(`RADS_BC_TRACE=1`) showed array_churn's `main` **bailing to the tree-walker
entirely**: `turbo arr = []` (array literal) and `array.push(...)` (member-
expression callee) are BOTH outside the BC subset. Option 1 (in-place slot
arithmetic for `x = x + ...`) would have optimized a loop that never ran
bytecode. So the run pivoted to widening the subset — and got the acc-loop
wins anyway, since main() now compiles.

### BC_MKARR + BC_CALLM (`694de5f`)

- `BC_MKARR`: array literals (≤32 elems) — elements compiled in source
  order, popped owned into a fresh `array_create(n)`; no re-clone.
- `BC_CALLM`: `obj.member(args)` for plain-identifier objects. Mirrors
  eval_call's member dispatch step-for-step: array push/pop method path
  (early return), string→`net.*` dispatch, then dotted-native/user-fn
  resolution on `"obj.member"` via resolve_callee_cached(cacheable=false).
  Runtime-type dispatch preserved — rebinding the object identifier
  between calls dispatches correctly in both executors.
- **Journal option 1 note**: with main() now BC-compiled, `acc = acc +
  array.pop(arr)` runs bytecode; the SETL_POP path frees the old slot
  value (int — trivial). Slot-elision is still a valid future win but is
  NOT what unlocked this round.

### Two tree-walker/BC divergences fixed (both caught by the A/B protocol)

1. **`eval_binary_op` operand order was UNSPECIFIED** — both
   `eval_expression` calls as arguments of one `binop_apply` call; gcc
   evaluated right-to-left. `"a" + arr.pop() + arr.pop()` printed
   differently under BC vs tree-walker. Fixed: sequenced locals, forced
   left-to-right (matches the BC VM and every sane language).
2. **BC_CALL leaked every heap-typed argument** (never freed its popped
   args after invoke_callee; eval_call does). Invisible on the int-only
   benchmarks, but now array/string args flow through calls. Fixed.

### Numbers (results/20261012T123323Z.json)

- array_churn **12.6 → 10.8ms (-14%)**, now **1.33x faster than CPython**
  (14.4). fib 90.9, string_build 25.2, file_io 21.5 — all steady or
  better; string_build now 1.25x from CPython parity territory.
- Suite 7/8 (known pre-existing fail). Sanitize: NO new leaks — all
  reported leaks are the pre-existing program-lifetime category (BcFunc
  cache, const strings, AST/parser/csv/env); zero UAF/UB errors.
- A/B (BC vs RADS_NO_BC=1) byte-identical on: array literals (mixed
  types), dotted push/pop, nested loops building strings from popped
  values, empty-array pop, empty literals.

## Test results

- `make`: green (pre-existing warnings only).
- `run_tests.sh`: **7/8** — known pre-existing `v0_0_7_comprehensive.rads`.
- Sanitize run on all 4 benchmarks + probes: LeakSanitizer pre-existing
  only; ASAN/UBSAN clean of memory errors.
- Benchmarks (best-of-5): fib 90.9 / string_build 25.2 / array_churn
  **10.8** / file_io 21.5 vs CPython 58.1/20.2/14.4/37.4.

## Next up (single task for next run)

**Perf round 8 candidates, pick ONE:**
1. **In-place slot arithmetic for `x = x + ...`** (still open, now
   actually reachable): compound-assign fold in BC — detect
   `AST_ASSIGN_EXPR(target=ident, value=binop(left=same ident))`, emit an
   in-place add on the slot (skip GETL-clone + SETL-free roundtrip).
   array_churn's accumulate loop and fib's `n - 1` shapes benefit.
2. **BC_GETIDX**: `arr[i]` index expressions as an opcode — eval_expression
   clones the array Value per pop... actually no, index expr is not in the
   subset at all; adding it unlocks more programs (and GETL of an array
   slot is refcount-cheap already).
3. **Slot write-back elision in cruise**: the step sequence GETL/INT/BIN/
   SETL_POP on the iterator could be a single BC_STEP opcode.

Rules unchanged: compile-bail fallback, suite 7/8 minimum, ASAN clean
(pre-existing leaks tolerated), `bash benchmarks/run.sh` before/after,
A/B with RADS_NO_BC=1, diff BC vs tree-walker output on a mixed-type probe
(mandatory — it caught both bugs this run). If stalled >10 min, fall back
to AGENTS.md workstream priority.

## Open blockers

- `git push` still fails: no credentials. **31 commits** now await push
  locally on `phase0/build-recovery` (+1 this run). Needs PAT or
  `gh auth login` by the owner (colyzeo@gmail.com).
- Main checkout still on dirty `nightshift/20261012-fix-bisect-build`
  with uncommitted stdlib work — untouched, as before.
- `v0_0_7_comprehensive.rads` unsupported syntax — rewrite or implement.
- Pre-existing gcc warnings (unused vars/fns) — cosmetic.
- `.gitignore` line 57 `core` matches `src/core` — `git add -f` needed.
- LeakSanitizer pre-existing leaks (AST/parser/csv/env + BcFunc caches) —
  hygiene workstream, not blocking.
