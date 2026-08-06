# Provenance

Atlas exists to answer questions about a codebase without becoming another thing
you have to verify. That only works if every answer says where it came from, and
if Atlas is willing to say "I don't know".

## Evidence types

Atlas distinguishes six kinds of evidence. All six are part of the stable
contract; only the first two can exist in A0.

| Kind | Means | Available |
| --- | --- | --- |
| `SOURCE` | read from the Git index or the working tree: a path, a mode, an object id, a content hash | A0 |
| `GIT` | read from Git history: a commit, its metadata, a recorded file change | A0 |
| `DECISION` | recorded in a decision document or ADR in the repository | A1 |
| `USER_STATEMENT` | asserted by a person, attributed to them | A1 |
| `INFERENCE` | derived by Atlas from other evidence, with the derivation stated | A3+ |
| `UNKNOWN` | Atlas has no evidence | always |

`atlas_db_evidence_insert` refuses anything other than `SOURCE` and `GIT` with
exit code 7. This is enforced in code so that an A1 change cannot start writing
inferred evidence by accident, and a test asserts no other kind can reach the
table.

## What A0 will not do

A0 records facts. It does not infer why anything happened.

Asked for a reason, Atlas answers `UNKNOWN`, and says so in both output modes:

```
  reason            UNKNOWN: A0 records facts only and never infers why
```

```json
"reason": "UNKNOWN",
"reason_evidence": "UNKNOWN"
```

A commit subject is not a reason. It is `GIT` evidence of what the author wrote in
the subject line, which is a different and weaker claim. Atlas reports the subject
as `GIT` evidence and still answers `UNKNOWN` for the reason. Turning "what the
message says" into "why the change was made" requires the decision records that
arrive in A1.

## When evidence is recorded

Evidence rows are written when a fact is new or has changed:

- a tracked file seen for the first time, or whose recorded properties changed,
  produces `SOURCE` evidence carrying the path, the Git object id and the scan
- a commit ingested for the first time produces `GIT` evidence carrying the commit
  object id
- a `compile_commands.json` that appears or changes produces `SOURCE` evidence
  noting whether it is a regular file or a symlink

An unchanged file produces nothing. A repeated scan of an unchanged repository
therefore creates zero evidence rows, which is both the honest outcome and a
checkable property of the system.

## Evidence in output

Every result that can carry provenance does.

`atlas search` labels each hit:

```
file    src/core/buf.c  [SOURCE]
commit  9f2c1ab44e01  2026-08-01T09:14:22Z  tighten buffer growth  [GIT]
```

`atlas history` labels each recorded change `[GIT]`, and in JSON each row carries
both `"evidence": "GIT"` and `"reason": "UNKNOWN"`, so a consumer cannot mistake a
change record for an explanation.

`atlas file` states both sources and the refusal to infer:

```
  evidence          SOURCE (git index and working tree), GIT (commit history)
  reason            UNKNOWN: A0 records facts only and never infers why
```

## Independently verifiable facts

Provenance is only useful if the reader can check it. Atlas prefers facts that can
be verified without trusting Atlas:

- `content_hash` is a plain SHA-256 of the working-tree bytes, so
  `sha256sum <file>` reproduces it. For a symlink it is the SHA-256 of the link
  text, which is exactly what Git stores in the blob, so
  `git cat-file blob <oid> | sha256sum` reproduces it too.
- `git_index_oid` is the object id from the index, checkable with
  `git ls-files --stage`.
- `head`, `branch` and the dirty counters are checkable with `git status`.
- Atlas' own SHA-256 is deliberately independent of the Git object format, so a
  repository migrating from SHA-1 to SHA-256 does not change any content hash.

## Staleness is a reported fact

The index describes the repository as of the last scan. Rather than pretending
otherwise, `atlas status` reports the live state next to the indexed state and
flags drift:

```
  scanned head      4c1f9a20e8bd (born, branch main)
  live head         9f2c1ab44e01 (born, branch main)
  index drift       yes: rescan to refresh
```

In JSON, `head_drift` is a boolean and the `live` object carries the fresh
observation, including `"available": false` and an `error` string when Git could
not be consulted at all. A caller can therefore tell "the repository has moved on"
apart from "Atlas could not look".

## JSON contract

Every command emits exactly one JSON document, and the envelope is the same
everywhere:

