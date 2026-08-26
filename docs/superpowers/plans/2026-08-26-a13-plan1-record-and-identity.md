# A13 Plan 1 — the record and the identity

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give every registered repository a `scanner_uid` — derived from the
owner of its root directory, refused for the three uids that must never scan,
and visible on every surface that describes a repository.

**Architecture:** One additive migration adds a column. `repo add` stats the
repository root and records its owner. A new `atlas repo scanner` command
assigns or re-derives it for repositories that already exist. Nothing behaves
differently yet — no scanner exists, nothing reads the column to decide
anything. This plan ships the identity that Plans 2–5 build on, and it is
independently testable because every step is observable through the CLI.

**Tech Stack:** C17, SQLite3, CMake ≥ 3.16, the first-party test harness in
`tests/atlas_test.h`. No new dependencies.

**Spec:** `docs/superpowers/specs/2026-08-26-per-user-scanner-design.md`

## Global Constraints

Copied from `CLAUDE.md`; every task's requirements implicitly include these.

- **Warnings are errors** (`ATLAS_WERROR=ON`) in first-party code. Fix the
  cause; never suppress.
- **No shell.** No `system()`, `popen()`, `/bin/sh -c`. Processes only through
  `atlas_proc_run` with an explicit argv and an absolute `argv[0]`.
- **Layers do not short-circuit.** A renderer never queries; the service layer
  never formats; `sqlite3` types never leave `src/db`.
- **Every fallible function returns `atlas_status` and takes an `atlas_err *`.**
  One exit path per function; no early `return` that skips a release.
- **`atlas_db_prepare` caches by string-literal pointer.** Pass string
  literals and bind parameters; never format SQL into a reused buffer.
- **Row callbacks receive borrowed pointers** valid only for the call. Copy
  anything that must outlive them.
- **Schema changes are numbered transactional migrations.** A migration that
  rebuilds a table verifies its own row preservation before it commits.
- **Paths are bytes.** Store and look up by `path_raw`; `path_text` is the
  lossless `%XX` display form. Never split a path on whitespace.
- **Untrusted text** (filenames, git output, error text) is encoded with
  `atlas_safe()` / `atlas_text_encode_safe()` at the point of output. Do not
  double-encode values already stored encoded.
- **A new `.c` file** is added to the explicit `atlas_core` source list in
  `CMakeLists.txt`. There is no glob.
- **A new test** is added to `ATLAS_TESTS` in `tests/CMakeLists.txt` **and** to
  one of the `set_tests_properties(... LABELS ...)` lines.
- **A new command touches five places:** a service function, a method on
  `atlas_renderer_vtbl` in `src/cli/render.h`, an implementation in **both**
  `render_human.c` and `render_json.c`, dispatch plus help in `src/cli/cli.c`,
  **and the `COMMANDS[]` table in `is_a_command`**. Run the built binary once.
- **Tests always override the data directory** with a temporary path and never
  open the real user database.
- **No commits, pushes or merges** unless the operator explicitly asks. Each
  task below ends with a commit step; ask before running it the first time and
  follow the operator's answer for the rest of the plan.

**The fixture API, verified against `tests/support/fixture.h`.** The type is
`fixture`, not `fx`; `fx_open` returns an `atlas_status` and takes an
`atlas_err *`; the repository directory is `fx_repo`, not `fx_repo_dir`; and
`fx_open` creates the tree but **not** a git repository — `fx_init_repo` does
that. Every test below opens like this:

```c
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);
    T_OK(fx_init_repo(&fx, fx_repo(&fx), "sha1", &err), &err);
    /* … */
    fx_close(&fx);
```

`atlas_doctor_report`, `atlas_doctor_report_init/_free` and
`atlas_service_doctor` all live in `include/atlas/service.h`, not in
`include/atlas/doctor.h`. There is no such header.

**The test entry point is `ATLAS_TEST_MAIN(suite, TESTS)`** — two arguments, a
suite name and a `static const atlas_test TESTS[]` array of
`{"human readable name", function}` pairs. It is not a list of function names.
Every test file below ends like this:

```c
static const atlas_test TESTS[] = {
    {"what this case proves", test_function_name},
};

ATLAS_TEST_MAIN("suite_name", TESTS)
```

`fx_db_scalar_int` takes the **data directory**, not an `atlas_db *`:
`atlas_db_prepare` is not public to tests, so the helper opens its own
read-only connection the way `fx_daemon_schema_ready` already does.

---

### Task 1: Migration 27 — the column

**Files:**
- Modify: `include/atlas/db.h:24` (bump `ATLAS_SCHEMA_VERSION`)
- Modify: `src/db/migrate.c` (add `M27_*` statements and the table entry after `{26, …}`)
- Test: `tests/test_migrate_scanner_uid.c` (create)
- Modify: `tests/CMakeLists.txt` (add to `ATLAS_TESTS` and a `LABELS` line)

