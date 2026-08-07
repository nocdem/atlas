# Structural code intelligence (A3)

A3 gives Atlas something structural to say about C-family source: what a file
defines, what it includes, what depends on it, and what *might* be affected if it
changes. This document is the contract for that: which facts Atlas records, how
sure it is of each one, and — at least as importantly — what it explicitly does
not claim.

It is written first because the honest part of A3 is the truth model, not the
parser. A structural index that cannot say "I am not sure" is worse than no
index, because a wrong exact edge is indistinguishable from a right one.

## The one sentence

> **Atlas is not a compiler and does not pretend to be one.** Every structural
> fact carries a resolution class saying how it was arrived at, and the classes
> that mean "proven" are only ever used for facts a byte-level reader can
> actually prove.

## What A3 answers

| question | answered by |
| --- | --- |
| What is this file's role? | typed, evidence-backed roles with the basis stated |
| What symbols does it define or declare? | `code_symbols`, per file, per site |
| Which files does it include? | `file_includes_file`, with the include spelling kept |
| Which files depend on it? | `file_depends_on_file`, traversed inbound |
| Which functions appear to call which? | `symbol_calls_symbol`, always a *candidate* |
| Where is a symbol defined and declared? | `symbol_defined_by` / `symbol_declared_by` |
| Is a relation exact, inferred, ambiguous or unresolved? | the resolution class on every edge |
| Which translation units use a file? | `unit_compiles_file`, `unit_uses_header` |
| What may be affected if this changes? | bounded inbound traversal, with a path per result |
| Is the structural index current or stale? | `code_index_state`, its own generation pair |

## Resolution classes

Every symbol, occurrence and relation carries exactly one of these. They are a
CHECK constraint in the schema and an enum in `include/atlas/code.h`, so a value
outside the set cannot be stored.

| class | means | example |
| --- | --- | --- |
| `SOURCE_EXACT` | read directly from the bytes; no inference at all | the file contains `#include "buf.h"` at line 12 |
| `BUILD_METADATA` | resolved using a validated `compile_commands.json` record | that include resolved through `-Iinclude` from the translation unit that compiles this file |
| `UNIQUE_LEXICAL` | exactly one candidate matched lexically; not compiler-proven | one repository file is named `buf.h`; one external definition of `atlas_buf_free` exists |
| `AMBIGUOUS` | more than one candidate matched; the set is recorded | two files named `config.h`; two external definitions of `init` |
| `UNRESOLVED` | no candidate matched, with a typed reason | `#include <stdio.h>`; a call to a libc function |
| `CONDITIONAL` | the fact was found inside `#if`/`#ifdef` that Atlas did not evaluate | a declaration under `#ifdef _WIN32` |
| `MODEL_PROPOSAL` | a model asserted it | **never written in A3** — see below |
| `UNKNOWN` | Atlas has no basis at all | a parse that failed before reaching this point |

`atlas_code_resolution_writable_in_a3()` refuses `MODEL_PROPOSAL`, exactly as
`atlas_provenance_writable_in_a2()` refuses `USER_APPROVED_DECISION`. The class
exists in the vocabulary and in the schema so that a later phase can start
producing it deliberately, and A3 cannot produce it by accident.

### The required distinctions, stated as rules

1. **A textual `#include` is a source fact.** That the bytes `#include "x.h"`
   appear at line N is `SOURCE_EXACT` and always recorded, whatever happens next.
2. **Resolving that include to a repository file is a separate fact** with its
   own class. The edge carries both: the spelling, always; the target, only when
   something resolved it.
3. **An identifier followed by `(` inside a function body is a call
   *candidate*.** It is never `SOURCE_EXACT` as a call edge. The occurrence's
   existence is exact; the callee is not.
4. **A unique lexical candidate is still not compiler-proven.** `UNIQUE_LEXICAL`
   is the strongest class a name match can earn.
5. **Function pointers, macros and conditional compilation never become exact
   edges.** An indirect call produces `UNRESOLVED` with reason
   `indirect_or_unknown`; a name that matches both a macro and a function
   produces `AMBIGUOUS` with both candidates and their kinds.
6. **Several possible definitions stay ambiguous**, with the candidate set in
   `code_candidates` and `candidate_count` on the edge.
