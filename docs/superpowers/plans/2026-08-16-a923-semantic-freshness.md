# A9.2.3 — Semantic Index Freshness, Coverage & Automatic Rebuild — Implementation Plan

> **For agentic workers:** Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the Atlas daemon own semantic-index freshness for repositories an
operator has explicitly enabled, and make a generation describe what it actually
covered, so A9.2.2's negative semantics rest on trustworthy inputs instead of on
a manual rebuild somebody remembered to run.

**Architecture:** Freshness stays *derived*, never stored — A6's rule. The three
genuine gaps are (1) no durable per-repository semantic build description, so
the daemon cannot rebuild unaided; (2) no scheduler, so nothing notices; (3) no
scope-discovery denominator, so `198/198` is an overclaim. A9.2.3 adds one
migration (`sem_repo_config` plus coverage columns on `sem_generations`), one
scheduler on the watcher's existing timer that queues the *existing* writer
sem-index job, and a coverage manifest computed at publication and fed into
A9.2.2's existing coverage dimensions. No second epistemic engine, no second
rebuild implementation, no new vocabulary where one exists.

**Tech Stack:** C17, SQLite, libclang, CMake. First-party test harness.

**Spec:** the A9.2.3 season brief in the session prompt (§§0–64).

---

## Global Constraints

- `CLAUDE.md` hard rules apply verbatim: no shell, no new dependencies, warnings
  are errors, never modify a registered target repository, exactly one writer.
- Freshness is **recomputed on every read, never cached**. A stored dirty bit
  would be a second answer to a question that already has one.
- **Coverage is a second axis, not a freshness value.** Source-current +
  coverage-incomplete is a real, reportable state. Folding it into freshness
  would derive one axis from another.
- **UNKNOWN is zero** on every new vocabulary. Auto-rebuild disabled is zero.
- **Migration 18 is additive and relabels nothing.** Pre-A9.2.3 generations
  carry `scope_discovery = 'UNKNOWN'`; they are never declared COMPLETE.
- **The migration must not auto-enable any existing repository.** Auto-rebuild
  runs a compiler; that is an operator decision, and A8-CI's rule that no model
  can cause a compiler to run must survive.
- Manual and automatic rebuild use **one pipeline**: `atlas_sem_index_on`.
- Report measurements as observations ("7 s observed"), never as bounds.
- A new command touches **five** places including `COMMANDS[]`, and must be run
  once from the built binary.

---

## Pre-change architecture map (§1) — established by inspection

| Question | Answer as found on `fac4ff9` |
|---|---|
| structural index | A3 lexical, `code_*` tables, migration 5, `code_index_state` |
| semantic index | A8-CI libclang, `sem_*` tables, migration 11 |
| generation rows | `sem_generations` (RUNNING/COMPLETE/FAILED), `sem_current` is the pointer |
| creation | `atlas_sem_index_run` ← `atlas_sem_index_on` ← CLI `code index` or RPC `code.index` (operator-uid) |
| selection as current | one transaction: mark COMPLETE + repoint `sem_current` |
| source binding | `o.commit_id = repo.scanned_head`, read at start; `repo_identity_hash` written as `""` |
| completeness | `tu_total/complete/partial/failed/unsupported`, never summed |
| stale reason | `ATLAS_SEM_STALE_*` fixed strings, recomputed by `atlas_sem_freshness_of` |
| repository move | not detected by the semantic layer at all |
| compdb discovery | never discovered; `--compdb` explicit, **not persisted** |
| rebuild trigger | CLI or operator-uid RPC only. **Nothing automatic.** |
| incremental | yes, per-unit `input_digest` over the transitive include closure |
| old generations queryable | rows survive; only `sem_current` is served |
| partial builds visible | no — publication is one statement |
| where it runs | writer thread (daemon) or the CLI's own process |
| restart | forgets nothing durable; a crashed build leaves RUNNING unpublished |

**Defects found during the audit, before any design:**

1. **`live_compdb_digest` is `NULL` at every call site** (`server_sem.c:63`,
   `service_sem.c:85`, `context.c:496,812`). The compdb-staleness branch in
   `atlas_sem_freshness_of` is unreachable. §18/§44 do not work today.