**Interfaces:**
- Consumes: nothing.
- Produces: column `repositories.scanner_uid INTEGER NOT NULL DEFAULT 0`, where
  `0` means *no scanner assigned*. `ATLAS_SCHEMA_VERSION == 27`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_migrate_scanner_uid.c`:

```c
/* Atlas - A13: migration 27 gives every repository a scanner uid column.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0. */
#include "atlas_test.h"

#include "atlas/db.h"
#include "support/fixture.h"

#include <string.h>

/* The column exists, defaults to 0, and 0 means "no scanner assigned" —
 * never "uid 0", which is root and is refused in Task 3. */
static void test_migration_27_adds_scanner_uid_defaulting_to_unset(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);

    T_EQ_INT((int)ATLAS_SCHEMA_VERSION, 27);

    atlas_db *db = NULL;
    T_OK(atlas_db_open(fx_data_dir(&fx), &db, &err), &err);

    /* The column exists on a freshly migrated database, and its declared
     * default is the unset value. Asserted against the schema rather than
     * against a row, because this task registers nothing. */
    int64_t dflt = -1;
    T_OK(fx_db_scalar_int(db,
                          "SELECT COUNT(*) FROM pragma_table_info('repositories') "
                          "WHERE name = 'scanner_uid' AND \"notnull\" = 1 AND dflt_value = '0';",
                          &dflt, &err),
         &err);
    T_EQ_INT((int)dflt, 1);

    atlas_db_close(db);
    fx_close(&fx);
}

ATLAS_TEST_MAIN(test_migration_27_adds_scanner_uid_defaulting_to_unset)
```

If `fx_db_scalar_int` does not exist in `tests/support/fixture.h`, add it in
this task — it is a two-line helper the later tasks reuse:

```c
/* Runs a single-column, single-row query and returns the integer. The SQL must
 * be a string literal: `atlas_db_prepare` caches by pointer. */
atlas_status fx_db_scalar_int(atlas_db *db, const char *sql, int64_t *out, atlas_err *err);
```

- [ ] **Step 2: Run it and watch it fail**

```sh
cd /opt/atlas && cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j 4 --target test_migrate_scanner_uid
./build/tests/test_migrate_scanner_uid
```

Expected: FAIL — `ATLAS_SCHEMA_VERSION` is 26, and `no such column: scanner_uid`.

- [ ] **Step 3: Add the migration**

In `src/db/migrate.c`, beside the other `M26_*` constants:

```c
/* A13. Which uid's scanner may report facts about this repository.
 *
 * Additive: one column, no table rebuilt, so foreign keys stay enforced and no
 * existing row is rewritten. The default is 0 and 0 means **unassigned**, not
 * "uid 0" — root is refused as a scanner uid in `atlas_scanner_uid_refusal`.
 * A repository that predates this migration has no scanner and must not be
 * given one by a default: inventing an assignment nobody expressed is
 * migration 19's mistake. `atlas repo scanner` is how an operator assigns one. */
static const char M27_SCANNER_UID[] =
    "ALTER TABLE repositories ADD COLUMN scanner_uid INTEGER NOT NULL DEFAULT 0;";

static const char *const M27_STATEMENTS[] = {M27_SCANNER_UID, NULL};
```

Then, immediately after the `{26, …}` entry in `MIGRATIONS[]`:

```c
    /* Additive: one column, no table rebuilt, so foreign keys stay enforced.
     * See the M27 comment for why the default is 0-meaning-unassigned. */
    {27, "which uid's scanner may report about a repository", M27_STATEMENTS, false},
```

And in `include/atlas/db.h`:

```c
#define ATLAS_SCHEMA_VERSION 27
```

- [ ] **Step 4: Run it and watch it pass**

```sh
cmake --build build -j 4 --target test_migrate_scanner_uid && ./build/tests/test_migrate_scanner_uid
```

Expected: PASS.

- [ ] **Step 5: Prove the whole suite still migrates**

```sh
cd build && ctest -L unit --output-on-failure -j 4
```

Expected: PASS. A schema bump breaks any test that hardcodes 26 — fix those by
referring to `ATLAS_SCHEMA_VERSION`, never by writing 27.

- [ ] **Step 6: Wire the test in**

`tests/CMakeLists.txt`: add `test_migrate_scanner_uid` to `ATLAS_TESTS` and to
the `unit` `LABELS` line. An unlabelled test is invisible to `ctest -L unit`.

- [ ] **Step 7: Commit**

```bash
git add include/atlas/db.h src/db/migrate.c tests/test_migrate_scanner_uid.c \
        tests/support/fixture.h tests/support/fixture.c tests/CMakeLists.txt
git commit -m "feat(a13): migration 27 - which uid's scanner may report about a repository"
```

---

### Task 2: Reading and writing the column

**Files:**
- Modify: `include/atlas/service.h` (add `scanner_uid` to `atlas_repo_info`)
- Modify: `include/atlas/db.h` (declare the two typed operations)
- Modify: `src/db/db_repo.c` (implement them; extend the repo row read)
- Test: `tests/test_migrate_scanner_uid.c` (extend)

**Interfaces:**
- Consumes: Task 1's column.
- Produces:

```c
/* Sets the uid whose scanner may report about `repo_id`. `uid` of 0 clears the
 * assignment. Refusals are the caller's job: this writes what it is given. */