7. **Missing build configuration is unknown, not false.** With no compile
   database, resolution still happens lexically and says so; it never silently
   claims `BUILD_METADATA`.
8. **Impact results are candidates supported by graph paths**, never a claim that
   a build will break or that any code will run.

### The evidence table is untouched

A3 writes **nothing** to `evidence`. `atlas_db_evidence_insert` still refuses
everything but `SOURCE` and `GIT`, and the A0 rule that a reason request returns
`UNKNOWN` is unchanged.

Structural facts are a different kind of thing and live in their own tables with
their own `resolution` and `provenance` columns — the same separation A2 made for
model records. Widening `evidence` to carry "one file lexically appears to call a
function in another" would make "how does Atlas know this?" and "what did a
lexical scan guess?" the same question.

The graph's `provenance` column is deliberately narrower than its resolution
class: `SOURCE` (read from repository bytes), `BUILD_METADATA` (read from a
validated compile-database record), `INFERENCE` (Atlas derived it from other
graph rows, and the derivation is on the row), `UNKNOWN`.

## Language scope

Semantic extraction runs for C-family files only, decided by extension:

| extension | language | treated as |
| --- | --- | --- |
| `.c` | `c` | implementation source |
| `.h` | `c` | header |
| `.inc`, `.def` | `c-fragment` | an included fragment |

Every other file stays indexed at the A0/A1 file and git level and receives no C
semantics at all. A `.py` file has a language, a hash and a history; it has no
symbols, and Atlas does not invent any.

A `.inc` or `.def` fragment is parsed with exactly the same rules as any other
supported file and recorded with the language `c-fragment`, so a consumer can
tell "this is a translation unit" from "this is a piece of one". It is not given
special treatment beyond that label: a fragment out of context often produces a
partial parse, and reporting that partiality is more useful than pretending the
fragment can be understood on its own.

C++ is **out of scope**. A `.cpp` or `.hpp` file is not parsed. Getting C++
wrong quietly is far worse than not answering.

## What the indexer recognises

A single-pass byte lexer, then a small state machine over its tokens. It is
first-party, has no dependency of any kind, and never runs a process.

Recognised:

- `#include "..."` and `#include <...>`, with the spelling kept verbatim
- `#define NAME` and `#define NAME(...)`, as distinct kinds (`macro`,
  `macro_function`)
- function **definitions** (a declarator followed by `{`) and **declarations**
  (followed by `;`)
- `typedef` declarations, including the `typedef struct { ... } NAME;` form
- `struct`, `union` and `enum` definitions and their tags, plus enum constants
- file-scope object declarations, where the declarator is confidently
  recognisable
- `static` linkage (`internal`), `extern`/default (`external`), macros (`none`),
  and `unknown` where the run could not be classified
- lexical call candidates inside function bodies, with the enclosing symbol
- byte offset, line and column for every symbol and occurrence

Correctly **not** treated as code:

- `/* */` and `//` comments, including a `//` comment continued by a backslash
- string literals, character literals and their escapes
- escaped newlines anywhere, including inside a directive or a token
- preprocessor replacement text: a `#define` body is skipped, so
  `#define FOO bar(` does not open a paren and `#define X "unterminated` does not
  swallow the rest of the file

GNU extensions (`__attribute__((...))`, `__asm__(...)`, `__extension__`,
`__inline__`, `__restrict`, `_Noreturn`, `__typeof__`) are **tolerated**: they
are skipped where they can be, and where a construct is not understood the run is
recorded as `unknown` with the file marked `partial` rather than guessed at.

### Conditional compilation

Atlas does not evaluate the preprocessor. It tracks conditional nesting depth
and marks anything found at depth > 0 with resolution `CONDITIONAL`.

One exception, because without it every header would be entirely conditional: a
**include guard** — a leading `#ifndef IDENT` immediately followed by
`#define IDENT`, whose `#endif` closes the file — does not count as a conditional
level. `#pragma once` is recognised and costs nothing. Everything else,
including `#if 1`, counts.

### Hard limits, all reported

Every one is a macro in `include/atlas/limits.h`, and reaching one produces a
`code_index_errors` row and a `truncated` flag on the file, never a silent stop.

