# US-003 Instant Note Jump - Design

> Status: Approved for planning
> Date: 2026-09-15
> Related: [[US-003 Instant Note Jump]], [[EPIC-002 Lean Launcher v2 - Broader Obsidian Actions]]

## Problem

Lean Launcher v1 ships one Obsidian action: quick task capture (`task <text>`). US-003 adds a second: fuzzy-type a note's title, press Enter, the note opens in Obsidian - without navigating Obsidian's own file explorer.

This is new infrastructure, not an extension of `daily_note.h`: it needs a cached, live-synced index of note titles (v1's direct-append model has no notion of "all notes in the vault"), and an "open a note" mechanism, which v1 explicitly never needed.

## Scope

In scope:
- A `note <text>` command prefix that fuzzy-matches against note titles in the single vault configured in Settings (same vault v1's `task` prefix already uses)
- A live-synced background index of the vault's `.md` files
- Opening the matched note in Obsidian via Obsidian's official CLI (`Obsidian.com`, ships with every install) - no community plugin dependency, no hand-rolled URI scheme

Out of scope (future EPIC-002 items, not this design):
- Full-text search of note *content* (only titles/filenames)
- Multi-vault search (v1/this design both target the single configured vault)
- New-note-from-template, plain-text daily capture, theme/plugin toggles - separate US items under EPIC-002

## Architecture

```
LauncherWindow (launcher.h)
  └── prefix detector ─────► "note <text>" recognized alongside
                               existing "task <text>" and calculator patterns
        └── NoteIndex::Search(query) → ranked NoteResult rows (AppCategory::NoteJump)
              └── Enter → OpenNoteInObsidian(vaultName, relativePath)

note_index.h (new)
  ├── NoteIndex singleton - background-threaded, chunked snapshot (shared_ptr<const IndexChunk>),
  │     same shape as FileIndex (file_index.h) but scoped to one vault root
  ├── Start(vaultPath, notifyHwnd) / Stop() / Restart(newVaultPath) - driven by Settings' vault picker
  └── Search(query) - reuses search.h's existing MatchScore() against note titles (filename minus .md)

obsidian_config.h (extended)
  └── OpenNoteInObsidian(vaultPath, relativeNotePath) - locates and spawns Obsidian's
        official CLI (Obsidian.com) via CreateProcessW: `open vault=<name> path=<relative-path>.md`
```

### Why a new `note_index.h` instead of extending `FileIndex`

Considered extending the existing whole-machine `FileIndex` (in `file_index.h`) to also watch the vault root and tag items by source. Rejected: it would couple two conceptually different features (general file search vs. vault-scoped note-jump) into one class, complicate `FileIndex::Search()`'s scoring/filtering with a per-source branch, and wouldn't cleanly support a query that must stay scoped to note titles only. A separate `NoteIndex` mirrors `FileIndex`'s proven pattern (chunked snapshots, background thread, change-notification watcher) while staying isolated and independently testable - consistent with how `daily_note.h` was kept separate from `file_index.h` in v1 rather than widening an existing module.

## Data Flow

1. User types `note standup`
2. Prefix detected → `NoteIndex::Search(L"standup")` runs against the cached snapshot
3. Ranked result rows rendered: title + relative folder path for disambiguation (e.g. "Standup - 06 BJ/10 Daily/2026/09")
4. Enter → `OpenNoteInObsidian` locates `Obsidian.com` and spawns `obsidian open vault="Lean Notes" path="06 BJ/10 Daily/2026-09-14.md"` via `CreateProcessW`
5. The CLI hands off to the running (or newly launched) Obsidian app and opens/focuses the note
6. Launcher hides, same as any other launch - Obsidian's window opens/focuses, which is expected and correct here (unlike v1's task-add, which deliberately never opens Obsidian)

## Indexing Engine

Mirrors `FileIndex` (`file_index.h`) but scoped to a single vault root instead of the whole machine:

- `NoteIndex::Start(vaultPath, hwnd)` spawns a background thread, walks the vault root once (`recursive_directory_iterator`, `.md` files only, skipping `.obsidian/` and `.trash/`), publishes a chunked snapshot via the same `PublishSnapshot`/`AppendSnapshotChunk` shape as `FileIndex`.
- Live sync: `FindFirstChangeNotificationW` on the vault root (recursive) triggers a debounced reindex (reuse `FileIndex`'s ~3s debounce), plus a periodic idle rescan as a safety net (mirror `FileIndex`'s 5-minute interval).
- Driven by vault selection: Settings' existing "Obsidian Vault" row calls `NoteIndex::Instance().Restart(newPath)` when the vault changes (stop, clear snapshot, start against the new root). No vault selected → `NoteIndex` is simply never started, and `note ` shows the same "Set up your vault in Settings" row v1 already uses for `task `.
- Result cap: reuse `FileIndex::Search`'s existing `maxResults = 30` convention - no new tuning knob.
- Scoring: reuse `search.h`'s existing `MatchScore(name, query)` (acronym/prefix/token-aware) against note titles (filename minus `.md`) - no new scoring engine.

## Open Mechanism

Obsidian ships an official CLI (`Obsidian.com`, `FileDescription: "Obsidian CLI"`) with every Windows install, installed to `%LOCALAPPDATA%\Obsidian\Obsidian.com` and also registered on `PATH`. Its `open` command takes a vault-relative path and a vault name and opens/focuses the note directly:

```
obsidian open vault="Lean Notes" path="06 BJ/10 Daily/2026-09-14.md"
→ Opened: 06 BJ/10 Daily/2026-09-14.md   (exit 0)
```

**Verified 2026-09-15** against a real note in `$VAULT_PATH` (`01 Projects/LP Products/Lean Launcher/20 Architecture.md`) - forward-slash relative paths with the `.md` extension, quoted vault name by display name, clean text output, exit code 0. No percent-encoding needed; no undocumented URI parameter contract to reverse-engineer.

`OpenNoteInObsidian(vaultPath, relativeNoteRef)`:
1. Locates the CLI: try `%LOCALAPPDATA%\Obsidian\Obsidian.com` first (Obsidian's standard non-portable Windows install location - the same "known fixed location, no user config" convention this file already trusts for `%APPDATA%\obsidian\obsidian.json`), falling back to a `PATH` search (`SearchPathW`) for portable/custom installs.
2. Builds the command line with a hand-rolled Windows command-line argument quoter (`QuoteCommandLineArgument`, implementing the documented `CommandLineToArgvW` escaping algorithm) - not a raw string join, since a note title containing a literal `"` could otherwise inject an unintended `vault=`/`path=` boundary.
3. Spawns the CLI via `CreateProcessW` with `CREATE_NO_WINDOW` (the CLI is console-subsystem; this avoids flashing a console window).

This resolves EPIC-002's "architecture undecided" note for this action more cleanly than either originally-considered option (`obsidian://open` URI or the Advanced URI plugin) - it's Obsidian's own documented, supported interface, not a URI scheme being driven by guessed parameters.

> Note for future EPIC-002 work: this CLI also exposes `search`, `tasks`, `append`/`prepend`, and `create` (with template support) - directly relevant to EPIC-002's other backlog items (full-text search, template-based note creation). Worth revisiting those items' own designs against this CLI rather than assuming they need their own bespoke mechanism.

## Error Handling

| Condition | Behavior |
|---|---|
| No vault configured | `note ` prefix shows "Set up your vault in Settings" (matches v1's `task` behavior) |
| Duplicate note titles in different folders | Both shown as separate results; folder path disambiguates, no auto-merge/dedup |
| Vault has zero or a very large number of notes | No artificial cap on indexing; result *list* still capped at 30 like file search |
| Obsidian CLI not found, or `CreateProcessW` fails to spawn it | Inline error shown - never silently no-ops, matching v1's write-failure handling |

## Testing

- Pure-function unit tests in `tests/core_tests.cpp`: title extraction from path, `QuoteCommandLineArgument`'s Windows command-line escaping (spaces, embedded quotes, trailing backslashes), a `MatchScore` reuse sanity-check against note titles.
- CLI discovery and process-spawn behavior are exercised manually only (Task 5's smoke test) - not in the automated `ctest` suite, since the CI runner (`windows-latest` GitHub Actions) has no Obsidian install to spawn.
- `NoteIndex` build/watch logic tested against a temp directory, never the real vault - same convention as `daily_note.h`'s `AppendTask` tests.
- Manual smoke test against a scratch vault with nested folders and at least one duplicate title before calling this done.

## Key Decisions Carried Forward

- **Dedicated `note <text>` prefix**, not blended into default app/file search - explicit, no ambiguity, matches v1's existing `task <text>` pattern.
- **Live file watching**, not periodic-only refresh - mirrors `FileIndex`'s proven pattern; a note created seconds ago in Obsidian should be jumpable without waiting on a timer.
- **New `note_index.h` module**, not an extension of `FileIndex` - isolation over reuse, matching the codebase's existing small-module bias.
- **Single configured vault only** - same vault v1's `task` prefix uses; multi-vault search is out of scope (also out of scope for v1 per `20 Architecture.md`'s own backlog).
- **Obsidian's official CLI for the open action**, not a hand-rolled `obsidian://open` URI - discovered mid-design that Obsidian ships `Obsidian.com` with every install; verified live against the real vault before committing to this mechanism. Documented, supported, no percent-encoding to get wrong.