atlas_status atlas_db_repo_set_scanner_uid(atlas_db *db, int64_t repo_id, int64_t uid,
                                           atlas_err *err);

/* 0 when no scanner is assigned. */
atlas_status atlas_db_repo_scanner_uid(atlas_db *db, int64_t repo_id, int64_t *out,
                                       atlas_err *err);
```

and `atlas_repo_info.scanner_uid` (`int64_t`, 0 when unassigned), populated
everywhere `atlas_repo_info` is filled.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_migrate_scanner_uid.c` and add it to `ATLAS_TEST_MAIN`:

```c
static void test_scanner_uid_round_trips_and_zero_clears(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);
    T_OK(fx_init_repo(&fx, fx_repo(&fx), "sha1", &err), &err);

    atlas_db *db = NULL;
    T_OK(atlas_db_open(fx_data_dir(&fx), &db, &err), &err);

    /* The signature here is the one that exists *now*, before Task 4 widens it.
     * Task 4's Step 3 updates this call along with every other call site. */
    atlas_repo_info reg;
    atlas_repo_info_init(&reg);
    T_OK(atlas_service_repo_add_db(db, fx_repo(&fx), "r", false, &reg, &err), &err);
    int64_t repo_id = reg.id;
    atlas_repo_info_free(&reg);

    int64_t got = -1;
    T_OK(atlas_db_repo_scanner_uid(db, repo_id, &got, &err), &err);
    T_EQ_INT((int)got, 0);

    T_OK(atlas_db_repo_set_scanner_uid(db, repo_id, 1000, &err), &err);
    T_OK(atlas_db_repo_scanner_uid(db, repo_id, &got, &err), &err);
    T_EQ_INT((int)got, 1000);

    /* Zero clears rather than meaning root. */
    T_OK(atlas_db_repo_set_scanner_uid(db, repo_id, 0, &err), &err);
    T_OK(atlas_db_repo_scanner_uid(db, repo_id, &got, &err), &err);
    T_EQ_INT((int)got, 0);

    atlas_db_close(db);
    fx_close(&fx);
}
```

- [ ] **Step 2: Run it and watch it fail**

```sh
cmake --build build -j 4 --target test_migrate_scanner_uid && ./build/tests/test_migrate_scanner_uid
```

Expected: FAIL — `atlas_db_repo_set_scanner_uid` is not defined (a link error,
which is what an unlisted symbol looks like here).

- [ ] **Step 3: Implement in `src/db/db_repo.c`**

```c
atlas_status atlas_db_repo_set_scanner_uid(atlas_db *db, int64_t repo_id, int64_t uid,
                                           atlas_err *err) {
    static const char SQL[] = "UPDATE repositories SET scanner_uid = ?1 WHERE id = ?2;";
    sqlite3_stmt *q = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &q, err);
    if (st != ATLAS_OK) {
        return st;
    }
    (void)sqlite3_bind_int64(q, 1, uid);
    (void)sqlite3_bind_int64(q, 2, repo_id);
    return atlas_db_step_done(db, q, err);
}

atlas_status atlas_db_repo_scanner_uid(atlas_db *db, int64_t repo_id, int64_t *out,
                                       atlas_err *err) {
    static const char SQL[] = "SELECT scanner_uid FROM repositories WHERE id = ?1;";
    sqlite3_stmt *q = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &q, err);
    if (st != ATLAS_OK) {
        return st;
    }
    (void)sqlite3_bind_int64(q, 1, repo_id);
    int64_t v = 0;
    if (sqlite3_step(q) == SQLITE_ROW) {
        v = sqlite3_column_int64(q, 0);
    }
    atlas_db_finish(db, q);
    if (out != NULL) {
        *out = v;
    }
    return ATLAS_OK;
}
```

Add `int64_t scanner_uid;` to `atlas_repo_info` in `include/atlas/service.h`,
add `scanner_uid` to the `SELECT` list wherever `db_repo.c` fills an
`atlas_repo_info`, and copy it into the struct there.

- [ ] **Step 4: Run it and watch it pass**

```sh
cmake --build build -j 4 --target test_migrate_scanner_uid && ./build/tests/test_migrate_scanner_uid
cd build && ctest -L unit --output-on-failure -j 4
```

Expected: PASS, both.

- [ ] **Step 5: Commit**

```bash
git add include/atlas/db.h include/atlas/service.h src/db/db_repo.c tests/test_migrate_scanner_uid.c
git commit -m "feat(a13): read and write a repository's scanner uid"
```

---

### Task 3: Deriving the uid, and the three refusals

**Files:**
- Create: `src/core/scanner_uid.c`
- Create: `include/atlas/scanner_uid.h`
- Modify: `CMakeLists.txt` (add `src/core/scanner_uid.c` to the `atlas_core` source list)
- Test: `tests/test_scanner_uid.c` (create)
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `atlas_orchpolicy` (`dispatcher_uid`, `model_dispatcher_uid`),
  `atlas_gwpolicy` (`gateway_uid`), `atlas_syspolicy`.
