# RADS Autonomous State — handoff journal

Last updated: 2026-10-15 (cron run 29)
Branch: `phase0/build-recovery`
HEAD: f051c04 — fix(memory): stdlib_struct leak sweep (push-then-free)

## What this run changed

**Run 29 = journal "Next up": the stdlib_struct.c leak sweep.** Dead.
Scratch-first (rads-r29/build): repro'd exactly 146 B / 7 allocs on
test_struct.rads, mapped ownership with minimal probes, applied the
Run 23 json precedent (`array_push` then `value_free`) at **4 sites**,
then ported the identical patch to the worktree.

1. Root cause: `array_push` stores `value_clone_impl(v)` — refcount++
   for arrays, strdup for strings — so `array_push(arr, f())` orphans
   f()'s return. Sites fixed in `src/stdlib/stdlib_struct.c`:
   - `:375` unpack — parse_value temp per field
   - `:496` unpack_from — same
   - `:545` iter_unpack inner — parse_value temp per field
   - `:551` iter_unpack outer — `array_push(outer, inner_val)` clone
     never released (the 96 B direct + 48 B indirect journalled leak)
   - `:91` (pack) deliberately untouched: ints only, bitwise clone.
2. Pattern per site: `Value pv = parse_value(...); array_push(arr, pv);
   value_free(&pv);` — `value_free` (interpreter.h:83) is public;
   `value_release` is static to interpreter.c, unusable from stdlib.

## Test results (run 29)

Scratch validation: probes p1/p2/p3 (iter_unpack, unpack ">c")
**146 B/7 → 0/0** under LSan, full test_struct 0 leaks rc=0, REPL mode
clean, A/B `RADS_NO_BC=1` vs BC **byte-identical** (ab.rads:
pack/unpack ">iHbc", iter_unpack "<h", ">bB", ">c" from string).
Worktree: clean rebuild + gauntlet **26/26** green. Pushed to fork:
`0a861cb..f051c04`.

## Known hazards (noted, not fixed — pre-existing)

- **REPL/script `import` sub-AST never freed** (probed this run, run 29
  build): `import "lib.rads"` leaks 152 B / 3 allocs — `ast_create_program`
  at interpreter.c:1790 (exec_statement import case) parses the module
  and never `ast_free`s it. **Next up.**
- `turbo i32 c = GREEN;` echoes null — enum member resolution
  semantics, identical in script mode; not a REPL regression.
- Native-return values leak engine-wide (run 25 baseline class; includes
  `str.split` tokens) — documented, tolerated.

## Next up (single task for next run)

1. **import sub-AST leak**: exec_statement's import case
   (interpreter.c:~1790) — `ast_free` the imported module's Program AST
   after execution. Same method: LSan differential before/after, both
   modes, ownership mapped before freeing (top-level func decls may be
   registered by reference — verify ast_free safety first).

Rules unchanged: compile-bail fallback, suite no-new-failures minimum,
ASAN clean (pre-existing leaks tolerated), A/B with RADS_NO_BC=1, diff BC
vs tree-walker output byte-for-byte, `git push fork` only.

## Run 28 (previous run — summary retained below)

