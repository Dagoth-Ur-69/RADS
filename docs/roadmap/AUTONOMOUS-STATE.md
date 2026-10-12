# RADS Autonomous State — handoff journal

Last updated: 2026-10-12 (cron run 11)
Branch: `phase0/build-recovery`
HEAD: `6e34014` perf(round 8): BC_SLOT_BIN — in-place slot arithmetic for x = x <op> rhs

## What this run changed

**Perf round 8, journal option 1 — exactly as planned this time.** New
opcode `BC_SLOT_BIN` (imm: u16 slot, u8 op): `local[slot] =
local[slot] <op> TOS` as one in-place read-modify-write, replacing the
GETL(clone)/BC_BIN/SETL_POP(free-old) round-trip for `x = x + rhs`
shapes (+ - * / %) where x is a slotted local and the binop's LEFT
operand is that same identifier.

### Design invariants (why it can't diverge)

- In-place lanes re-implement binop_apply's numeric cases exactly:
  int/int for all five ops **including div/mod-by-zero → null**
  (slot type flips to null, then null+1 → null — matches tree-walker);
  float/float **ADD only** (binop_apply has no float SUB/MUL/…).
- Every other shape (strings, mixed types, bools) goes generic: clone
  the slot exactly like BC_GETL, run the SAME binop_apply as BC_BIN,
  free old, store. Bit-identical by construction.
- **No reassociation**: only `x = x <op> rhs` with x as the left atom
  folds; `t = t + "x" + i` (subtree root is t+"x") falls through —
  folding it would change semantics in a null-propagating language.
- Globals never fold (bc_slot_of < 0 → SETG path); `x = x op x` can't
  alias (locals aren't in env, rhs can't mutate the slot).
- Cruise increment folded too: `INT 1; SLOT_BIN i` replaces
  GETL/INT/BIN/SETL_POP (4 ops → 2, no push/pop at all).

### Verification (mandatory gauntlet, all green)

- **A/B byte-identical** (BC vs RADS_NO_BC=1) on a 17-output probe
  covering: all 5 int ops, div/mod-by-zero→null then null+1→null,
  float ADD and float SUB→null, string concat via generic lane,
  bool/null operands, global (non-folded) path, param slot folds,
  side-effect rhs (`acc = acc + array.pop(arr)`), type-change slots
  (int→string mid-flight), chained folds, nested string-building loop.
- Suite **7/8** (known pre-existing `v0_0_7_comprehensive.rads`).
- ASAN/UBSAN: zero memory errors. LeakSanitizer reports are the
  pre-existing program-lifetime BC-cache class — **proven** by a
  control probe with zero fold patterns leaking identically at the
  same bc_compile/bc_const_idx/bc_put sites; SLOT_BIN allocates
  nothing in its fast lanes.
- Clean -O2 rebuild after sanitize (the run-8 trap stays shut).

### Numbers (results/20261012T155059Z / 155114Z)

- Fresh same-session baseline BEFORE the change: string_build 30.8,
  array_churn 11.0, fib 92.9, file_io 21.9.
- AFTER: **string_build 26.2/27.1 (-13..15%)** (5000-iter cruise
  increment fold), **array_churn 10.6 (-4%)** (acc fold + increment;
  1.36x faster than CPython 14.4). fib 92.7/91.9, file_io 22.7/22.0 —
  no folds present, unchanged within noise (the controls).
- Note: run-10 recorded string_build 25.2 — cross-session machine
  variance; only same-session before/after is a valid comparison.

## Test results

- `make`: green (pre-existing warnings only). Suite 7/8 as above.
- Benchmarks (after): fib 92.7 / string_build 26.2 / array_churn
  **10.6** / file_io 22.7 vs CPython 58.8/20.7/14.4/36.6.

## Next up (single task for next run)

1. **BC_GETIDX** (journal carry-over): `arr[i]` index expressions as
   an opcode — index expr is not in the subset at all today; adding it
   widens the subset (more programs compile) more than another
   micro-fold would. Clone semantics must mirror eval_expression's
   index path exactly (A/B with mixed-type arrays mandatory).
2. Alternative: fold `if (x <op> imm)` compare-and-branch into one
   opcode (BC_CMPF) — fib's `n < 2` runs per call; smaller win.
3. Housekeeping option: exit-time BcFunc cache free (turn LSan leaks
   into clean reports) — pure hygiene, unlocks stricter CI later.

Rules unchanged: compile-bail fallback, suite 7/8 minimum, ASAN clean
(pre-existing leaks tolerated), `bash benchmarks/run.sh` before/after
(same session only), A/B with RADS_NO_BC=1, diff BC vs tree-walker
output on a mixed-type probe (mandatory). If stalled >10 min, fall
back to AGENTS.md workstream priority.

## Open blockers

- `git push` still fails: no credentials. **36 commits** now await
  push locally on `phase0/build-recovery` (+1 this run). Needs PAT or
  `gh auth login` by the owner (colyzeo@gmail.com).
- Main checkout still on dirty `nightshift/20261012-fix-bisect-build`
  with uncommitted stdlib work — untouched, as before.
- `v0_0_7_comprehensive.rads` unsupported syntax — rewrite or implement.
- Pre-existing gcc warnings (unused vars/fns) — cosmetic.
- `.gitignore` line 57 `core` matches `src/core` — tracked files add
  fine, but `git add` of NEW files under src/core needs `-f`.
- LeakSanitizer pre-existing leaks (AST/parser/csv/env + BcFunc caches)
  — hygiene workstream, not blocking.