- Produces:

```c
/* The uid that owns `root`, which is the uid whose scanner may read it.
 * `lstat`, never following a link on the final component. */
atlas_status atlas_scanner_uid_of_root(const char *root, int64_t *out, atlas_err *err);

/* Why `uid` may not be a scanner uid, or NULL when it may.
 * The returned string is a static literal. In a per-user deployment this
 * always returns NULL: there, the daemon's uid *is* the operator's uid, and
 * refusing it would forbid the whole deployment mode. */
const char *atlas_scanner_uid_refusal(int64_t uid);
```

- [ ] **Step 1: Write the failing test**

Create `tests/test_scanner_uid.c`:

```c
/* Atlas - A13: which uid may scan, and which three may never.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0. */
#include "atlas_test.h"

#include "atlas/scanner_uid.h"
#include "support/fixture.h"

#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The owner of the root is the answer, and it is read with lstat so a root
 * replaced by a symlink since registration does not redirect the question. */
static void test_the_root_s_owner_is_the_scanner_uid(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);
    T_OK(fx_init_repo(&fx, fx_repo(&fx), "sha1", &err), &err);

    int64_t uid = -1;
    T_OK(atlas_scanner_uid_of_root(fx_repo(&fx), &uid, &err), &err);
    T_EQ_INT((int)uid, (int)getuid());

    fx_close(&fx);
}

/* A root that does not exist is an error with a message, never a silent 0 —
 * 0 means "unassigned" and must never be produced by a failed read. */
static void test_a_missing_root_is_an_error_not_zero(void) {
    atlas_err err;
    atlas_err_init(&err);
    int64_t uid = -1;
    T_CHECK(atlas_scanner_uid_of_root("/nonexistent/atlas/a13", &uid, &err) != ATLAS_OK);
    T_CHECK(err.msg[0] != '\0');
}

/* Root is never a scanner uid: 0 is the column's "unassigned" value, so
 * accepting it would make an assignment indistinguishable from its absence. */
static void test_root_is_refused(void) {
    T_CHECK(atlas_scanner_uid_refusal(0) != NULL);
}

ATLAS_TEST_MAIN(test_the_root_s_owner_is_the_scanner_uid, test_a_missing_root_is_an_error_not_zero,
                test_root_is_refused)
```

- [ ] **Step 2: Run it and watch it fail**

```sh
cmake --build build -j 4 --target test_scanner_uid 2>&1 | tail -5
```

Expected: FAIL — `atlas/scanner_uid.h` does not exist.

- [ ] **Step 3: Implement**

`include/atlas/scanner_uid.h`:

```c
/* Atlas - A13: which uid's scanner may report about a repository.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The answer is the owner of the repository root. It is not derived from who
 * ran `atlas repo add`: registration is a local write under the data-directory
 * lock with no socket peer to ask, and which uid performs it depends on the
 * deployment — the operator's own in a per-user install, the daemon's in a
 * system one. An answer that changes with the deployment is not an identity. */
#ifndef ATLAS_SCANNER_UID_H
#define ATLAS_SCANNER_UID_H

#include "atlas/error.h"

#include <stdint.h>

atlas_status atlas_scanner_uid_of_root(const char *root, int64_t *out, atlas_err *err);
const char *atlas_scanner_uid_refusal(int64_t uid);

#endif /* ATLAS_SCANNER_UID_H */
```

`src/core/scanner_uid.c`:

```c
#include "atlas/scanner_uid.h"

#include "atlas/gwpolicy.h"
#include "atlas/orchpolicy.h"
#include "atlas/syspolicy.h"

#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

atlas_status atlas_scanner_uid_of_root(const char *root, int64_t *out, atlas_err *err) {
    if (root == NULL || root[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a repository root is required");
    }
    struct stat sb;
    memset(&sb, 0, sizeof(sb));
    /* lstat, not stat: a root replaced by a symlink since registration must not
     * redirect the question to whatever it points at. */
    if (lstat(root, &sb) != 0) {
        return atlas_err_set_errno(err, ATLAS_ERR_REPO, errno,
                                   "cannot read the repository root to learn its owner");
    }
    if (!S_ISDIR(sb.st_mode)) {
        return atlas_err_set(err, ATLAS_ERR_REPO,
                             "the repository root is not a directory, so it has no owner to scan "
                             "as");
    }
    if (out != NULL) {
        *out = (int64_t)sb.st_uid;
    }
    return ATLAS_OK;
}

const char *atlas_scanner_uid_refusal(int64_t uid) {
    /* Root is refused unconditionally and in every deployment: 0 is the
     * column's "unassigned" value, so accepting uid 0 would make an assignment
     * indistinguishable from its absence. */
    if (uid == 0) {
        return "uid 0 is how the column records \"no scanner assigned\", so it cannot also name "
               "one";
    }

    /* The remaining three refusals are about principals that do not own the
     * trees they would report on. This season is safe because the reporting
     * principal owns the files it reports: whatever it could misreport, it
     * could equally write. A principal that owns none of the tree breaks that
     * equivalence.
     *
     * In a per-user deployment the daemon's uid *is* the operator's uid and
     * does own the tree, so there is nothing to refuse and this returns NULL. */
    atlas_syspolicy sp;
    atlas_syspolicy_load(&sp);
    if (!atlas_syspolicy_is_system_scope(&sp)) {
        return NULL;
    }

    atlas_orchpolicy op;
    atlas_orchpolicy_load(&op);
    if (op.dispatcher_uid > 0 && uid == op.dispatcher_uid) {
        return "the orchestration worker owns none of the trees it would report on";
    }
    if (op.model_dispatcher_uid > 0 && uid == op.model_dispatcher_uid) {
        return "the model dispatcher owns none of the trees it would report on";
    }

    atlas_gwpolicy gp;
    atlas_gwpolicy_load(&gp);
    if (gp.gateway_uid > 0 && uid == gp.gateway_uid) {
        return "the gateway owns none of the trees it would report on";
    }
    return NULL;
}
```

