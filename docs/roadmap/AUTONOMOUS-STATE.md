# RADS Autonomous State — handoff journal

Last updated: 2026-10-13 (cron run 14)
Branch: `phase0/build-recovery`
HEAD: see `git log -1` — perf(round 10): BC_CMPF fused compare-and-branch

## What this run changed

**Run 14 = journal "Next up" #1: BC_CMPF, shipped.** `if/loop` conditions
of the shape `local <cmp> int-literal` now compile to a single fused
compare-and-branch opcode instead of GETL+INT+BIN+JMPF — zero stack traffic,
no value clone. All changes in `src/core/interpreter.c` (+ regression test):

- New opcode `BC_CMPF` (enum + handler): imm = u16 slot, u8 op, i64 rhs,
  u16 absolute jump offset. `bc_try_cmpf()` peeks the condition AST before
  the generic compile — pure slot lookup (no slot creation), so any
  non-matching shape falls back to the exact old path unchanged.
- Emission sites: `AST_IF_STMT` and `AST_LOOP_STMT` (while-form) conditions.
  The cruise range-loop cond is slot-vs-slot (GETL/GETL/BIN LT/JMPF) — not
  the literal shape, intentionally left generic.
- Semantics mirror `binop_apply` + `bc_truthy` exactly: int slot → raw cmp;
  EQ mixed-type → false (jump); NEQ mixed-type → true (no jump); LT/LTE/
  GT/GTE on any non-int slot → null → falsy → jump. Skipping the unfused
  path's clone is observationally identical (the clone was always freed by
  BC_JMPF; the slot is only read).

### Verification (mandatory gauntlet, all green)

- **A/B byte-identical**: p1 probe (16 outputs: all 6 ops both ways, loop
  fusion, float/string/null slots, EQ/NEQ mixed, negative + large literals,
  else-if chains) — BC ≡ NOBC, zero diff. fib.rads BC ≡ NOBC.
- **ASAN clean** (detect_leaks=0): p1 + p2-hot + regression test, rc=0,
  zero hits.
- **Timing differential (positive compile proof, pre-patch vs post-patch
  binaries, median of 5)**: fib 0.099→0.075s = **1.32×**; compare-heavy
  probe (5 fused cmps/iter, 200×5000×10) 1.103→0.452s = **2.44×**.
  (Raw BC-vs-NOBC on the same probe: ~7.9× — all 10 rounds compounded.)
- **Suite**: failure list IDENTICAL pre/post (9 pre-existing stdlib files:
  bisect/http/itertools/math/string/struct/time/v0.0.12/v0_0_7_comprehensive).
  Zero new regressions.
- **New regression test**: `tests/test_bc_cmpf.rads` (5 shapes: all ops both
  branches, loop fusion, non-int folds, negative/large literals, nested
  loops + else-if chains) — PASS, BC ≡ NOBC, ASAN-clean.
- Full benchmarks (results/20261013T050701Z): fib **90.8→71.0ms**, others
  flat (string_build 27.1 / array_churn 11.0 / file_io 22.1 — noise).

### Method notes (for future rounds)

- Pre/post patch binaries built via `git diff > file; git checkout --; make;
  cp; git apply; make` — clean isolated differential WITHOUT touching .git
  internals (stash included; tracked file only).
- Scratch dir `/home/zari/.hermes/cache/scratch/rads-r14/` held rads-pre,
  rads-post, rads-asan (parallel builds, per run-13 lesson).
- RADS grammar notes hit this run: while-spelling is `loop(...)` (the lexer
  aliases "while"→"loop" but the parser wants `loop`); `turbo f64`/`str`
  type annotations don't parse — use bare `turbo x = 1.5;`.

## Next up (single task for next run)

1. **BC_CMPF for cruise range-loops** (slot-vs-slot variant, `bc_try_cmpf2`):
   `cruise (i in a..b)` cond is GETL/GETL/BIN(LT)/JMPF — fuse to one opcode
   (u16 slotA, u16 slotB, u8 op, u16 off) with the same mixed-type folds.
   Applies to every range loop incl. benchmark harness loops.
2. Exit-time BcFunc cache free (LSan hygiene).
3. `v0_0_7_comprehensive.rads` unsupported syntax — rewrite or implement.

Rules unchanged: compile-bail fallback, suite no-new-failures minimum,
ASAN clean (pre-existing leaks tolerated), `bash benchmarks/run.sh`
before/after (same session only), A/B with RADS_NO_BC=1, diff BC vs
tree-walker output on a mixed-type probe (mandatory). If stalled >10 min,
fall back to AGENTS.md workstream priority.

## Open blockers

