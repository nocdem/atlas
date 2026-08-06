# The Atlas plugin for Claude Code

Makes Atlas participate in a Claude Code session automatically. After a one-time
setup, nobody types an `atlas` command during ordinary work.

## What it installs

| Component | What it does |
| --- | --- |
| `hooks/hooks.json` | 15 lifecycle hooks that open a change session, record which tools touched which paths, correlate that against what the index actually observed, and close unexplained changes as `UNKNOWN` |
| `.mcp.json` | the `memory` MCP server — 10 tools for querying the index and recording reasons and decisions |
| `skills/atlas-memory/SKILL.md` | the instruction contract telling Claude when to query and when to record |
| `bin/atlas-hook`, `bin/atlas-mcp` | POSIX-sh launchers that locate the Atlas executable |

Everything runs against a local daemon over a `0600` Unix socket. Nothing here
reaches the network, and nothing here writes to your repository.

## Setup

Atlas prints the commands rather than running them:

```sh
atlas integrate claude print
```

The whole of it:

```sh
# 1. Record where Atlas is, so the plugin can find it after Claude caches it.
atlas integrate claude install --user

# 2. Keep the index current.
atlas service install --user
systemctl --user enable --now atlas

# 3. Install the plugin permanently, at user scope.
claude plugin marketplace add /path/to/integrations/claude
claude plugin install atlas@atlas-local --scope user

# 4. Check.
claude plugin list
claude plugin validate /path/to/integrations/claude
atlas integrate claude doctor
```

Step 1 exists because Claude copies an installed plugin into a cache directory
whose path changes on every update. Nothing inside the plugin can hold a stable
path to a binary outside it, so the launcher reads a small record from your own
config directory instead. If `atlas` is on your `PATH`, the launchers find it
without step 1 — the record is the fallback for a GUI-launched session that did
not inherit your shell's `PATH`.

## Removing it

```sh
claude plugin uninstall atlas@atlas-local
claude plugin marketplace remove atlas-local
atlas integrate claude uninstall --user   # removes one config file
systemctl --user disable --now atlas
```

**Your index survives all of this.** `uninstall` removes the integration record
and nothing else. To remove the data as well, delete the Atlas data directory
yourself — deliberately, because it is the thing that took time to build.

## What Atlas stores about a session

Metadata, and only metadata:

- which session, and which repositories it worked in
- which tool ran, whether it succeeded, and at most one normalized path
- which paths the index observed changing, and whether this session's edit tools
  named them
- reasons and decisions you explicitly asked Atlas to record

It does **not** store your prompts, Claude's replies, transcripts, tool inputs,
tool outputs, error text, shell commands, source snippets, environment variables
or credentials. `docs/ai-trust-boundary.md` in the Atlas repository lists this
exhaustively, and the test suite asserts it against payloads containing all of
them.

## Failure behaviour

If the daemon is down, slow, or answers something malformed:

- hooks return `{}` and exit 0, so the session is unaffected
- MCP tools return `degraded: true` with a short explanation
- no reason and no decision is fabricated
- no repository file is touched
- diagnostics go to stderr, visible with `claude --debug`

To take Atlas out of the loop without reconfiguring Claude, set
`ATLAS_CLAUDE_DISABLE=1`. To stop it registering repositories automatically, set
`ATLAS_CLAUDE_NO_AUTO_REGISTER=1`.

## What it deliberately does not hook

`WorktreeCreate` is not configured. That hook *replaces* Claude's own worktree
creation with whatever the hook prints, and where a worktree lives is not Atlas'
decision to make. Worktree and directory changes are observed through
`CwdChanged` and `DirectoryAdded` instead, which report the same fact without
taking over the mechanism.