If `atlas_syspolicy_is_system_scope` does not exist publicly, add it in this
task as a two-line accessor beside `atlas_syspolicy_load` in
`include/atlas/syspolicy.h` and `src/core/syspolicy.c`; `scope_is_system` in
`src/ipc/sock.c` is the same predicate and must be changed to call the new one
rather than keeping a second copy.

Add `src/core/scanner_uid.c` to the `atlas_core` source list in
`CMakeLists.txt` — there is no glob, and an unlisted file surfaces as a link
error, not a build error.

- [ ] **Step 4: Run it and watch it pass**

```sh
cmake --build build -j 4 --target test_scanner_uid && ./build/tests/test_scanner_uid
```

Expected: PASS.

- [ ] **Step 5: Wire the test in and run the unit subset**

`tests/CMakeLists.txt`: `test_scanner_uid` into `ATLAS_TESTS` and the `unit`
`LABELS` line.

```sh
cd build && ctest -L unit --output-on-failure -j 4
```

- [ ] **Step 6: Commit**

```bash
git add include/atlas/scanner_uid.h src/core/scanner_uid.c CMakeLists.txt \
        tests/test_scanner_uid.c tests/CMakeLists.txt include/atlas/syspolicy.h src/core/syspolicy.c src/ipc/sock.c
git commit -m "feat(a13): the root's owner is the scanner uid, and the three that may never be"
```

---

### Task 4: `repo add` records it

**Files:**
- Modify: `src/core/service.c:539-610` (`atlas_service_repo_add_db`)
- Modify: `include/atlas/service.h` (the `exact_root` sibling gains a uid argument)
- Modify: `src/cli/cli.c:4164` (accept `--scanner-uid`)
- Test: `tests/test_repo_scanner.c` (create)
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `atlas_scanner_uid_of_root`, `atlas_scanner_uid_refusal`,
  `atlas_db_repo_set_scanner_uid`.
- Produces:

```c
/* `scanner_uid_given` false derives the uid from the root's owner; true takes
 * `scanner_uid` as the operator's choice. The flag is separate from the value
 * because 0 already means "unassigned" in the column and cannot also mean
 * "derive". `out->scanner_uid` carries what was stored. */
atlas_status atlas_service_repo_add_db(atlas_db *db, const char *path, const char *name,
                                       bool exact_root, bool scanner_uid_given,
                                       int64_t scanner_uid, atlas_repo_info *out, atlas_err *err);
```

  Every existing caller gains `false, 0` — including the Task 2 test, whose
  six-argument call is written against the signature as it stands before this
  task.

- [ ] **Step 1: Write the failing test**

Create `tests/test_repo_scanner.c`:

```c
/* Atlas - A13: registration records which uid's scanner may read the tree.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0. */
#include "atlas_test.h"

#include "atlas/service.h"
#include "support/fixture.h"

#include <unistd.h>

/* Registration derives the uid from the root's owner, with no operator input. */
static void test_registration_derives_the_uid_from_the_root(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);
    T_OK(fx_init_repo(&fx, fx_repo(&fx), "sha1", &err), &err);
    atlas_db *db = NULL;
    T_OK(atlas_db_open(fx_data_dir(&fx), &db, &err), &err);

    atlas_repo_info info;
    atlas_repo_info_init(&info);
    T_OK(atlas_service_repo_add_db(db, fx_repo(&fx), "r", false, false, 0, &info, &err), &err);
    T_EQ_INT((int)info.scanner_uid, (int)getuid());

    atlas_repo_info_free(&info);
    atlas_db_close(db);
    fx_close(&fx);
}

/* An explicit override wins over the derived value. */
static void test_an_explicit_uid_overrides_the_derived_one(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);
    T_OK(fx_init_repo(&fx, fx_repo(&fx), "sha1", &err), &err);
    atlas_db *db = NULL;
    T_OK(atlas_db_open(fx_data_dir(&fx), &db, &err), &err);

    atlas_repo_info info;
    atlas_repo_info_init(&info);
    T_OK(atlas_service_repo_add_db(db, fx_repo(&fx), "r", false, true, 4242, &info, &err),
         &err);
    T_EQ_INT((int)info.scanner_uid, 4242);

    atlas_repo_info_free(&info);
    atlas_db_close(db);
    fx_close(&fx);
}

/* A refused uid is refused at registration and names the reason. Registration
 * fails; it does not silently store 0, because 0 means "unassigned" and a
 * refusal is not an absence. */
static void test_a_refused_uid_fails_registration_with_a_reason(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);
    T_OK(fx_init_repo(&fx, fx_repo(&fx), "sha1", &err), &err);
    atlas_db *db = NULL;
    T_OK(atlas_db_open(fx_data_dir(&fx), &db, &err), &err);

    /* `(true, 0)` is the operator naming root, which is refused. `(false, 0)`
     * is "derive from the root's owner" and is not — which is why the flag is
     * a separate parameter rather than a sentinel value. */
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    T_CHECK(atlas_service_repo_add_db(db, fx_repo(&fx), "r", false, true, 0, &info, &err) !=
            ATLAS_OK);
    T_CHECK(err.msg[0] != '\0');

    atlas_repo_info_free(&info);
    atlas_db_close(db);
    fx_close(&fx);
}

ATLAS_TEST_MAIN(test_registration_derives_the_uid_from_the_root,
                test_an_explicit_uid_overrides_the_derived_one,
                test_a_refused_uid_fails_registration_with_a_reason)
```

`0` cannot mean both "derive" and "the operator said root", which is why the
signature carries a separate `bool scanner_uid_given` rather than overloading
the value.

- [ ] **Step 2: Run it and watch it fail**

```sh
cmake --build build -j 4 --target test_repo_scanner 2>&1 | tail -5
```

Expected: FAIL — `atlas_service_repo_add_db` takes six arguments, not seven.

- [ ] **Step 3: Implement**

Change the signature in `include/atlas/service.h` and `src/core/service.c`:

```c
atlas_status atlas_service_repo_add_db(atlas_db *db, const char *path, const char *name,
                                       bool exact_root, bool scanner_uid_given,
                                       int64_t scanner_uid, atlas_repo_info *out, atlas_err *err);
```

In `atlas_service_repo_add_db`, after `atlas_db_repo_add` succeeds and before
`atlas_db_index_state_ensure`:

```c
    /* Which uid's scanner may read this tree. Derived from the root's owner
     * unless the operator named one. A refusal fails the registration rather
     * than storing 0: 0 is "unassigned", and a refusal is not an absence. */
    int64_t suid = scanner_uid;
    if (!scanner_uid_given) {
        st = atlas_scanner_uid_of_root(root, &suid, err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    {
        const char *why = atlas_scanner_uid_refusal(suid);
        if (why != NULL) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "uid %lld cannot be this repository's scanner: %s",
                                 (long long)suid, why);
        }
    }
    st = atlas_db_repo_set_scanner_uid(db, id, suid, err);
    if (st != ATLAS_OK) {
        return st;
    }
    out->scanner_uid = suid;
```

Update the one-line wrapper `atlas_service_repo_add` and every other call site;
`grep -rn atlas_service_repo_add_db src/ tests/` finds them all.

In `src/cli/cli.c`, accept `--scanner-uid N` on `repo add` and pass
`(true, N)`; without it pass `(false, 0)`. Add the flag to the usage string at
`src/cli/cli.c:4167` and to the `--help` body.

- [ ] **Step 4: Run it and watch it pass**

```sh
cmake --build build -j 4 --target test_repo_scanner && ./build/tests/test_repo_scanner
cd build && ctest -L unit --output-on-failure -j 4
```

- [ ] **Step 5: Drive the real binary once**

The failure mode this catches is the one CLAUDE.md calls the misleading one:
everything wired, every service test passing, and the binary still answering
`unknown command` or ignoring the flag.

```sh
cd /tmp && rm -rf a13 && mkdir a13 && cd a13 && git init -q . && git commit -q --allow-empty -m x
/opt/atlas/build/atlas --data-dir /tmp/a13/data repo add /tmp/a13
/opt/atlas/build/atlas --data-dir /tmp/a13/data repo add /tmp/a13 --name two --scanner-uid 4242
```

Expected: the first prints a scanner uid equal to your own; the second prints
4242.

- [ ] **Step 6: Wire the test in and commit**

`tests/CMakeLists.txt`: `test_repo_scanner` into `ATLAS_TESTS` and the
`integration` `LABELS` line.

```bash
git add include/atlas/service.h src/core/service.c src/cli/cli.c \
        tests/test_repo_scanner.c tests/CMakeLists.txt
git commit -m "feat(a13): registration records the scanner uid, and refuses the three that may not"
```

---

### Task 5: `atlas repo scanner` for repositories that already exist

**Files:**
- Modify: `src/core/service.c` (add `atlas_service_repo_set_scanner`)
- Modify: `include/atlas/service.h`
- Modify: `src/cli/render.h` (a method on `atlas_renderer_vtbl`)
- Modify: `src/cli/render_human.c` **and** `src/cli/render_json.c`
- Modify: `src/cli/cli.c` (dispatch, help text, **and `COMMANDS[]` in `is_a_command`**)
- Test: `tests/test_repo_scanner.c` (extend)