- `git push` still fails: no credentials. **39 commits** now await
  push locally on `phase0/build-recovery` (+1 this run). Needs PAT or
  `gh auth login` by the owner (colyzeo@gmail.com).
- Main checkout still on dirty `nightshift/20261012-fix-bisect-build`
  with uncommitted stdlib work — untouched, as before.
- Pre-existing gcc warnings (unused vars/fns) — cosmetic.
- `.gitignore` line 57 `core` matches `src/core` — tracked files add
  fine, but `git add` of NEW files under src/core needs `-f`.
- LeakSanitizer pre-existing leaks (AST/parser/csv/env + BcFunc caches)
  — hygiene workstream, not blocking.
- Sanitize-build stdout swallow (banner only, exit 0) — pre-existing,
  cosmetic; use a parallel -O2 build for output checks (this run kept
  `rads-asan` + `rads-o2` side by side in the scratch dir — do that).

## Run log (one line each)

- run 11: BC_GETIDX planned, compile-bail lesson learned.
- run 12: BC_GETIDX shipped (round 9), 2.1× timing proof, index UAF
  discovered (pre-existing), 37 commits pending push.
- run 13: index UAF root-caused (share-no-bump at the tail free) and
  fixed in tree-walker + BC_GETIDX; regression test added; 38 commits
  pending push.
- run 14: BC_CMPF shipped (round 10, fused compare-and-branch); fib
  1.32× / cmp-heavy 2.44× pre-vs-post; regression test added; 39 commits
  pending push.

---

## Run 15 — 2026-10-14: unification + first-ever push + PR

**What changed:**
- Merged `nightshift/20261013-stdlib-float-parity` INTO `phase0/build-recovery`
  (conflicts resolved: heapq/logging/statistics take the nightshift DOTTED-name
  versions — `heapq.heapify` style — because the dotted-call resolver builds
  `module.fn` names; underscore variants could never match. `make_error`
  duplicate removed).
- `fix(interp)`: mixed int/float arithmetic completed in `binop_apply` —
  OP_ADD lacked int+float (1+0.5 → null); OP_LT/LTE/GT/GTE also dropped mixed
  operands. All widen to double now.
- `run_tests.sh`: union of both test sets (12 files) + off-by-one total fix.

**Test results:** clean rebuild RC=0; suite 11/11 green (v0_0_7_comprehensive
restored); float spot-check 1+0.5=1.5, sin(π/2)=1.0, cos(π/2)=0.0.

**PUSH DOCTRINE (new, permanent):**
- `origin` (zarigata/RADS) is READ-ONLY for this agent (403).
- Writable remote: `fork` = https://github.com/Dagoth-Ur-69/RADS.git (gh auth:
  Dagoth-Ur-69, `gh auth setup-git` already done).
- After committing: `git push fork phase0/build-recovery`. Never force-push.
- Upstream PRs: `gh pr create --repo zarigata/RADS --head
  Dagoth-Ur-69:phase0/build-recovery ...` — PR #19 open with runs 1-14 + this.
- Worktree: `/home/zari/Desktop/RADS-wt-bc`, branch `phase0/build-recovery`.
- NOTE: `git add` of NEW files under `src/core` needs `-f` (.gitignore line 57
  `core` matches the dir; tracked files add fine).

**Next up (run 17):** continue the pythonic-surface push — **optional
semicolons**. `test_math.rads` line 7 (`println(...)` without `;`) is the
next parse blocker; test_string wants paren-free `echo`. Parser change:
after an expression statement, accept end-of-line as terminator when `;`
is absent AND the next token starts a new statement (identifier/keyword/
`}`). Keep `;` fully supported — this is additive, not a swap. Then
re-run the gauntlet + red files; target: test_math parsing end-to-end.

## Session hygiene law (all runs)
One task per run, then journal + commit + push + END. Context bloat kills
the loop (run 16 hit 36k tokens, compaction exhausted). Concretely:
- Cap read_file at 60 lines; prefer `sed -n 'A,Bp'` with tight ranges.
- No full-suite logs into context — `bash run_tests.sh 2>&1 | tail -5`.
- One build per run max; verify with 2-3 probe files via `./bin/rads`.
- If a context-pressure warning appears: STOP work, write journal, end run.

## Run log (continued)
- run 16: `#` line comments + `let` alias shipped (commit e97f80c, pushed
  to fork). Gauntlet 13/13 (restored missing test_stream.rads — runner
  had a phantom 12th entry; new test_pythonic_syntax.rads). test_itertools
  flipped red→green as a side effect. 8 red files remain, all blocked on
  optional semicolons / paren-free echo / dotted stdlib calls.
- run 15: nightshift unified into phase0, mixed-float arithmetic fixed,
  45 commits pushed to fork Dagoth-Ur-69/RADS, upstream PR #19 open.