| bound | macro |
| --- | --- |
| file bytes parsed | `ATLAS_CODE_MAX_FILE_BYTES` |
| one token's bytes | `ATLAS_CODE_MAX_TOKEN_BYTES` |
| brace / paren nesting | `ATLAS_CODE_MAX_NESTING_DEPTH` |
| symbols per file | `ATLAS_CODE_MAX_SYMBOLS_PER_FILE` |
| relations per file | `ATLAS_CODE_MAX_RELATIONS_PER_FILE` |
| occurrences per file | `ATLAS_CODE_MAX_OCCURRENCES_PER_FILE` |
| includes per file | `ATLAS_CODE_MAX_INCLUDES_PER_FILE` |
| symbol name bytes | `ATLAS_CODE_MAX_NAME_BYTES` |
| include resolution depth | `ATLAS_CODE_MAX_INCLUDE_DEPTH` |
| traversal depth / nodes | `ATLAS_CODE_MAX_TRAVERSAL_DEPTH`, `ATLAS_CODE_MAX_TRAVERSAL_NODES` |
| candidates kept per edge | `ATLAS_CODE_MAX_CANDIDATES` |
| compile database bytes / entries | `ATLAS_CODE_MAX_COMPILE_DB_BYTES`, `ATLAS_CODE_MAX_COMPILE_UNITS` |

A file that is not valid UTF-8 is still parsed — C source is bytes — but a file
containing a NUL byte in its first 8000 bytes is treated as binary and skipped
with a reason.

## Compile databases

`compile_commands.json` is **data**. It is read, bounded, and parsed through
`atlas/jsonread.h`, the one yyjson facade. Nothing in it is ever executed, passed
to a shell, or used to construct a process.

Recognised per entry:

| member | treatment |
| --- | --- |
| `file` | the translation unit's source path; normalised and checked against the repository |
| `directory` | the base for relative paths; normalised and checked |
| `output` | recorded as identity, so two configurations of one file stay distinct |
| `arguments` | walked with an **allowlist**; everything not on it is counted and dropped |
| `command` | **never parsed and never executed.** Presence and a SHA-256 of the string are recorded; the string itself is not stored |

The `arguments` allowlist is exactly:

```
-I <dir> / -I<dir>          include directory
-iquote <dir>               quote-only include directory
-isystem <dir>              system include directory
-idirafter <dir>            trailing include directory
-D NAME[=V] / -DNAME[=V]    define
-U NAME  / -UNAME           undefine
-std=<name>                 language standard
-x <lang>                   explicit language
-o <path>                   output identity
```

Everything else — including `-include`, `-fplugin=`, `@response-file`, and any
argument beginning with `@` — is counted in `dropped_args` and otherwise ignored.
Response files are **not** opened; a compiler plugin argument is **not** acted on.

Every path is normalised (lexically: `.` removed, `..` folded, no symlink is
followed) and then classified:

- inside the registered repository root → stored as a repository-relative
  include directory usable for resolution
- outside it → stored with `external = 1`, usable only as a statement that the
  build looks there. **It does not authorise Atlas to read anything.** No code
  path opens a file from an external include directory.

Duplicate entries for one `(file, output)` pair collapse to one unit; a second
entry with a different `output` is a second configuration and both are kept. An
entry whose `file` resolves outside the repository is dropped with a reason.

**With no compile database, indexing still works.** Include resolution falls back
to the includer's directory and to a repository-wide basename match, and reports
`UNIQUE_LEXICAL` or `AMBIGUOUS` instead of `BUILD_METADATA`. A missing compile
database is a stated fact (`compile_db_present: false`), never an error.

Atlas does **not** parse CMake, Make or any other build syntax in A3.
Translation-unit facts come only from validated compile-database records.

## The graph

### Nodes

| node | table | keyed by |
| --- | --- | --- |
| repository file | `code_files` | `(repo_id, path_raw)` |
| translation unit | `code_units` | `(repo_id, source_path_raw, output)` |
| symbol | `code_symbols` | `(code_file_id, kind, name, byte_offset)` |
| occurrence | `code_occurrences` | `(code_file_id, byte_offset)` |
| compile configuration | `code_units` + `code_unit_includes` / `code_unit_defines` | — |