**Interfaces:**
- Consumes: Task 3 and Task 4.
- Produces: `atlas repo scanner NAME [--uid N]` — re-derives from the root's
  owner, or sets the uid given. Prints the stored value.

```c
/* Assigns `name`'s scanner uid. Derives from the root's owner when
 * `uid_given` is false. Fills `out` with the repository as stored afterwards. */
atlas_status atlas_service_repo_set_scanner(atlas_ctx *ctx, const char *name, bool uid_given,
                                            int64_t uid, atlas_repo_info *out, atlas_err *err);
```

- [ ] **Step 1: Write the failing test**

Append to `tests/test_repo_scanner.c` and add to `ATLAS_TEST_MAIN`:

```c
/* A repository registered before A13 carries 0, and the command is how an
 * operator assigns one without re-registering. */
static void test_the_command_assigns_a_uid_to_an_existing_repository(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);
    T_OK(fx_init_repo(&fx, fx_repo(&fx), "sha1", &err), &err);
    atlas_db *db = NULL;
    T_OK(atlas_db_open(fx_data_dir(&fx), &db, &err), &err);

    atlas_repo_info info;
    atlas_repo_info_init(&info);
    T_OK(atlas_service_repo_add_db(db, fx_repo(&fx), "r", false, false, 0, &info, &err),
         &err);
    /* Simulate a pre-A13 row. */
    T_OK(atlas_db_repo_set_scanner_uid(db, info.id, 0, &err), &err);
    atlas_repo_info_free(&info);

    atlas_repo_info after;
    atlas_repo_info_init(&after);
    T_OK(atlas_service_repo_set_scanner_db(db, "r", false, 0, &after, &err), &err);
    T_EQ_INT((int)after.scanner_uid, (int)getuid());

    atlas_repo_info_free(&after);
    atlas_db_close(db);
    fx_close(&fx);
}
```

Provide `atlas_service_repo_set_scanner_db` as the `atlas_db *`-taking sibling,
matching the existing `atlas_service_repo_add` / `_add_db` pair, so the test
needs no `atlas_ctx`.

- [ ] **Step 2: Run it and watch it fail**

```sh
cmake --build build -j 4 --target test_repo_scanner 2>&1 | tail -5
```

Expected: FAIL — undefined `atlas_service_repo_set_scanner_db`.

- [ ] **Step 3: Implement the service function**

In `src/core/service.c`, mirroring `atlas_service_repo_add_db`'s shape: resolve
the name to a row, read its root, derive or take the uid, apply
`atlas_scanner_uid_refusal`, call `atlas_db_repo_set_scanner_uid`, then fill
`out` by re-reading the row so the caller sees what is stored rather than what
was asked for.

- [ ] **Step 4: Wire the command through all five places**

1. `src/cli/render.h`: add `atlas_status (*repo_scanner_set)(atlas_renderer *r, const atlas_repo_info *ri, atlas_err *err);` to `atlas_renderer_vtbl`.
2. `src/cli/render_human.c`: print `  scanner uid       %lld` beside the other repository fields. The uid is an Atlas integer, not repository text, so it is printed as-is.
3. `src/cli/render_json.c`: emit `"scanner_uid"` as a number. Add it to `j_repo_item` and `j_repo_added` too, so `repo list --json` and `repo add --json` carry it.
4. `src/cli/cli.c`: dispatch `repo scanner NAME [--uid N]`, and extend the `repo` usage string to `usage: atlas repo add|list|remove|scanner ...`.
5. `src/cli/cli.c`: add `"repo scanner"` to the `COMMANDS[]` table in
   `is_a_command`. **This is the one that gets forgotten**, and its failure
   mode is that every test passes and the binary answers `unknown command`.

- [ ] **Step 5: Run it and watch it pass**

```sh
cmake --build build -j 4 && ./build/tests/test_repo_scanner
cd build && ctest -L unit -L integration --output-on-failure -j 4
```

- [ ] **Step 6: Drive the real binary**

```sh
/opt/atlas/build/atlas --data-dir /tmp/a13/data repo scanner two
/opt/atlas/build/atlas --data-dir /tmp/a13/data repo scanner two --uid 4242
/opt/atlas/build/atlas --data-dir /tmp/a13/data repo list --json
```

Expected: the first re-derives from the root's owner, the second sets 4242, and
the JSON listing carries `"scanner_uid"` for both repositories.

- [ ] **Step 7: Commit**

```bash
git add include/atlas/service.h src/core/service.c src/cli/render.h \
        src/cli/render_human.c src/cli/render_json.c src/cli/cli.c \
        tests/test_repo_scanner.c
git commit -m "feat(a13): atlas repo scanner assigns a uid without re-registering"
```

---

### Task 6: `atlas doctor` names a repository with no scanner