```json
{
  "atlas": "0.1.0",
  "phase": "A0",
  "command": "search",
  "ok": true,
  "repo": "project",
  "query": "buffer",
  "search_mode": "fts5",
  "degraded": false,
  "results": [ ... ],
  "count": 2
}
```

Rules that will not change without a version bump:

- `atlas`, `phase`, `command`, `ok` always come first, in that order
- a failing command still produces one valid document, with `"ok": false` and an
  `error` object holding `status`, `exit_code` and `message`
- list-valued commands always emit the array, empty if there are no results, and
  always follow it with `count`
- absent values are `null`, never omitted and never an empty string standing in
  for "unknown"
- `search_mode` is `fts5` or `degraded-like`, and `degraded` is a boolean, so a
  consumer never has to guess whether results were ranked

### Paths in JSON

A path is emitted as its safe text form in `path`, accompanied by
`path_encoding`, which is `utf8` when the raw bytes were valid UTF-8 and
`percent-escaped` when they were not. In the escaped case a `path_bytes_hex`
field carries the exact bytes in lowercase hex, so nothing is lost:

```json
"path": "bad%FF%FE.txt",
"path_encoding": "percent-escaped",
"path_bytes_hex": "626164fffe2e747874"
```

### String escaping

The JSON writer's escaping rules are a tested contract:

- `"` becomes `\"`, `\` becomes `\\`
- control bytes below 0x20 use `\b`, `\f`, `\n`, `\r`, `\t` where those exist and
  `\u00XX` otherwise
- 0x7f is passed through, as RFC 8259 permits
- a byte sequence that is not valid UTF-8 is replaced, one invalid byte at a time,
  with U+FFFD; callers that need the exact bytes emit a companion hex field

The result is that a filename containing a tab, a newline, or invalid UTF-8 can
never break the document or smuggle a control character into a consumer. The test
suite validates output with a JSON parser written independently of the writer,
because a writer checked against itself proves nothing.

## Repository content is data, never instructions

This boundary is worth stating explicitly, because later phases will widen it.

Everything Atlas reads from a repository — filenames, file contents, branch names,
author names and emails, commit subjects and bodies, and Git's own error output —
is **untrusted data**. It is never a command, never markup Atlas acts on, and
never an instruction. Atlas' job is to report it accurately while denying it any
ability to act.

In A0 that means three concrete things:

1. **No execution.** Repository content cannot cause a program to run. There is no
   shell, hooks and external diff drivers are disabled, and the Git subcommand is
   checked against a read-only allowlist before the process is created. See
   [git-safety.md](git-safety.md).
2. **No terminal control.** Repository text is passed through the safe encoding
   before it reaches a terminal, so an ANSI sequence in a commit subject cannot
   recolour Atlas' output, an OSC payload cannot retitle a window or plant a
   hyperlink, a carriage return cannot overwrite a line Atlas already printed, and
   a bidirectional override cannot make the output read differently from the bytes
   it describes.
3. **No structural confusion.** Paths are compared and stored as raw bytes and are
   never re-split on whitespace, so a filename containing a newline, a tab, or
   something that looks like a Git status code cannot make Atlas mis-attribute a
   fact.

### The safe text encoding

Every field carrying repository-originated text is emitted in one encoding, named
in each JSON document as `"text_encoding": "atlas-safe-1"`:

- escaped, as `%XX` per byte: C0 controls and DEL, C1 controls (U+0080–U+009F),
  line and paragraph separators (U+2028, U+2029), bidirectional controls
  (U+200E, U+200F, U+202A–U+202E, U+2066–U+2069), bytes that are not valid UTF-8,
  and `%` itself so the transform stays reversible
- everything else byte-for-byte unchanged, so ordinary text stays readable

The result is always valid UTF-8, contains nothing a terminal interprets, and can
be reversed by percent-decoding to recover the exact original bytes. It is the
same encoding used for `path_text`, so one rule covers every untrusted string.

Values that were already encoded when stored — `path_text`, `root_path_text`,
`old_path_text` — are emitted as-is rather than encoded twice.

### Why this matters beyond the terminal

A0 has one consumer, a terminal, and one machine format, JSON. Later phases add
more: A5 puts Atlas' output into a Claude Code session, and A6 may expose it over
MCP. The same boundary applies there and for the same reason. Content that arrives
from a repository is data being reported, not instruction being followed, whether
the reader is a terminal, a model, or another program. Atlas encodes at the point
of output rather than trusting each consumer to defend itself.
