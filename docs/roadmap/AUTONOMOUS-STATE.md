# RADS Autonomous State — handoff journal

Last updated: 2026-10-12 (cron run 9)
Branch: `phase0/build-recovery`
HEAD: `c7fa2ed` perf(core): single-pass string concat + OP_ADD fast paths in the BC VM

## What this run changed

**Perf round 6, journal option 1 (string concat inline) — landed as
`c7fa2ed`, plus a latent BC bug found and fixed along the way (`1b4fe73`).**

Work continues in the worktree (`/home/zari/Desktop/RADS-wt-bc`); the main
checkout was still on dirty `nightshift/20261012-fix-bisect-build` with
uncommitted stdlib work — untouched again. Same note stands for next run.

### Float literals were TRUNCATED in the BC VM (`1b4fe73`)

`bc_compile` emitted `(uint64_t)double` — a VALUE cast, not a bit-pattern
reinterpret — so every float literal in a bytecode-compiled body became its
truncated integer (0.5 -> 0). Present since round 4 (BC VM introduction);
invisible because all four benchmarks are int-only. Caught by a run-9
mixed-type concat probe that compared BC vs tree-walker output line by
line. Fixed via memcpy of the raw bits. Lesson recorded: A/B output diff
between executors is now part of the perf protocol.

### String concat fast paths (`c7fa2ed`)

- `binop_apply` concat tail: was strcpy+strcat+make_string(strdup)+free
  (four passes over the LHS per binop); now one malloc + two memcpys,
  Value built in place. Helps the tree-walker too.
- `BC_BIN` OP_ADD fast paths: int/int, float/float, str/str, str/int,
  int/str skip binop_apply entirely (no 256-byte stack-buffer dance, no
  intermediate copies; popped string operands freed in place).

- Numbers (run-9 baseline vs after): string_build 56.5 -> **29.1ms
  (-48%)**, fib 98.2 -> **89.5** (int/int fast path), array_churn 12.6,
  file_io 23.1. Suite 7/8 (known pre-existing fail), ASAN+UBSAN clean.
- BC output verified byte-identical to tree-walker on mixed-type concat
  probes (str+int, str+float via call args, bool+str, doubling loop).
- Build trap hit (again): `make sanitize` depends on `clean` — running it
  mid-sequence wipes bin/rads, and a following plain `make -j4` sees the
  ASAN binary as up-to-date so benchmarks silently run the slow sanitizer
  build (RADS=None). Always `make clean && make -j4` after sanitize.


### Loops in the BC subset (`30f255f`)

- `cruise (i in a..b)` range loops, `loop (cond)` while-loops, `break`,
  `continue` now compile to bytecode. break/continue are plain BC_JMPs
  recorded in per-loop patch lists, fixed up when the loop closes — correct
  through nested ifs and nested loops (inner loop closes first and claims
  its own sites).
- cruise bounds: start/end evaluated once into slots; `$range_end$` hidden
  slot (interned name no identifier can produce). Condition via
  binop_apply OP_LT — bit-identical tree-walker semantics, verified by A/B
  (RADS_NO_BC=1) on nested-loop/break/continue/empty-range/while tests.
- **`echo` statement added to the subset (BC_ECHO)** — the trap of the run:
  every benchmark main() ends in `echo(...)`, so without it loop
  compilation NEVER fired for the benchmarks (main bailed to tree-walk).
  Found via new `RADS_BC_TRACE=1` env flag (prints compile-bail fn names).
- **`main()` now routes through `interpreter_execute_callback`** instead of
  raw exec_statement — its body compiles to bytecode like any blast
  function. Global-scope semantics unchanged (no params, globals visible).
- Numbers (best-of-5 wall): 2M-iter cruise micro 187→**158ms (-15%)**;
  official suite: fib 100.6→95.8, string_build 58.0→**53.5**,
  array_churn 13.3→12.8, file_io 23.2→21.6. array_churn gains are modest
  because its inner work is native `array.*` calls, not loop arithmetic.
- Suite 7/8 (known pre-existing fail), ASAN+UBSAN clean on all workloads
  and loop stress tests.

### rads-mask deleter solved (`04c1ac3`)

Journal runs 6/7 flagged `tools/rads-mask/tests/*.rads` showing as deleted.
Root cause: `tools/rads-mask/Makefile` clean target had `rm -f tests/*.rads`
(intended to clear GENERATED transpiler outputs), and the root Makefile's
`clean` cascades `make -C tools/rads-mask clean` — so every `make clean`
wiped tracked fixtures. Fixed at source; verified `make clean` now leaves
zero git deletions.

## Test results

- `make`: green (pre-existing stdlib warnings only).
- `run_tests.sh`: **7/8** — the 1 fail is the known pre-existing
  `v0_0_7_comprehensive.rads` (await-in-nested-scope parser gap), unchanged.
- Sanitize build (ASAN+UBSAN+LSAN): all 4 benchmarks + loop stress tests
  clean (leak noise pre-existing).
- Benchmarks (results/20261012T055804Z.json): fib **95.8** / string_build
  **53.5** / array_churn **12.8** / file_io **21.6** vs CPython
  58.3/20.6/14.4/35.8 and Node 105.0/101.0/103.2/124.8.

## Next up (single task for next run)

**Perf round 7 candidates, pick ONE:**
1. **In-place slot arithmetic for `x = x + ...`** (round-6 leftover): avoid
   clone/free of the LHS slot on compound assignment; array_churn's
   accumulate loop is the target. string_build at 29.1 vs CPython 20.3 is
   now within 1.45x — diminishing returns, but slot-elision still applies.
2. **Widen subset: index/member expressions** (`arr[i]`, `arr.length`) as
   opcodes — array_churn's `array.push/pop` call overhead is next wall.
3. **`len()` / `str.contains` native call overhead**: BC_CALL resolves
   through call-site cache every iteration; a BC_LENC opcode could skip
   invoke_callee for the single-arg native case.

Rules unchanged: compile-bail fallback, suite 7/8 minimum, ASAN clean,
`bash benchmarks/run.sh` before/after, A/B with RADS_NO_BC=1, and (new,
mandatory since run 9) **diff BC vs tree-walker output line-by-line on a
mixed-type probe** — that check is what caught the float-literal bug.
If it stalls >10 min, fall back to the AGENTS.md workstream priority.

## Open blockers

- `git push` still fails: no credentials. **30 commits** now await push
  locally on `phase0/build-recovery` (+2 this run). Needs PAT or
  `gh auth login` by the owner (GitHub account colyzeo@gmail.com).
- `v0_0_7_comprehensive.rads` uses unsupported syntax (`await` in nested
  scope, `#`, `fn`, `let`) — rewrite or implement; candidate work.
- Pre-existing gcc warnings (unused vars/fns in stdlib_net, functools,
  parser.c:648) — cosmetic.
- `.gitignore` line 57 `core` matches `src/core` — `git add -f` for
  core changes (hit again this run, expected).
- LeakSanitizer pre-existing leaks (AST/parser/csv/env stdlib) —
  hygiene workstream, not blocking.
- ~~rads-mask tests deleted by clean~~ — SOLVED this run (`04c1ac3`).
- `benchmarks/rads/` was untracked in git (run.sh referenced it, files
  only existed in the dirty main checkout) — committed this run. Watch
  that the nightshift branch doesn't also carry a conflicting copy.