**Files:**
- Modify: `src/core/doctor.c` (add the finding)
- Modify: `include/atlas/service.h` (add the count to `atlas_doctor_report`)
- Modify: `src/cli/render_human.c`, `src/cli/render_json.c`
- Test: `tests/test_repo_scanner.c` (extend)

**Interfaces:**
- Consumes: Task 2's `atlas_db_repo_scanner_uid`.
- Produces: `atlas_doctor_report.repos_without_scanner` (`int`), and a problem
  line naming each repository.

- [ ] **Step 1: Write the failing test**

```c
/* A repository with no scanner is a finding, never silence: after Plan 2 it
 * will also be a repository that is never current, and an operator must be
 * able to learn why from `atlas doctor` alone. */
static void test_doctor_names_a_repository_with_no_scanner(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);
    T_OK(fx_init_repo(&fx, fx_repo(&fx), "sha1", &err), &err);
    atlas_db *db = NULL;
    T_OK(atlas_db_open(fx_data_dir(&fx), &db, &err), &err);

    atlas_repo_info info;
    atlas_repo_info_init(&info);
    T_OK(atlas_service_repo_add_db(db, fx_repo(&fx), "r", false, false, 0, &info, &err),
         &err);
    T_OK(atlas_db_repo_set_scanner_uid(db, info.id, 0, &err), &err);
    atlas_repo_info_free(&info);

    atlas_doctor_report rep;
    atlas_doctor_report_init(&rep);
    T_OK(atlas_service_doctor_db(db, &rep, &err), &err);
    T_EQ_INT(rep.repos_without_scanner, 1);
    /* `atlas_service_doctor` takes an `atlas_ctx`; this task adds the
     * `atlas_db *`-taking sibling `atlas_service_doctor_db` beside it and
     * makes the existing function a one-line wrapper, matching the
     * `atlas_service_repo_add` / `_add_db` pair that already exists. */

    atlas_doctor_report_free(&rep);
    atlas_db_close(db);
    fx_close(&fx);
}
```

- [ ] **Step 2: Run it and watch it fail**

```sh
cmake --build build -j 4 --target test_repo_scanner 2>&1 | tail -5
```

Expected: FAIL — no member `repos_without_scanner`.

- [ ] **Step 3: Implement**

Count repositories whose `scanner_uid` is 0 and add one problem line per
repository, encoded with `atlas_safe()` because a repository name is untrusted
text:

```c
    "repository \"%s\" has no scanner uid; nothing may report about it. "
    "Assign one with `atlas repo scanner %s`."
```

`atlas doctor` observes and creates nothing — this adds a count and a message
and must not assign anything.

- [ ] **Step 4: Run it and watch it pass**

```sh
cmake --build build -j 4 && ./build/tests/test_repo_scanner
cd build && ctest --output-on-failure -j 4
```

Expected: the full suite passes. `test_plugin.c` snapshots a fresh HOME around
`atlas doctor` and asserts nothing appeared — if it fails, the new code created
something and must stop.

- [ ] **Step 5: Commit**

```bash
git add include/atlas/service.h src/core/doctor.c src/cli/render_human.c \
        src/cli/render_json.c tests/test_repo_scanner.c
git commit -m "feat(a13): doctor names a repository that has no scanner uid"
```

---

### Task 7: The gates

**Files:** none — this task runs what exists.

- [ ] **Step 1: Full release suite**

```sh
cd /opt/atlas && make test
```

Expected: 100 % pass. The suite is 99 tests before this plan and 101 after.

- [ ] **Step 2: Sanitizers**

This plan changes production code, so the full matrix applies.

```sh
make asan
make ubsan
make tsan
```

Expected: all pass, 0 races, 0 leaks. `test_scanner_uid` allocates nothing, but
`atlas_repo_info` gained a member and every `_free` path must still be exact.

- [ ] **Step 3: Adversarial**

```sh
make adversarial
```

Expected: pass. The new `lstat` on a repository root is a read of a path Atlas
already trusted enough to register, but it is a new syscall on that path and
the hostile-repository checks should see it.

- [ ] **Step 4: Record a change reason**

Atlas indexes itself. Record why these paths changed, truthfully, through the
MCP tool or `atlas record-reason`; record `UNKNOWN` rather than inventing one.

- [ ] **Step 5: Report to the operator**

Report: the schema version before and after, the two new tests and their
labels, the five wiring places for the new command with the `COMMANDS[]` entry
called out explicitly, the full-suite and sanitizer results as observations
(counts and pass/fail, not adjectives), and the fact that no behaviour changed
— no scanner exists yet, and nothing reads `scanner_uid` to decide anything.

---

## What this plan deliberately does not do

- No scanner process, no `scanner.` RPC group, no mirror, no watcher movement.
- Nothing reads `scanner_uid` to gate, refuse or route anything. A repository
  with `scanner_uid = 0` behaves exactly as it does today.
- No backfill of the two existing repositories. Migration 27 leaves them at 0
  and `atlas doctor` says so; assigning is an operator action through Task 5's
  command, because a migration cannot `stat` and inventing an assignment nobody
  expressed is the one thing a migration must not do.