A **symbol is a site**, not a global entity. Two files each defining `static void
helper(void)` produce two rows, and nothing merges them. Cross-file identity is
expressed by edges with a resolution class, which is the only honest way a
lexical indexer can express it.

### File roles

A file may hold several. Each role row records its `basis`, so path naming is
never presented as proof:

| role | typical basis |
| --- | --- |
| `implementation` | extension `.c` |
| `public_header` | `.h` under an `include/` directory, or named by a compile unit's include dirs |
| `private_header` | `.h` beside implementation sources |
| `test` | path under `test`/`tests`, or a basename starting `test_` |
| `build_metadata` | `CMakeLists.txt`, `Makefile`, `*.cmake`, `meson.build`, `compile_commands.json` |
| `documentation` | `.md`, `.rst`, `.txt`, `LICENSE`, `README` |
| `vendored` | path under `third_party`/`vendor`/`external`/`node_modules` |
| `generated` | a generated-file marker in the first 4 KiB, or an `output` path in the compile database |
| `unknown` | nothing matched |

`basis` is one of `extension`, `path_naming`, `content_marker`, `build_metadata`,
`include_graph`, `none`. A role whose only basis is `path_naming` or `extension`
is reported with that basis and with resolution `SOURCE_EXACT` **about the path**
— the path really does say that — and never with a claim that the role is proven.
The distinction is in the output: a consumer sees `role: test, basis:
path_naming`, which is exactly as much as Atlas knows.

### Relations

One table, `code_relations`, with a source and a destination that each carry a
kind. That is deliberate: two indexes (by source, by destination) make inbound and
outbound traversal the same query shape, which is what keeps reverse dependency
and impact bounded and fast.

| kind | source → destination |
| --- | --- |
| `file_includes_file` | file → file (or unresolved spelling) |
| `file_defines_symbol` | file → symbol |
| `file_declares_symbol` | file → symbol |
| `unit_compiles_file` | unit → file |
| `unit_uses_header` | unit → file |
| `symbol_contains_occurrence` | symbol → occurrence — **recognised, not materialised**; see below |
| `symbol_calls_symbol` | symbol → symbol (or unresolved name) |
| `symbol_declared_by` | symbol → symbol |
| `symbol_defined_by` | symbol → symbol |
| `file_depends_on_file` | file → file |

`file_depends_on_file` is derived and says so: provenance `INFERENCE`, with a
`detail` naming the contributing edge kind. Its reverse — "depended on by" — is
the same rows read from the destination index, not a second set of rows.

**`symbol_contains_occurrence` is a fact Atlas stores once, and this table is
not where.** The containment is `code_occurrences.enclosing_id` — the column the
extractor writes it to, with a foreign key and an index, and the column every
consumer already reads. Materialising an edge as well stored the same thing
twice: on the acceptance fixture that was 235 520 relation rows, 38 % of the
whole table, each with five index insertions, and not one query in Atlas read a
single one of them. Caller-to-callee traversal does not need them either,
because `symbol_calls_symbol` already carries the enclosing symbol as its
source.

The kind stays in the vocabulary and in the schema's `CHECK`. It is a legitimate
edge for a producer with no occurrence table of its own — a future importer,
say — and removing it would turn that from an insert into a migration. What was
removed is the duplication, not the fact and not the ability to record it.

Every relation carries: `resolution`, `provenance`, `line`, `col`,
`candidate_count`, a typed `detail` when unresolved or ambiguous, the
`generation` it was established in, and `owner_file_id` — the file whose parse
produced it, which is what makes replacement per file rather than per repository.

### Linkage is respected

- a `static` definition has `linkage = internal` and is a candidate **only for
  occurrences in the same file**
- an `extern` or default-linkage definition is a candidate repository-wide
- two external definitions of one name are a **conflict**: every occurrence that
  names it becomes `AMBIGUOUS`, with both recorded
- a declaration links to a definition only when the evidence permits: same name,
  compatible kind, and either same-file internal linkage or unique external
  linkage
- a macro and a function with the same name are two candidates of different
  kinds, never one symbol

### Which analyzer produced the graph

Provenance says which source a fact was read from. Resolution says how firmly it
was established. Neither says which *algorithm* produced it, and that is a third
question with a failure mode of its own:

1. Atlas indexes a repository.
2. Atlas is upgraded, and the upgrade corrects the lexer or the resolver.
3. Not one byte of the repository or the compile database changes.
4. Every generation still lines up, so the pass finds nothing to do.

The stored graph is now wrong in exactly the way the upgrade fixed, and it
reports itself current. Nothing else Atlas records can see this: every other
staleness signal is about the *inputs*.

Two Atlas-owned constants close it — `ATLAS_CODE_ANALYZER_ID`, currently
`atlas-c-lexical`, and `ATLAS_CODE_ANALYZER_VERSION`, an integer epoch. They are
compiled into the binary: no repository, no compile database and no model can
influence either, which is what makes them safe to report to a model at all.

They are stored **normalized**. `code_analyzers` interns the pair, and
`code_index_state.analyzer_id` references one row per repository. The
alternative — a name and a version on every relation — would put two more
columns on six hundred thousand rows to say the same thing six hundred thousand
times, and a structural pass has exactly one producer.

It is a *reference* rather than the values themselves so the per-fact case stays
reachable without a redesign. A future producer that mixes sources — an optional
SCIP index for the files it covers and this lexical analyzer for the rest — adds
`analyzer_id INTEGER REFERENCES code_analyzers(id)` to `code_relations`. That is
one integer per row rather than two strings, joined the same way, against a
vocabulary that is already interned. **A3 implements none of that**: no SCIP, no
Clang, no LSP, no plugin loader, no external analyzer, no new dependency.

The rules:

- **A mismatch makes the structural graph stale**, with the fixed reason *"the
  structural index was produced by a different analyzer version"*.
- **The next ordinary sync repairs it.** The pass notices by itself and rebuilds;
  nobody has to pass `--rebuild` and nothing waits for a human.
- **The rebuild is derived data only.** `atlas_db_code_clear_repo` names
  `code_files` and `code_units`, so the cascade reaches symbols, occurrences,
  relations, candidates and roles and reaches nothing else. Sessions, change
  sets, recorded reasons, decisions, evidence, commits and the file index come
  through untouched, and `tests/test_code_analyzer.c` asserts it row by row.
- **The identity is reported through `code status` and `code.status`**, in
  human and JSON output and in the MCP tool. It is **not** added to the automatic
  context envelope: that envelope carries what a reader needs in order to decide
  whether to ask, and the answer to "which analyzer" belongs with the answer.
- **Bump the version whenever a pass would produce different facts from
  identical bytes** — a lexer fix, a resolution rule change, a different set of
  materialised edges. Not for a refactor that cannot change an output.

The analyzer row is immutable once written. It is a historical fact about what
built something, not a setting, so an upgrade adds a row rather than rewriting
the record of what produced the previous graph.

## Incremental behaviour

Structural indexing is a stage of the A1 reconciliation pass, not a second
pipeline.

1. **A1 establishes content identity.** Nothing in A3 stats or hashes a file.
2. **Selection is a hash comparison, not a "was it hashed" flag.** The candidate
   set is exactly:

   ```sql
   files f LEFT JOIN code_files c USING (repo_id, path_raw)
   WHERE f.deleted = 0 AND f.content_hash IS NOT NULL AND <extension is supported>
     AND (c.id IS NULL OR c.content_hash IS NULL OR c.content_hash <> f.content_hash)
   ORDER BY f.path_raw
   ```

   This is what makes the acceptance criteria fall out rather than being aimed
   at: an unchanged pass parses zero files *even when it was a full
   content-verifying pass*, because a full pass rehashes bytes and finds the same
   hash. A one-file edit selects one file.
3. **Workers parse.** A parse job reads bytes through `atlas_path_open_nofollow`,
   lexes them, and fills its own slot. It touches no database handle and creates
   no process, exactly like the hash jobs beside it.
4. **The writer applies**, per file, in bounded batches: delete every row owned by
   that file, insert the new ones. No parse and no file read happens inside a
   transaction.
