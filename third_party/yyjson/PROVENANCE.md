# yyjson — vendored third-party source

Atlas parses untrusted JSON on its IPC boundary. That parser is not first-party
code: a hand-written generic JSON parser is exactly the kind of component whose
edge cases (deep nesting, number overflow, invalid UTF-8, truncated input) are
discovered in production rather than in review. yyjson is vendored instead.

## Exact upstream identity

| field | value |
| --- | --- |
| project | yyjson |
| upstream | <https://github.com/ibireme/yyjson> |
| release tag | `0.12.0` |
| release page | <https://github.com/ibireme/yyjson/releases/tag/0.12.0> |
| **annotated tag object** SHA-1 | `7871d321ff4cd8068c1f777c97975dc2fb640ab3` |
| **peeled commit** SHA-1 | `8b4a38dc994a110abaec8a400615567bd996105f` |
| source archive | <https://github.com/ibireme/yyjson/archive/refs/tags/0.12.0.tar.gz> |
| archive SHA-256 | `b16246f617b2a136c78d73e5e2647c6f1de1313e46678062985bdcf1f40bb75d` |
| licence | MIT (see `LICENSE`) |

### Why two SHA-1s, and which one to compare against

`0.12.0` is an **annotated** tag, so `refs/tags/0.12.0` names a tag *object*, not
a commit. Recording only one of the two invites a false mismatch: somebody
checking with `git rev-parse refs/tags/0.12.0` sees the tag object, while
somebody reading the commit list on the release page, or running `git rev-parse
refs/tags/0.12.0^{commit}`, sees the peeled commit. Both are correct identities
for the same release, and both are recorded here so neither reading looks wrong.

```
$ git rev-parse refs/tags/0.12.0            # 7871d321... (the tag object)
$ git rev-parse refs/tags/0.12.0^{commit}   # 8b4a38dc... (what it points at)
```

The archive is generated from the peeled commit, so the archive digest and the
per-file digests below correspond to `8b4a38dc...`.

## Vendored files and their SHA-256

Only the two source files and the licence are vendored. The upstream build
system, tests, documentation and CI configuration are deliberately not carried.

| file | upstream path | SHA-256 |
| --- | --- | --- |
| `yyjson.c` | `src/yyjson.c` | `ac2e9bbb2e2d9149d90878d40506a1d624fa0b33c979a11b61075c54782c6d6a` |
| `yyjson.h` | `src/yyjson.h` | `175867c5493a5df648cec566717fa1c29aa2f6096f5f0cf1efad0b65e1f6d7b3` |
| `LICENSE` | `LICENSE` | `45e384d3d52c73cba3a64d6e6c25d47cd738cd8a55c30629e3201046eda62947` |

The files are byte-for-byte upstream. Nothing was reconstructed, reformatted or
patched. `scripts/verify_third_party.sh` re-checks these digests and is run by
the test suite, so a silent local edit fails the build rather than shipping.

Re-verified against a freshly downloaded `0.12.0.tar.gz` during the A1
correctness pass: the archive digest and all three per-file digests matched, so
no vendored byte was changed.

To reproduce the check yourself, without trusting this file:

```sh
curl -sSL -o yyjson-0.12.0.tar.gz \
    https://github.com/ibireme/yyjson/archive/refs/tags/0.12.0.tar.gz
sha256sum yyjson-0.12.0.tar.gz          # must equal the archive digest above
tar xzf yyjson-0.12.0.tar.gz
sha256sum yyjson-0.12.0/src/yyjson.c yyjson-0.12.0/src/yyjson.h yyjson-0.12.0/LICENSE
diff -u yyjson-0.12.0/src/yyjson.c third_party/yyjson/yyjson.c   # must be empty
```

## How Atlas builds it

- Compiled as its own static library target, `atlas_yyjson`.
- `ATLAS_WERROR` is **not** applied to it: warnings are errors in first-party code
  only, and suppressing an upstream warning by editing upstream source would break
  the digest guarantee above.
- No `FetchContent`, no download step, no network access at build time. The source
  is in the repository.
- Third-party lines of code are counted separately from first-party lines in the
  phase report.

## How Atlas uses it

Reading only, and only in `src/ipc/proto_parse.c`:

- `yyjson_read_opts` with `YYJSON_READ_STOP_WHEN_DONE` and a fixed allocator-free
  path over a caller-owned buffer whose length is already bounded by the frame
  reader (`ATLAS_IPC_MAX_REQUEST_BYTES`).
- Document depth is bounded by `ATLAS_IPC_MAX_JSON_DEPTH` **after** parsing, by an
  explicit iterative depth walk; yyjson itself imposes no depth ceiling.
- Malformed input returns a structured error. It never aborts the daemon.

Atlas does not use yyjson to *write* JSON. Output continues to go through the
first-party streaming writer in `src/output/json.c`, so the response contract and
its escaping rules are unchanged from A0.
