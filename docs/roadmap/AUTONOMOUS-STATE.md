# RADS Autonomous State — handoff journal

Last updated: 2026-10-11 evening (manual owner-directed session, between cron runs)
Branch: `phase0/build-recovery`
HEAD: `69a60f7` bench: record post-hash-registry results

## What this run changed

**Owner asked for a status report + more building. Took the journal's own
"Next up" (fib optimization) and landed the single biggest win in RADS
history in one commit.**

1. `a5c2160` — **perf(core): hash-table native registry.**
   `find_native()` was a linear walk over ~680 registered natives on EVERY
   call expression — including the miss that precedes every user-function
   call. A 2048-slot open-addressing FNV-1a table (~33% load) replaced the
   linked list. Honest numbers (best-of-3, results/20261011T181055Z.json):
   - fib(28): 3081.5 → **275.3 ms (11.2x)**. Still 4.9x CPython.
   - array_churn: 65.2 → **12.4 ms (5.3x) — now BEATS CPython (13.4)**.
   - string_build: 46.2 → 49.9 (noise; concat path untouched).
   - file_io: 20.8 → 17.7.
   **RADS now beats CPython on 2/4 workloads and Node on all 4.**
2. `69a60f7` — RESULTS.md updated with post-registry table + new takeaways.

## Test results

- `make`: green. `run_tests.sh`: **7/8** — the 1 fail is the known
  pre-existing `v0_0_7_comprehensive.rads` (unsupported syntax), unchanged
  by this work.

## Next up (single task for next run)

**Perf round 2: call-frame setup.** The remaining fib gap (275ms vs CPython
57ms) is per-call env overhead in `interpreter_execute_callback`: every
parameter bind does malloc+strdup+clone (`env_decl` → `env_list_set`), and
every frame's locals list is freed on return. Options in payoff order:
1. Small fixed-size inline param array in EnvFrame (e.g. 8 slots, name
   pointer + Value) before falling back to the heap list — kills the
   malloc/strdup/free per param for the common case.
2. `env_list_get` returns a clone — `eval_call`'s `env_get(name)` for the
   callee also clones the func value every call; cache the func binding or
   return borrowed refs on the hot path.
Measure after each change (`bash benchmarks/run.sh`); revert if the number
doesn't move. Update RESULTS.md + this journal.

## Open blockers

- `git push` still fails: no credentials. **15 commits** now await push
  locally on `phase0/build-recovery`. Needs PAT or `gh auth login` by the
  owner (GitHub account colyzeo@gmail.com).
- `v0_0_7_comprehensive.rads` + several stray tests/*.rads use unsupported
  syntax (`#` comments, `fn`, `let`) — rewrite or implement; candidate work.
- Pre-existing gcc warnings (unused vars/fns in stdlib_net, functools,
  parser.c:648 `update_block`) — cosmetic.
- `.gitignore` line 57 `core` matches `src/core` on directory add — use
  `git add -u` or explicit file paths when staging core changes.
