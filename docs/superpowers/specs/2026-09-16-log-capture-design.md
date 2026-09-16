# Log Capture Action - Design

**Goal:** Add a fourth configurable Obsidian action ("Log") that inserts a timestamped line into today's daily note, at the end of a user-configured heading's section, using the same direct on-disk file I/O the existing Task/Note-Add actions already use.

## Background

The vault's QuickAdd plugin has a "🌱 Log Entry" capture choice that formats `- {{time}}: {{VALUE}}\n` and inserts it after the `## Log` heading in today's daily note. This feature replicates that as a native Lean Launcher action, so log capture works without Obsidian open.

An earlier design (session of 2026-09-15) proposed doing this via the Obsidian CLI's `eval code=<javascript>` developer command with a base64-encoded payload, plus new CLI-stdout-capture plumbing. That design is superseded - see "Rejected approach" below.

## Rejected approach: Obsidian CLI `eval`

The existing Task/Note-Add actions (`AppendTask`/`AppendNoteText` in `src/daily_note.h`) never invoke Obsidian or its CLI - they read/write the note file directly via Win32 file APIs. That is why they work even when Obsidian isn't running. The Obsidian CLI's `append`/`prepend`/`daily:append`/`daily:prepend` commands only support start-of-file, end-of-file, or after-frontmatter insertion - none support "after an arbitrary heading." Only `eval` (an explicitly-labeled developer command, not part of the stable command set) can do heading-aware insertion via the CLI, and it requires Obsidian to be running plus new subprocess-stdout-capture plumbing this codebase doesn't otherwise need.

## Chosen approach: local heading-based text insertion

Heading-based insertion is straightforward to do locally in C++, using the exact same "read the whole file, transform, write it back" shape this codebase already uses for JSON config parsing:

1. Read today's daily note path via the existing `ResolveTodayPath` (same as Task/Note-Add).
2. If the note doesn't exist: create it with the same minimal frontmatter `AppendLine` already writes for a new file, then append the log line - there's no heading to search for in a file that doesn't exist yet.
3. If the note exists: read it fully as UTF-8 (`ReadFileUtf8`, already in `obsidian_config.h`), decode to a `std::wstring`, and find the line that exactly matches the configured heading text (`Settings::logHeading`, default `"## Log"`).
   - If found: compute the end of that heading's *section* - defined as the position immediately before the next line that is itself a heading (starts with one or more `#` followed by a space) whose level (count of leading `#`) is less than or equal to the target heading's level, or end-of-file if no such line exists. A heading of a *deeper* level (e.g. `### Sub` nested under `## Log`) does not end the section - its content stays inside.
   - If not found: fall back to end-of-file, matching `AppendNoteText`'s existing behavior.
   - Insert the formatted log line at that position (adding a leading `\n` first if the preceding character isn't already one, mirroring `AppendLine`'s existing no-trailing-newline handling), then write the whole file back.

## Line format

`- HH:MM: <text>\n` - 24-hour local time (matching QuickAdd's `{{time}}` token), with the same embedded-CR/LF-stripping `BuildTaskLine`/`BuildPlainLine` already apply to pasted multi-line text.

## Settings surface

Mirrors the Task/Note-Add action shape exactly, plus one new field for the heading:

| Field | Default | Purpose |
|---|---|---|
| `logEnabled` | `true` | Master per-action toggle |
| `logPrefix` | `"l"` | Launcher prefix, e.g. `l back from a walk` |
| `logPillLabel` | `"Log"` | Result-row tag |
| `logPreviewPrefix` | `"Log: "` | Preview text shown before what was typed |
| `logHeading` | `"## Log"` | Exact heading line to insert after |

Settings UI: a fifth accordion section (`kSectionLog`) between "Add to note" and "Daily note overrides", following the exact same summary/detail-rows pattern as the other three actions.

## Dispatch and execution

Follows the Task/Note-Add pattern exactly: a new `AppCategory::LogAdd` value, a prefix-detection block in `UpdateResults()`, an execution block in `LaunchSelected()` calling the new `AppendLogEntry()`, a secondary-actions block (`Add log entry` / `Copy text` / `Open today's note`), a footer hint, an actions-panel label set, and a pill-label entry.

## Explicitly out of scope

Obsidian CLI, `eval`, base64 payloads, CLI-stdout-capture plumbing. If a future feature genuinely needs Obsidian-side execution (not just file I/O), that plumbing can be designed then, scoped to what that feature actually needs.