Struct-literal orphan leak — turned out four stacked corpses in one
class: (1) per-literal StructDef orphan (interpreter.c:1609) — registry
binding now owns the ONE canonical def, literals/clones borrow;
(2) parser identifier orphan in parse_call — free expr after creating
the literal (name aliases expr's name); (3) 4 stdlib creators
(datetime.now/csv/cli.run/env.vars) left StructInstance->definition
uninitialized — live SEGV on 0xbebebebebebebebe, all NULL-init'd,
unguarded readers label NULL "native"; (4) script-mode struct+enum
registry leak — interpret() tail now env_free → env_free_structs →
env_free_enums. LSan script 172 B/8 → 0/0; gauntlet 26/26.
Commits 30d3a9c + 0a861cb on fork.

## Run 27 (previous run — summary retained below)

REPL could not define functions at all (exec_statement had no
AST_FUNCTION_DECL case) nor parse struct/enum lines (no declaration
route) — the journalled UAF was unreachable because registration itself
was dead. Three fixes: public parse_repl_line() over parse_declaration,
interpret_repl_statement routes decls through exec_function, per-line AST
arena freed in bulk after interpreter_cleanup_environment() (negative
control reproduced the exact bc_try_execute UAF). New REPL regression
gate in run_tests.sh (stdin probe, asserts 5/arena/610). Gauntlet 26/26 +
gate. Commits 9329354, e71ed1a, 6b21627 on fork.

## Run 26 (previous run — summary retained below)

BcFunc exit-cache free through the ast_free chokepoint (negative
control: 1377 B / 19 allocs → 0); sibling sweep found regex.replace
heap-overflow WRITE (estimate-sized buffer) rewritten two-pass,
zero-width EOS parity, and regex.search's uninitialized
StructInstance->definition SEGV (static borrowed label fix);
tests/test_regex.rads born and wired in → gauntlet 26/26. A/B
byte-identical. Commits 11d787b, d518ac8, 0247539 on fork.

## Run 25 (previous run — summary retained below)

**Run 25 = journal "Next up": the struct.unpack heap-overflow READ.** Dead,
with a reproduced corpse for proof.

1. **struct memory safety** (`stdlib_struct.c`, full rewrite of the data
   path): `pack`/`pack_into` returned binary through `make_string(strdup)`,
   truncating at the first NUL — `pack(">i",42)` produced a 1-byte string;
   `unpack`/`unpack_from`/`iter_unpack` then read past that allocation
   (heap-buffer-overflow READ at `stdlib_struct.c:66`). New contract,
   mirroring `list(struct.pack(...))` in Python 3: **binary = array of byte
   ints (0-255)**; unpack family accepts byte-int arrays or strings; every
   read bounds-checked against real data length; `pack_into` zero-fill
   grows, negative offsets rejected; `iter_unpack` floors partial trailing
   records. **Negative control**: pristine source under ASAN reproduces
   `heap-buffer-overflow stdlib_struct.c:66 in parse_value`; fixed source
   clean. Gauntlet struct test expanded to 40+ checks (exact byte counts/
   contents, 64-bit two's complement, char fmt, string input, growth,
   short-buffer error paths returning null).

2. **str.replace Python parity bug** (`stdlib_string.c`): replaced only the
   FIRST occurrence — `str.replace("a-b-c","-","+")` → `"a+b-c"`. Rewrote
   as two-pass splice replacing ALL occurrences.

3. **Test runner hardening** (`run_tests.sh`): it judged pass/fail by exit
   code alone, but RADS `assert()` prints to stderr and returns 0/1
   WITHOUT aborting — every failing assertion was invisible; tests with
   failing asserts still PASSED. Runner now also fails on
   `"Assertion failed"` in output. Honest gauntlet = 25/25.

4. **Stale-expectation refresh** (hidden by the blind runner, not
   regressions): `test_string.rads` — contains/starts_with/ends_with/
   is_empty return bool (`== 1/0` → `== true/false`); substring is
   end-exclusive (Python slice), self-contradictory `"ell"` expectation
   fixed. `test_str_array.rads` — `str()` uses Python-exact reprs since
   run 22 (`true→"True"`, `null→"None"`, nested strings quoted). Verified
   canonical via live binary + no test anywhere asserting old reprs.

## Run 25 test results (retained)

Clean rebuild, gauntlet **25/25** on the hardened runner. A/B byte-identical
BC vs `RADS_NO_BC=1` on test_struct, test_string, test_str_array. ASAN+
UBSAN+LSan: struct/string tests clean of any NEW leak class (native-return
values leak engine-wide, baseline `str.split` leaks identically — same
documented class as the BcFunc 56-byte exit leak; `array_push` clones with
refcount bump so nested struct arrays follow engine convention, no
double-free). Negative control: old source + ASAN = heap-buffer-overflow at
stdlib_struct.c:66. Pushed to fork: `17036f8..a6b0bd4` (9ed130b fix,
8908e20 str.replace, a6b0bd4 tests+runner).

## Run 24 (previous run — summary retained below)

**Run 24 = list & dict comprehensions.** `[x*2 for x in xs if c]` and `{k: v
for x in xs}` in both execution paths, Python semantics; RADS-native
`cruise` spelling works too. AST_COMPREHENSION_EXPR + flat clause list
(multi-for flattens into one loop nest, filters guard the chain); tree-
walker recursion mirroring cruise ownership rules; VM inline loop nest with
BC_ALEN/BC_APPEND/BC_DSTORE. Dict-literal grammar widened (computed keys),
two corpses fixed en route (dangling free in identifier-key path, off-by-one
peek2_next_types misuse). Wired run-23's orphaned test_dict.rads into the
gauntlet. Clean rebuild, gauntlet 25/25, A/B byte-identical, no new leak
classes (BcFunc cache leak pre-existing, documented). Pushed:
`aa026ab..6573c65`.

## Open blockers

## Run 23 (previous run — summary retained below)

*Run 23 = dictionaries end-to-end: VAL_DICT core type, dict methods
(stdlib_dict.c), {"k": v} printing, json.parse objects → dicts, a[i]=v
corpse fixed, sticky-failure test harness (sabotage-verified). Gauntlet
24/24, 6/6 rads-mask fixtures byte-identical to CPython, ASAN/UBSAN clean
on dict paths, 5k-key stress 0.13s. Commits bfc39f4 + aa026ab on fork.
Full detail preserved under "Run 23 detail" below.*

1. **Core**: `VAL_DICT` in `ValueType`; `{...}` parses to
   `AST_ARRAY_LITERAL` + `is_dict` flag (`ast_create_dict_literal`);
   tree-walker + VM (new `BC_MKDICT`/`BC_SETIDX` opcodes) both grow
   keyed read (`dict_index_read` — string/int/float keys, missing → null)
   and keyed write (`dict_store` — replace-or-append, last write wins).
   **Also fixed the ancient `a[i] = v` silent no-op corpse for arrays**
   (same AST_ASSIGN_EXPR index-target path).

2. **Methods**: new `src/stdlib/stdlib_dict.c` — `len/keys/values/items/
   get/contains/pop/update/setdefault/copy`; `d.keys()` dispatch prepends
   the receiver like the str.* lane (eval_call + BC_CALLM both).

3. **Printing**: `echo` shows `{"k": v}`; `str()` shows Python's
   `{'a': 1}`; `type(d)` = `"dict"`; `len(d)` = pair count.

4. **json**: `json.parse` objects return VAL_DICT (duplicate keys:
   last wins, RFC 8259); `json.get/keys/values/has_key/type_of` accept
   dicts. THREE latent bugs fixed en route: **json.get returned a
   BORROWED payload reference → heap corruption** (now clones);
   **jp_parse_object leaked every key and value** (array_push clones,
   parsed originals never freed); the array→object stringify heuristic
   stays array-only now that dicts are explicit.

5. **Test framework hardening**: `set_test_result` is sticky-failure (a
   later passing expect erased an earlier failure — false greens since
   inception), and any test failure now propagates to the process exit
   code (`stdlib_test_failed()` + main.c check; run_tests.sh judges rc).

**Test results:** clean rebuild RC=0; gauntlet **24/24** (new
tests/test_dict.rads: 12 self-checking cases — sabotage-verified the
harness catches an injected wrong value); 6/6 transpiled fixtures
byte-identical to CPython; ASAN clean on all dict paths (UBSAN too).
Dict stress: 5k keys built+read in 0.13s. Commit bfc39f4, pushed to fork.

**Known pre-existing issues (NOT from this run, verified on pristine
sources via git stash):** (a) heap-buffer-overflow READ in
`stdlib_struct.c:66` parse_value via struct.unpack (test_struct.rads
passes functionally; ASAN flags the read); (b) bc_compile caches leak at
exit (program-lifetime by design); (c) test.run() inside REPL-only flows
never exits nonzero (fixed for file runs).

**Next up (run 24): list comprehensions** — the second half of the
journal's dict+comprehension scope: `[x*2 for x in xs]`,
`[x for x in xs if x > 2]`, ideally dict comprehensions `{k: v for ...}`
(now meaningful with VAL_DICT). Transpiler rules for `for k in d:` +
`d.items()` unrolling in rads-mask. Fallback: GAPLIST high-value gaps
(Phase 1 #3).

## What this run changed

**Run 22 = journal "Next up": elif chains + f-strings through the
transpiler.** Elif chains were already green (rules existed); the probes
surfaced SEVEN corpses, all now fixed:

1. **`str()`/`print` now have PYTHON semantics, `echo` keeps RADS
   semantics** (`src/stdlib/stdlib_test.c`): new `py_str_value()`
   serializer — shortest round-trip float repr (`3.14`, `2.0`,
   `0.30000000000000004`, `1e-07`, `1e+16`, `1000000000000000.0`),
   `True`/`False`/`None`, quoted strings inside arrays. This is the
   key design call: `echo` keeps its `%f` format (zero blast radius on
   the gauntlet), the transpiler routes `print(x)` → `echo(str(x))`.

2. **Bare `len()`** — string length or array count, loud error otherwise.

3. **`str.startswith`/`str.endswith`** Python spellings registered
   (aliased to the existing `starts_with`/`ends_with` natives).

4. **`pyfmt.fixed(x, n)`** — Python `{x:.nf}` semantics for f-string
   specs (also handles int args, inf/nan).

5. **Scientific-notation float literals** (`src/core/lexer.c`):
   `1e16`, `2.5e-7`, `3E+8` lex as TOKEN_FLOAT (parser strtod already
   handles them; only consumption was missing).

6. **rads-mask f-string expansion** (`simple_transpiler.c`): `f"..."`
   → parenthesized RADS concatenation. `{expr}` → `str(expr)`, `{{`/`}}`
   escapes, `{e:.2f}` → `pyfmt.fixed(e,2)`, `{e:>10}`/`{e:<10}`/`{e:^7}`
   → `rjust`/`ljust`/`center`, `{e:5d}` → `rjust(5)`. String-literal
   aware (never expands inside other strings), last-colon spec split.

7. **rads-mask housecleaning**: `import`/`from` lines now DROPPED (the
   old `import "X.rads"` emitted a load of a nonexistent file — a
   landmine); `print()` → empty echo; True/False/None mapped with word
   boundaries + string-awareness (early, before all statement rules).

**Test results:** clean rebuild RC=0; gauntlet **23/23**; 6/6 transpiled
fixtures byte-identical to CPython; BOTH run-22 probes (elif/str-chains
34-line + f-string torture 11-print) byte-identical. Commit 682eacd,
pushed to fork.

**Next up (run 23):** push the probe frontier into **dicts + list
comprehensions** through the transpiler (`d = {}`, `d[k] = v`,
`d.get()`, `[x*2 for x in xs]`, `for k in d:`). Dicts are the biggest
remaining structural gap — RADS has no map type yet (probe first: maybe
structs/nested arrays can carry it, else plan a proper `hash.rads`/
VAL_HASH core type). Fallback: `docs/stdlib/GAPLIST.md` (Phase 1 #3).

**Open blockers:** none.

## Run 21 (previous)

**Run 21 = journal "Next up": run remaining transpiled fixtures + fix red.**
The fixtures themselves were ALREADY green — all 6 `.rads` outputs
byte-identical to CPython (`pass=6 fail=0`, `tail -n +4` +
`grep -v "^Executing:"` normalization; assignments/if_elif_else print
nothing in CPython too — assert-only). The run's real work was the
corpse sweep the journal predicted: `"abc".upper()` and bare `max()/min()`
returned **null, silently** (rc=0, no diagnostic). Four surgical edits:

1. **String-receiver dispatch** (`src/core/interpreter.c`, BOTH eval_call
   and BC_CALLM): member calls on strings now try `str.%s` natives FIRST,
   `net.%s` handles second. `str.*` wins on collision by design — string
   receivers are strings. Was: `net.%s` only → silent-null fallthrough.

2. **`str.upper`/`str.lower` registered** in `src/stdlib/stdlib_string.c`
   (aliased to existing `stdlib_string_upper/lower` from string_advanced),
   plus Python names `str.strip` (→trim), `str.starts_with`,
   `str.ends_with`.

3. **Bare variadic `max`/`min` natives** (`src/stdlib/stdlib_math.c`):
   `max(3,7)`→7, `max([1,5,3])`→5, returns the winning element AS-IS so
   mixed int/float keeps the winner's type (`max(2.5,7)`→7 like Python;
   first draft widened to 7.000000 — caught by probe, fixed). Dotted
   `math.max`/`math.min` stay strict 2-arg. Empty seq → loud error.

4. Probe bank `/tmp/p1-p3.rads` — 10 corpse probes all live now (upper,
   lower, strip, max×4 forms, min×2, starts/ends_with).

**Test results:** clean rebuild RC=0; 6/6 fixtures CPython-identical
post-patch (no regressions); gauntlet **23/23**. Commit a8671bc, pushed
to fork.

**Next up (run 22):** `elif` chains + f-strings through the transpiler —
the two named corpses NOT yet verified (transpiler passes method calls
verbatim, but `elif`/f-string translation rules are unprobed). Probe
`tools/rads-mask` with a Python file using elif chains + f-strings +
string-method chains, transpile, run, diff vs CPython. Fallback:
`docs/stdlib/GAPLIST.md` (Phase 1 #3) — create the Python-module →
RADS-status map.

## Run 20 (previous)

**Run 20 = journal "Next up": rads-mask triage, but the str(array) hole
came first (user-flagged) and the triage fell out of it naturally.**
Theme: transpiled Python now runs green end-to-end. Four stacked fixes:

1. **str(array) returned "null"** — `stdlib_test_str` only handled
   scalars. Extracted `value_to_string()` from `value_print` (single
   source of truth, malloc'd buffer, recursive) in
   `src/core/interpreter.c`; echo AND str() both route through it.
   `str(x)` is now byte-identical to `echo x` for every type: arrays
   `[1, 2, 3]`, nested arrays, structs, functions, null. Old static
   helper in stdlib_test.c (512-byte buf, %g floats) removed —
   assertion-failure messages render arrays correctly now too.
   NOTE: str(float) uses %f (3.500000) matching echo, not old %g.

2. **arr.append(x) was "Undefined method"** — dispatch only knew
   `push`. Added the Python alias in BOTH sites: tree-walker eval_call
   AND BC_CALLM bytecode handler (`src/core/interpreter.c`).

3. **`cruise w in words {` + empty body** parsed `words { }` as an
   empty struct literal (run-19 gate accepts `}`), then died on the
   loop's close brace. New `parser->no_struct_literal` flag
   (`src/core/parser.c/.h`) suppresses struct-literal parsing for the
   cruise iterable expression only. Normal struct literals untouched.

4. **rads-mask** (`tools/rads-mask/src/simple_transpiler.c`):
   - Rule 10b: multi-arg `print(a, b)` → `echo(str(a) + " " + str(b))`,
     depth-aware comma split (commas in strings/calls/brackets safe).
   - Rule 7 rewritten: `for x in coll:` → `cruise x in coll {` (was
     invalid `cruise (coll) { turbo x;` — the `turbo x;` also SHADOWED
     the loop var with null). `range(n)`→`0..n`, `range(a,b)`→`a..b`
     native; old `array.range` mapping called a function that never
     existed anywhere in src/.
   - Bounded sprintf/snprintf in my touched paths (no new -Wrestrict
     or format-overflow warnings from run-20 code).

**Test results:** clean rebuild RC=0; gauntlet **23/23** (new
`tests/test_str_array.rads`, 6 sections, runner 22→23); rads-mask
transpiled suite green (brace balance + no-leak checks); transpiled
`test_comprehensive.py` runs end-to-end with Python-identical output
(Fib table + `Primes: [2, 3, 5, 7, 11]`) — first time EVER green.
Commit ea75753 (single atomic fix-commit), pushed to fork.

**Next up (run 21):** continue rads-mask triage — run the transpiled
fixtures for assignments/if_elif_else/loops/functions through
`./bin/rads` (not just the transpile-text checks) and fix what's red;
candidate corpses: `elif` chains, `while` translation, string-method
calls (`"x".upper()`), f-strings. Fallback: `docs/stdlib/GAPLIST.md`
(Phase 1 #3) — `max`/`min` bare natives are missing (probe hit
"Undefined function 'max'").

## Session hygiene law (all runs)

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

- run 27: REPL ownership class slain — the journal's dangling func_node was
  buried under TWO layers: functions never registered at all (no
  AST_FUNCTION_DECL case in exec_statement) and struct/enum lines never
  parsed (REPL used parse_statement). Fixed with parse_repl_line() over
  parse_declaration, exec_function registration in
  interpret_repl_statement, and a per-line AST arena freed after env
  teardown. ASAN negative control: registration-without-arena reproduces
  the exact bc_try_execute UAF; full fix clean. Gauntlet 26/26 + REPL
  regression gate. 9329354, e71ed1a on fork. Next: struct-literal orphan
  leak (86 B/4 allocs, both modes).
- run 24: list & dict comprehensions shipped — `[x*2 for x in xs if c]` /
  `{k: v for x in xs}` in BOTH paths (AST_COMPREHENSION_EXPR + flat
  clause list; tree-walker recursion mirroring cruise ownership rules;
  BC inline loop nest, hidden depth-suffixed slots, new BC_ALEN/
  BC_APPEND/BC_DSTORE). `cruise` spelling works, multi-for = one nest,
  dict upsert last-wins, computed dict keys, zero lexer changes. Two
  corpses en route: dangling free() in the dict-literal identifier-key
  path (segfault + `{"": rads}` garbage keys) and peek2_next_types
  off-by-one use. Wired run-23's orphaned test_dict.rads into the
  runner. Gauntlet **25/25**, A/B byte-identical, ASAN/UBSAN clean (no
  new leak classes; BcFunc-cache-at-exit leak documented as pre-existing).
  6573c65 on fork. Next: struct.unpack heap-overflow READ fix.
- run 21: silent-null corpses slain — `"abc".upper()` and bare `max/min`
  returned null with rc=0. String-receiver dispatch (eval_call+BC_CALLM)
  now tries `str.%s` before `net.%s`; str.upper/lower/strip/starts_with/
  ends_with registered; bare variadic max/min (winner-as-is typing,
  max(2.5,7)→7). 6/6 fixtures CPython-identical, gauntlet 23/23. a8671bc.
- run 20: str(array)="null" + three transpiler corpses slain. (1) shared
  value_to_string serializer — str(x) ≡ echo x for all types; (2)
  arr.append alias in both dispatch sites; (3) parser
  no_struct_literal flag for cruise iterables; (4) rads-mask Rule 10b
  multi-arg print → str-concat, Rule 7 for-in → `cruise x in coll` +
  native range mapping (old array.range never existed). Transpiled
  test_comprehensive.py GREEN end-to-end for the first time
  (`Primes: [2, 3, 5, 7, 11]`). Gauntlet 23/23. Commit ea75753, pushed.
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

**Next up (run 19):** journal is clean of red files — gauntlet 21/21. Shift
to Phase 1 workstream #2 per AGENTS.md priority: **rads-mask test suite**.
`tools/rads-mask` (typed RADS→C transpiler): run its test suite, triage
failures, fix the highest-value gaps (type annotations, inference, error
messages). One unit: make its suite measurably greener than found.
Alternative if blocked: stdlib GAPLIST.md (Phase 1 #3) — create
`docs/stdlib/GAPLIST.md` mapping Python modules → RADS status.

## Run 19 — 2026-10-14: cruise-over-arrays + struct-literal disambiguation

**What changed (run 18's silent flag resolved):** the run-18 verifier flagged
a failed patch to `RADS-wt-bt/tests/test_v0.0.12.rads` — a path in the OTHER
worktree (`-wt-bt`, note bt≠bc). Wrong worktree, no action needed there. The
REAL bug it led me to: `cruise i in <array>` executed **zero iterations,
silently** — two stacked defects, both pre-existing on 362defc (stash-verified):
1. `fix(interpreter)` 006483c — `AST_CRUISE_STMT` only handled `OP_RANGE`
   iterables; array iterables fell through with no diagnostic. Added the
   array path (iterable evaluated once, AST_INDEX_EXPR element-binding
   conventions: strings copied, nested arrays share+refcount, scalars plain;
   break/continue/return/throw propagated; container pinned across body).
2. `fix(parser)` 1ac0834 — `Identifier {` was ALWAYS eaten as struct literal,
   so `cruise i in items { n = n + 1; }` died "Expected ':' after field
   name" (and old binary segfaulted). Two-token peek (`identifier :` or `}`)
   now gates the literal parse; lexer snapshot/restore keeps it clean.

**Test results:** clean rebuild RC=0; new `tests/test_cruise_array.rads`
(7 sections) green; gauntlet **22/22** (runner 21→22). Malformed-input
probes (bad1–bad5) all exit 1 with line/col errors, no segfaults.

## Session hygiene law (all runs)
One task per run, then journal + commit + push + END. Context bloat kills
the loop (run 16 hit 36k tokens, compaction exhausted). Concretely:
- Cap read_file at 60 lines; prefer `sed -n 'A,Bp'` with tight ranges.
- No full-suite logs into context — `bash run_tests.sh 2>&1 | tail -5`.
- One build per run max; verify with 2-3 probe files via `./bin/rads`.
- If a context-pressure warning appears: STOP work, write journal, end run.

## Run log (continued)
- run 19: `cruise i in <array>` silently ran ZERO iterations — two stacked
  pre-existing bugs slain. (1) interpreter 006483c: AST_CRUISE_STMT handled
  OP_RANGE only; array path added with AST_INDEX_EXPR binding conventions.
  (2) parser 1ac0834: `Identifier {` always eaten as struct literal, killing
  `cruise i in items { n = ... }` with "Expected ':' after field name" +
  segfault; now gated on two-token `identifier :` / `}` peek. New
  test_cruise_array.rads (7 sections), gauntlet **22/22**. 3 commits pushed.
- run 18: the last 3 red files flipped — gauntlet **21/21** (runner 18→21).
  Six surgical edits: (1) lexer `fn`→TOKEN_BLAST alias (pythonic lambdas);
  (2) parser TOKEN_STRUCT as expression-start (struct.calcsize et al parse);
  (3) anonymous `{k:v}` map literals desugar to flat `[k,v,...]` arrays —
  same shape json.parse emits, so all json.* helpers accept them; (4)
  receiver-prepend dispatch in eval_call for chained calls on call results
  (test.expect(x).toBe(8)); (5) registered str.length + string.* aliases +
  bare toBe/toEqual/toBeTruthy/… matcher natives; (6) **string escape
  decoding at parse time** (`\n` `\t` `\r` `\"` `\\` `\e`) — the hidden
  root cause: raw `\"` poisoned every embedded-JSON literal, silently
  yielding empty objects (json.parse returned `[]` for 10/50 v0.0.12
  tests). test_v0.0.12's pretty-printer assert updated to decoded
  semantics. 50/50 in-suite, test_http green end-to-end over live
  httpbin.org, test_struct green. Clean rebuild + 21/21. Commit 27c1f10,
  pushed to fork. NOTE: pre-existing -Wimplicit-fallthrough warning in
  lexer.c `for`→cruise case (run 17 code) — left alone, no drive-bys.
- run 17: pythonic surface round 3 — optional semicolons
  (consume_statement_end: newline/}/EOF as implicit terminator), paren-free
  `echo expr` / `for i in iter { ... }`, `print`/`println`→echo and
  `for`→cruise lexer aliases, bare `import bisect;`, top-level statement
  execution before main() (module semantics), postfix `{` no longer eats
  loop bodies, **undefined calls now fail loudly** (test_time was
  vacuously green — silent null masked every failure), bare natives
  assert/str/type/abs, stale stdlib/test.rads import removed from
  test_time. 5 of 8 red files flipped: math/string/bisect/itertools/time
  all green with real output. Gauntlet 18/18 both BC and NO_BC (runner
  grew 13→18). Commit 7fade7b, pushed to fork. Remaining red: struct,
  http, v0.0.12 — blocked on fn() lambdas + dotted struct.* calls (run 18).
- run 16: `#` line comments + `let` alias shipped (commit e97f80c, pushed
  to fork). Gauntlet 13/13 (restored missing test_stream.rads — runner
  had a phantom 12th entry; new test_pythonic_syntax.rads). test_itertools
  flipped red→green as a side effect. 8 red files remain, all blocked on
  optional semicolons / paren-free echo / dotted stdlib calls.
- run 15: nightshift unified into phase0, mixed-float arithmetic fixed,
  45 commits pushed to fork Dagoth-Ur-69/RADS, upstream PR #19 open.