2. **`sem.status` RPC omits `compiler_id` and `started_at`.** On a system
   deployment the socket is the only path, so both fields are lost — the exact
   A9.2.1-closure defect ("missing the read-back is how the two paths start
   disagreeing"), one layer over.
3. **`repo_identity_hash` is written as `""`** for every generation, so a
   generation cannot say which repository lineage it described.
4. **`ATLAS_COVDIM_TRACKED_SOURCE` is asserted COMPLETE from TU parse
   success.** That is §14's overclaim verbatim: it says "every tracked source in
   scope was read" on the strength of the compilation database having named
   whatever it named. Nothing establishes the denominator.
5. **DNA is STALE and stays STALE.** Live: generation 4 at commit `1b12514`,
   HEAD `55abfaa`, no path to recovery but a human running a command.

---

## File structure

**Create:**
- `src/sem/schedule.c` — the daemon's semantic freshness scheduler: derived
  dirty state, the retry governor, the one call that queues a rebuild.
- `include/atlas/sem_schedule.h` — its interface, and the derived-state
  vocabulary (`atlas_sem_activity`).
- `tests/test_sem_freshness.c` — freshness derivation, coverage manifest,
  scope discovery, retry governor. Unit.
- `tests/test_sem_auto.c` — live-daemon automatic rebuild, coalescing, failure
  and recovery, restart, repository move. Daemon label.
- `tests/test_sem_absence.c` — A9.2.2 × A9.2.3 fixtures A–F.

**Modify:**
- `include/atlas/sem.h` — coverage manifest on `atlas_sem_generation`,
  `atlas_sem_config`, the scope-discovery vocabulary.
- `src/sem/sem.c` — vocabulary names/parsers.
- `src/sem/index.c` — record `repo_identity_hash`; compute the coverage manifest
  at publication; accept declared test roots.
- `src/db/migrate.c` — migration 18.
- `src/db/db_sem.c` — `sem_repo_config` CRUD, coverage columns, scope counts.
- `src/db/db_verify.c` — `sem_generation_state` upgraded to the real dimensions.
- `src/verify/detverify.c` — `sem_coverage` split so TRACKED_SOURCE comes from
  scope discovery and TESTS from declared roots.
- `src/daemon/watch.c` — call the scheduler from the existing timer.
- `src/core/service_sem.c` — `sem config` behaviour, live compdb digest, the
  richer status report.
- `src/core/service_remote.c` — read back every new field.
- `src/ipc/server_sem.c` — send every new field; `sem.config_get` read.
- `src/ipc/server.c` (operator table) — `sem.config_set`.
- `src/cli/cli.c`, `src/cli/render_human.c`, `src/cli/render_json.c` — the verbs.
- `src/mcp/mcp_tools.c` — `atlas_sem_status` extension (read only).
- `src/gw/gateway.c`, `src/gw/ui/mission-control.html` — read-only surfaces.
- `src/core/service_maintenance.c` — `RETENTION[]` row for `sem_repo_config`.
- `docs/code-intelligence.md`, `docs/verification.md`, `docs/operations.md`,
  `CLAUDE.md`.

---

## Phase 1 — Pin what already works, and the gap (§§2, 8, 9, 31)

### Task 1: Regression tests for the existing guarantees and the reproduced gap

**Files:**
- Create: `tests/test_sem_freshness.c`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `atlas_sem_freshness_of`, `atlas_sem_index_on`, fixture helpers.
- Produces: nothing other tasks depend on.

- [ ] **Step 1: Write failing tests** covering, in one file:
  `test_a_generation_binds_the_head_it_started_from`,
  `test_a_failed_build_leaves_the_previous_generation_current`,
  `test_a_compdb_change_alone_is_not_noticed_today` (the §18 gap, asserted as
  the *current* behaviour and inverted in Phase 3),
  `test_nothing_rebuilds_a_stale_generation_without_being_asked` (the §2 gap).
- [ ] **Step 2:** `cd build && ctest -R test_sem_freshness --output-on-failure` → FAIL (no such test).
- [ ] **Step 3:** wire into `ATLAS_TESTS` and the `unit` label; implement.
- [ ] **Step 4:** run → PASS.
- [ ] **Step 5:** commit.

---

## Phase 2 — Freshness correctness, no schema (§§18, 19, 23)

### Task 2: Wire the live compilation-database digest, and record repository identity

**Files:**
- Modify: `src/sem/index.c` (`atlas_sem_index_run` records `repo_identity_hash`),
  `src/core/service_sem.c` (compute the live digest; pass identity),
  `src/ipc/server_sem.c`, `src/sem/context.c`.
- Test: `tests/test_sem_freshness.c`

The live digest cannot come from the file index: a `compile_commands.json` is
routinely gitignored, so the index may not hold it. It is hashed from the
operator-declared paths at the moment freshness is asked, through
`atlas_path_open_nofollow` inside the root — a bounded read of files Atlas was
told to read, never a search.

- [ ] **Step 1:** failing test — build a generation, rewrite `compile_commands.json`
      with a different `-D`, assert freshness becomes `STALE` with reason
      `ATLAS_SEM_STALE_COMPDB`.
- [ ] **Step 2:** run → FAIL (reads CURRENT).
- [ ] **Step 3:** add `atlas_sem_live_compdb_digest(int root_fd, const char *const *paths,
      size_t n, char out[65], atlas_err *err)` in `src/sem/index.c`; call it from
      every `atlas_sem_freshness_of` call site that has a config.
- [ ] **Step 4:** run → PASS.
- [ ] **Step 5:** commit.

### Task 3: Close the RPC field gap

**Files:** Modify `src/ipc/server_sem.c`, `src/core/service_remote.c`.
**Test:** `tests/test_sem_auto.c` (daemon) asserts the socket path and the local
path report identical generation fields.

- [ ] **Step 1:** failing test comparing `atlas code sem-status --json` against a
      daemon and against a direct context, field by field.
- [ ] **Step 2:** run → FAIL on `compiler_id`, `started_at`.
- [ ] **Step 3:** send and read back both.
- [ ] **Step 4:** run → PASS. **Step 5:** commit.

---

## Phase 3 — Migration 18: the durable build description and the manifest (§§4, 13, 35, 36)

### Task 4: Migration 18

**Files:** Modify `src/db/migrate.c`, `include/atlas/sem.h`, `src/db/db_sem.c`,
`src/core/service_maintenance.c`. Test: `tests/test_db.c`, `tests/test_sem_freshness.c`.

Additive only. Two things:

```sql
/* The durable semantic build description. One row per repository, written by an
 * operator and by nobody else. Its absence is the default and means this daemon
 * never runs a compiler for this repository — which is what keeps A8-CI's rule
 * ("no model can cause a compiler to run") true after A9.2.3 makes repository
 * changes a rebuild trigger. */
CREATE TABLE sem_repo_config (
  repo_id INTEGER PRIMARY KEY,
  repo_identity_hash TEXT NOT NULL DEFAULT '',
  /* DISABLED is zero. A zeroed or malformed row schedules nothing. */
  auto_rebuild INTEGER NOT NULL DEFAULT 0,
  /* Newline-separated repository-relative paths, validated inside the root by
     the indexer exactly as --compdb is. Never discovered. */
  compdbs TEXT NOT NULL DEFAULT '',
  /* Newline-separated repository-relative prefixes the operator declares to be
     test sources. Empty means Atlas does not know which sources are tests, and
     the tests coverage dimension stays UNKNOWN — which is the honest answer and
     the one that stops "no production caller" being answerable. */
  test_roots TEXT NOT NULL DEFAULT '',
  configured_at TEXT NOT NULL DEFAULT '',
  /* The retry governor. A deterministic failure must not spin: a retry is
     allowed only once the source identity has moved past the one that failed. */
  fail_count INTEGER NOT NULL DEFAULT 0,
  fail_identity TEXT NOT NULL DEFAULT '',
  fail_reason TEXT NOT NULL DEFAULT '',
  fail_at TEXT NOT NULL DEFAULT ''
);
```

```sql
/* The coverage manifest, on the generation, sealed at publication.
   UNKNOWN is the default, so every pre-A9.2.3 generation reads UNKNOWN rather
   than being retro-declared complete — migration 17's rule. */
ALTER TABLE sem_generations ADD COLUMN scope_discovery TEXT NOT NULL DEFAULT 'UNKNOWN';
ALTER TABLE sem_generations ADD COLUMN scope_candidates INTEGER NOT NULL DEFAULT 0;
ALTER TABLE sem_generations ADD COLUMN scope_covered INTEGER NOT NULL DEFAULT 0;
ALTER TABLE sem_generations ADD COLUMN scope_uncovered INTEGER NOT NULL DEFAULT 0;
ALTER TABLE sem_generations ADD COLUMN tu_test INTEGER NOT NULL DEFAULT 0;
ALTER TABLE sem_generations ADD COLUMN tu_production INTEGER NOT NULL DEFAULT 0;
ALTER TABLE sem_generations ADD COLUMN test_scope_known INTEGER NOT NULL DEFAULT 0;
```

- [ ] **Step 1:** test asserting schema 18, `sem_repo_config` present and empty,
      every existing generation reading `scope_discovery = 'UNKNOWN'`.
- [ ] **Step 2:** run → FAIL. **Step 3:** implement, including the `RETENTION[]`
      row (CANONICAL, not prunable: an operator's build description is not
      rebuildable from the repository). **Step 4:** run → PASS. **Step 5:** commit.

### Task 5: The scope-discovery denominator (§§14, 15, 17)

**Files:** Modify `src/sem/index.c`, `src/db/db_sem.c`. Test: `tests/test_sem_freshness.c`.

At publication, in the publishing transaction:

- `scope_candidates` = files the **file index** holds for this repository whose
  name ends `.c` (translation-unit-eligible). This is a denominator Atlas can
  state, because A0/A1 enumerated the tree, not the compilation database.
- `scope_covered` = of those, how many appear as a `sem_units.source_text`.
- `scope_uncovered` = the difference, and it is the number that makes
  `scope_discovery` honest.
- `scope_discovery = 'DECLARED'` when the file index was current at publication
  (so the enumeration is one Atlas can vouch for), `'UNKNOWN'` otherwise.

The consequence, and the reason this exists: `198/198` translation units with
`scope_uncovered = 40` is a generation that parsed everything it was told about
and read four fifths of the repository. Only the second number can answer §14.

- [ ] **Step 1:** failing test — a fixture with three `.c` files, a compilation
      database naming two, assert `scope_candidates = 3`, `scope_covered = 2`,
      `scope_uncovered = 1`, `scope_discovery = 'DECLARED'`.
- [ ] **Step 2:** run → FAIL. **Step 3:** implement. **Step 4:** PASS. **Step 5:** commit.

---

## Phase 4 — A9.2.2 integration (§§20, 21, 48, 49)

### Task 6: Real coverage dimensions

**Files:** Modify `src/db/db_verify.c` (`sem_generation_state`),
`src/verify/detverify.c` (`sem_coverage`). Test: `tests/test_sem_absence.c`.

Three changes, each closing an overclaim:

1. `complete` currently means "no failed/partial/unsupported unit". It must also
   require `scope_uncovered == 0` and `scope_discovery = 'DECLARED'`.
2. `current` currently compares the generation's commit against `scanned_head`.
   It must also require the analyzer id and version to match, the compiler
   version to match, the live compdb digest to match, and the file index to be
   current — i.e. it must ask `atlas_sem_freshness_of`, which is the one
   implementation of that rule.
3. `sem_coverage` must stop assigning one value to four dimensions.
   `SEMANTIC_GENERATION` and `DIRECT_CALLS` follow the generation. `TRACKED_SOURCE`
   follows **scope discovery**. `GENERATED_SOURCE` follows scope discovery too — a
   generated `.c` outside the file index is exactly what `scope_uncovered` counts,
   and a build-directory source the index never saw is not covered by anything.
   `TESTS` follows `test_scope_known` and is UNKNOWN without declared roots.

- [ ] **Step 1:** failing fixtures — a generation with `scope_uncovered = 1` must
      make `atlas.no_proven_caller` return UNAVAILABLE and truth UNKNOWN with
      `ATLAS_TREASON_COVERAGE_PARTIAL`.
- [ ] **Step 2:** run → FAIL (returns ABSENT today). **Step 3:** implement.
- [ ] **Step 4:** PASS. **Step 5:** commit.

### Task 7: Fixtures A–F (§39)

**Files:** `tests/test_sem_absence.c`.

| Fixture | State | Expected |
|---|---|---|
| A | current, complete bounded scope, internal function, zero callers | `ABSENT` |
| B | stale, zero callers | `UNKNOWN`, `SEMANTIC_INDEX_STALE` |
| C | current, `scope_uncovered > 0` | `UNKNOWN`, `COVERAGE_PARTIAL` |
| D | current, one caller, unrelated dim incomplete | `PRESENT` |
| E | direct calls complete, address escapes | `UNKNOWN`, `INDIRECT_CALLS_UNRESOLVED` |
| F | claim is "no PROVEN direct caller", direct enumeration complete | `ABSENT` |

- [ ] **Step 1–5:** as above, one test function per fixture, each named for its
      row. Commit once all six pass.

---

## Phase 5 — The scheduler (§§5, 6, 7, 11, 12, 32, 33, 34)

### Task 8: `atlas_sem_activity` and the derived state

**Files:** Create `include/atlas/sem_schedule.h`, `src/sem/schedule.c`.
Modify `CMakeLists.txt`. Test: `tests/test_sem_freshness.c`.

Derived, never stored — the whole state is a pure function of the generation
rows, the config row, the repository row and whether a build is in flight:

```c
typedef enum atlas_sem_activity {
    ATLAS_SEM_ACT_UNKNOWN = 0,   /* nothing has been established */
    ATLAS_SEM_ACT_DISABLED,      /* no config, or auto_rebuild off */
    ATLAS_SEM_ACT_CURRENT,
    ATLAS_SEM_ACT_DIRTY,         /* STALE and eligible to rebuild now */
    ATLAS_SEM_ACT_BUILDING,
    ATLAS_SEM_ACT_INCOMPLETE,    /* source-current, coverage not complete */
    ATLAS_SEM_ACT_FAILED,        /* last attempt failed, identity unmoved */
    ATLAS_SEM_ACT_UNAVAILABLE    /* no usable generation */
} atlas_sem_activity;
```

`INCOMPLETE` is deliberately *not* a freshness value: it is the second axis, and
`atlas_sem_activity_of` is where the two are combined for reporting without
either being derived from the other.

The retry governor is one predicate: an automatic build is eligible when the
config enables it, no build is in flight, and either `fail_count == 0` or the
current source identity differs from `fail_identity`. Source identity is the
domain-separated digest of (`scanned_head`, live compdb digest, compiler
version, analyzer id, analyzer version) — so a deterministic failure never spins
and a genuine change always retries.

- [ ] **Step 1:** failing tests, one per state and one for the governor.
- [ ] **Step 2:** run → FAIL. **Step 3:** implement `atlas_sem_activity_of` and
      `atlas_sem_source_identity`. **Step 4:** PASS. **Step 5:** commit.

### Task 9: The daemon timer hook

**Files:** Modify `src/daemon/watch.c`, `src/daemon/writer.c` (record the failure
into `sem_repo_config` after an automatic attempt). Test: `tests/test_sem_auto.c`.

On the watcher's existing timer — which is already "the daemon's timer" and
already drives A8's recovery sweep — for each configured repository: compute
`atlas_sem_activity_of`; if `DIRTY`, queue the existing
`atlas_writer_submit_sem_index` job with `op_id = 0`.

Coalescing falls out of the derivation rather than being implemented (§§6, 34):
the scheduler always builds "now", never a queued intermediate state, and after
publication it recomputes freshness — still stale means still dirty means one
more build. Rapid saves during a build produce exactly one further build, not
one per save. **At most one automatic build is in flight across the daemon**,
because the writer thread is the one serialized writer; that is the existing
architecture and is measured rather than redesigned (§32).

- [ ] **Step 1:** failing daemon test — enable auto-rebuild, reach CURRENT,
      modify a source, assert CURRENT again within the fixture's wait bound
      without any manual command.
- [ ] **Step 2:** run → FAIL. **Step 3:** implement. **Step 4:** PASS. **Step 5:** commit.

### Task 10: Failure, restart, move, multi-repo (§§11, 29, 30, 31, 32, 42, 43)

**Files:** `tests/test_sem_auto.c`.

- [ ] Deterministic build failure → last-good preserved, `FAILED`, reason
      visible, negative queries UNKNOWN, no spin; fix the source → automatic
      recovery to CURRENT.
- [ ] Daemon restart in each state; an interrupted `RUNNING` generation must not
      become current, and is reaped.
- [ ] Repository root move: `repo_identity_hash` on the generation now makes the
      lineage checkable; assert the behaviour that the storage design actually
      produces and pin it.
- [ ] Two fixture repositories, one failing: the healthy one still converges.
- [ ] Commit.

---

## Phase 6 — Surfaces (§§22, 23, 24, 25, 26, 27, 28, 55)

### Task 11: CLI

**Files:** `src/cli/cli.c` (dispatch, help, **`COMMANDS[]`**), `src/cli/render_human.c`,
`src/cli/render_json.c`, `src/core/service_sem.c`, `include/atlas/service.h`.

- `atlas code sem-config NAME [--compdb P]... [--test-root P]... [--auto|--no-auto]`
  — operator-only mutation; `--json` refused as for other interactive commands.
- `atlas code sem-config NAME` with no flags — read.
- `atlas code sem-status NAME` — extended with activity, coverage manifest,
  last-good, failure reason, source identity.
- [ ] Run every new verb once from the built binary (§"the fifth is forgotten").
- [ ] Commit.

### Task 12: RPC, MCP, gateway, Mission Control

**Files:** `src/ipc/server_sem.c` (`sem.config_get`, read), the operator table
(`sem.config_set`, mutation), `src/core/service_remote.c`, `src/mcp/mcp_tools.c`,
`src/gw/gateway.c`, `src/gw/ui/mission-control.html`, `tests/test_plugin.c`,
`tests/test_orch_rpc.c`.

- `sem.config_set` goes in the **operator-uid** table beside `code.index`: it
  decides whether a compiler runs on repository change. Its name goes into the
  negative enumeration in `tests/test_orch_rpc.c`, which must confirm an ordinary
  peer gets `unknown method`.
- MCP gains **reads only** — the extended `atlas_sem_status`. No tool may enable
  auto-rebuild or trigger one; the daemon owns rebuild and a model needs no
  authority for it (§27, §55). Pinned count in `tests/test_plugin.c` updated.
- Gateway: read-only routes. Mission Control shows state **and** coverage as two
  fields, never one badge (§28).
- [ ] Commit.

---

## Phase 7 — Close (§§37, 50, 51, 52, 56, 57, 58, 59, 60, 61, 63, 64)

### Task 13: Matrix, install, acceptance, docs, push

- [ ] `make test`, `make asan`, `make ubsan`, `make tsan` — record exact counts.
- [ ] Backup/restore drill covering the new durable state (§37).
- [ ] Performance observations before and after (§50).
- [ ] `make install`; verify with `cmp`, not the report; restart `atlas.service`
      and `atlas-dispatcher.service`; verify no owned service runs a deleted inode.
- [ ] Installed end-to-end acceptance §58 and failure acceptance §59, through
      CLI, RPC, MCP and Mission Control.
- [ ] DNA read-only observation (§52) — no writes to `/opt/dna`.
- [ ] Non-interference proof for `/opt/dna` and `/opt/swapper` (§63).
- [ ] Documentation (§61), `CLAUDE.md` A9.2.3 section.
- [ ] Commit and push to `origin/main`, no force.