5. **Resolution runs after application**, deterministically, and over a scope
   bounded by the change rather than by the repository. This is the part that
   was got wrong first and is worth stating precisely, because "re-attempt every
   unresolved edge" is a correct answer that costs the same on a repository
   nobody touched as on a rebuild.

   `atlas_code_resolve_scope` is the whole rule. Each field is in it because
   leaving it out would give a *wrong* answer, not merely a slower one:

   | field | what it admits | why nothing else can matter |
   | --- | --- | --- |
   | `files` | the `code_files` ids this pass parsed | their edges were rewritten unresolved and must be settled, whatever they name |
   | `names` | externally linked definitions that appeared or vanished | a call resolves by name and nothing else, so only an edge naming one of these can answer differently — found by an indexed seek on `dst_name`, never by reparsing |
   | `file_set_changed` | a path was added or removed | include resolution reads the including file's directory, the build's include directories, and the set of paths. An edit to an existing file changes none of them, so it cannot make a previously unresolvable include resolvable |
   | `full` | a rebuild, an overflowed scope, or `resolve_settled` false | the scope is not known to be a description of the change, so the repository is the honest answer |

   **Internal linkage is excluded from `names` on purpose**, and it is the C
   rule rather than an optimisation: a `static` definition is a candidate only
   inside its own file, so changing one cannot change how an edge in any other
   file resolves. The edges it *can* change belong to the file just reparsed and
   are swept by id. Including internal names would sweep every edge in the
   repository naming `helper` — thousands of them in a real C project — to reach
   the handful that could differ.

   Beyond `ATLAS_CODE_MAX_RESOLVE_NAMES` touched names, or past the parsed-file
   bound, the scope stops being a description and resolution falls back to the
   whole repository — **and says so**, in `code_index_state.detail` and in a
   `code_index_errors` row. It is still resolution, never a reparse.

   **Invalidation is targeted, not scanned.** Reparsing a file deletes and
   recreates its symbol rows with new ids, so the edges elsewhere that resolved
   to the old ones must go back to unresolved. The writer does that *before* the
   delete, seeking from the ids that are about to disappear
   (`atlas_db_code_relations_unsettle_for_file`); afterwards only a left join
   over every relation in the repository could find the damage, and that join
   costs the same whether it finds one row or none. The repository-wide dangling
   sweep stays for the full path, where a scan is proportionate.

6. **A pass that provably wrote nothing skips resolution entirely.** The durable
   `code_index_state.resolve_settled` flag records that every edge has been
   through resolution since the last thing that could change an answer. It is
   cleared at the *start* of a pass and set only at the end, so a pass that died
   half way through resolution leaves it false and the next pass sweeps the
   repository rather than trusting a flag the dead pass never earned. That
   ordering is the whole reason the flag is durable rather than inferred from
   "did the last pass complete".
7. **Deletion and rename are explicit writer-path work**, not a foreign key.
   `files` rows are tombstoned rather than removed, so `ON DELETE CASCADE` from
   `files` would never fire. The pass deletes `code_files` rows whose path is
   gone or tombstoned, cascading the symbols, occurrences and relations they own,
   and then re-resolves the edges that pointed at them. A rename is a delete plus
   an add in `files`, and therefore a delete plus a parse here.
8. **A failed parse is recorded and degrades the repository**: the file's row
   gets `parse_status = failed`, an error row is written, and
   `code_index_state.degraded` is set with a reason. `code_index_current` is then
   false until a pass parses it successfully.
9. **A generation is published only at the end**, after every application and the
   resolution pass. A crash mid-pass leaves `last_complete_generation` where it
   was, and the hash comparison in step 2 redoes exactly what was missing.

`code_index_current` is true only when a structural generation has completed, it
equals the repository's own `last_complete_generation`, the structural index is
not degraded, and the repository's `index_current` is itself true. There is no
state in which Atlas reports the structural index as current alongside a known
parse failure.

**Hooks never wait for parsing.** No hook path blocks on structural work; a hook
asks for a pass and returns, as it already does.

### Where structural indexing runs

Deliberately in exactly one place: `atlas_reconcile_run`. So:

- the daemon indexes structurally on every pass
- `atlas sync` indexes structurally, daemon or offline
- `atlas code sync` is `atlas sync` with a structural rebuild flag
- **`atlas scan` does not.** It is the A0 command and stays the A0 command; after
  it, the structural index is stale for whatever it changed, and the next
  reconciliation pass picks that up from the content hashes. There is a test for
  it, so the behaviour is chosen rather than accidental.

## Impact analysis

`code.impact` is inbound traversal over `file_depends_on_file` and
`symbol_calls_symbol`, breadth-first from a file or a symbol, with:

- caller-selected depth, clamped to `ATLAS_CODE_MAX_TRAVERSAL_DEPTH`
- cycle detection by a visited set keyed on `(node kind, node id)`
- deterministic ordering: depth, then node kind, then path bytes, then id
- node and edge caps, with `truncated` and the reason when either is reached
- a **path** for every result: the chain of edge kinds that reached it
- separate sections for `exact`, `unique_lexical`, `ambiguous` and `unresolved`,
  because merging them would be exactly the conflation A3 exists to prevent

An impact result says: *there is a graph path from the thing you named to this
node, and here it is.* It does not say the node will break, will be rebuilt, or
will execute. That sentence is in the tool description and in the result.

No embeddings. No vector search. No LLM call.

## The trust boundary is unchanged, and one thing is added

Symbol names, include spellings and file paths are **repository text**. Whoever
can commit chooses them, and `ignore previous instructions` is a legal C
identifier prefix in a comment and a legal directory name.

So:

- **Automatic context gains typed counters only**: `code_index_current`,
  `code_generation`, `code_symbols`, `code_relations`, `code_ambiguous`,
  `code_unresolved`. Integers and booleans, from Atlas' own counting. No symbol
  name, no path, no include spelling, no summary. The envelope's fixed ASCII
  allowlist is unchanged and the renderer still checks its own output against it.
- **Explicit MCP results may carry names and paths**, bounded, safe-encoded, and
  labelled `untrusted_data: true` with the existing notice. That is the same
  channel and the same rules A2 established; A3 adds no new one.
- Structural facts cannot forge protocol framing: names are bounded to
  `ATLAS_CODE_MAX_NAME_BYTES`, encoded with `atlas_text_encode_safe`, and emitted
  through the first-party streaming writer like everything else.

## Explicit non-claims

Stated plainly, because the value of the index is proportional to how well its
limits are known.

1. **Atlas does not run the preprocessor.** Macro expansion is not performed. A
   call written through a macro is a call to the macro, if the macro is even in
   scope, and Atlas says `AMBIGUOUS` or `UNRESOLVED` rather than following it.
2. **Atlas does not do name lookup.** A call candidate is matched by name and
   linkage, not by scope. A local variable shadowing a function is invisible to
   it; the edge is reported as a candidate, which is what the class means.
3. **Atlas does not know a call executes.** `symbol_calls_symbol` means the
   bytes of one function's body mention another name followed by `(`. Dead code,
   a branch never taken, and a call under `#if 0` all look the same. (`#if 0`
   contents are marked `CONDITIONAL`, which is the most Atlas can say.)
4. **Atlas does not resolve function pointers.** `fp(x)` is `UNRESOLVED` with
   reason `indirect_or_unknown`, and `(*fp)(x)` produces no call candidate at
   all.
5. **Atlas does not implement C++, and does not guess at it.**
6. **Atlas does not verify a build.** An impact candidate is not a prediction
   that anything breaks.
7. **Atlas does not read outside the repository.** External include directories
   from a compile database are recorded as metadata and never opened, so a
   `<stdio.h>` include is `UNRESOLVED` and stays that way.
8. **Atlas does not merge same-named symbols.** Two definitions are two rows and
   an ambiguity, permanently, until the source stops having two.
9. **Atlas does not infer a reason.** A3 changes nothing about that: asked why a
   file exists or why a symbol was added, the answer is still `UNKNOWN` unless
   somebody recorded one through the A2 tools.
10. **A structural fact is not evidence.** Nothing here reaches the `evidence`
    table, and the A0 restriction stands.

## What A4 inherits

- natural-language file and subsystem summaries, built *from* these facts, with
  human approval — the approval workflow A2 deliberately did not fake
- decision documents and ADRs parsed from the repository, linked to the symbols
  and files A3 now knows about
- the `DECISION` and `USER_STATEMENT` evidence kinds, and the deliberate
  migration that lifts `atlas_db_evidence_insert`'s restriction to exactly those
- optional `clangd` integration for the cases where lexical resolution is
  genuinely not enough, behind the same argv allowlist git gets
- real DNA indexing, which A3 measures against a synthetic tree of the same shape
  and deliberately does not attempt
