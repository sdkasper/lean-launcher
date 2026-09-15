# Configurable Obsidian Integration Settings - Design

> Status: Approved for planning
> Date: 2026-09-15
> Related: [[EPIC-002 Lean Launcher v2 - Broader Obsidian Actions]], US-001, US-004, US-006 (prefix renames), new US for this feature

## Problem

Lean Launcher's three Obsidian actions - task capture (`T `), vault note search (`O `), append text (`a `) - are entirely hard-coded: the prefixes, their result-row pill labels, and their preview text are compile-time constants, and the whole integration is implicitly "on" whenever a vault is configured. There's no way to turn the integration off, rename a prefix, or relabel a result row without editing source.

This design adds a master "Enable Obsidian integration" toggle to a renamed Settings tab (`Vault` → `Obsidian`), gates all three actions behind it (and behind per-action sub-toggles), and makes each action's prefix and display labels user-editable - along with an optional manual override for the daily-note folder/format the vault auto-detection currently resolves silently.

## Scope

In scope:
- Master `obsidianEnabled` toggle; when off, behavior is bit-for-bit identical to a build with no Obsidian integration at all (no prefix parsing, no `NoteIndex` thread, no extra Settings rows beyond the toggle itself)
- Per-action toggles (`vaultSearchEnabled`, `taskAddEnabled`, `noteAddEnabled`) that gate prefix recognition independently, without hiding their config rows
- User-editable prefix, pill label, and (where applicable) preview-prefix text for each action, via a new inline free-text edit mode in Settings
- Prefix validation: non-empty, unique across all three (case-insensitive), checked at save time
- Optional manual override for daily-note folder/format, layered on top of the existing vault-plugin auto-detection
- Settings tab rename `Vault` → `Obsidian` (label and internal enum identifier)

Out of scope (unchanged by this design):
- The vault picker's cycle-on-Enter UX (kept as-is)
- Footer hints and actions-menu entry text (stay fixed strings, not exposed as settings)
- Anything from EPIC-002's other backlog items (templates, full-text search, theme/plugin toggles)

## Architecture

```
Settings (settings.h)
  └── new fields: obsidianEnabled, {vaultSearch,taskAdd,noteAdd}Enabled,
        per-action {prefix, pillLabel[, previewPrefix]}, dailyNote{Folder,Format}Override

obsidian_config.h (extended)
  ├── TryParsePrefix(input, prefix, outText) - generic, replaces the three
  │     hand-written TryParseTaskPrefix / TryParseNoteJumpPrefix / TryParseNoteTextPrefix
  └── ResolveDailyNoteConfig(vaultPath, settings) - wraps ReadDailyNoteConfig,
        overlays the two override fields when non-empty

launcher.h
  ├── UpdateResults(): each of the three prefix blocks now reads
  │     settings_.obsidianEnabled && settings_.<action>Enabled before calling
  │     TryParsePrefix(input_.text, settings_.<action>Prefix, ...)
  ├── LoadSettings()/SaveSettings(): new DWORD entries (toggles) + REG_SZ
  │     entries (prefixes, labels, overrides), same patterns as today
  ├── SettingsCategory::Vault → SettingsCategory::Obsidian (rename)
  ├── Row scheme extended past the current 0-9 flat range to fit the new
  │     Obsidian rows (master toggle, vault picker, 3x[toggle+prefix+pillLabel
  │     (+previewPrefix)], 2x daily-note override) - exact indices decided
  │     during planning, not fixed here
  └── new editingRow_ state (parallel to today's recordingRow_ hotkey-capture
        state): repurposes the search box's existing TextInput-style editing
        (Insert/Erase/Move/SelectAll) for exactly one settings row at a time
```

### Why a generic `TryParsePrefix` instead of three configurable-string variants

The three existing functions are identical except for the literal prefix string. Keeping three near-duplicate functions that each read a different `Settings` field would just move the duplication instead of removing it. A single generic function taking the prefix as a parameter is both less code and the natural shape once the prefix is no longer a compile-time constant - the call site (`launcher.h`) already knows which `Settings` field to pass. This replaces, not adds to, the three existing functions; call sites and tests move to the generic form.

### Why reuse the search box's text-editing state instead of a separate widget

The app already has one fully-featured text input (`input_`: insert, erase, caret move, selection, IME composition) wired to the search box. Building a second, independent text-editing implementation for Settings rows would duplicate all of that for no behavioral difference - the only new thing needed is a routing layer (`editingRow_`) that decides, per keystroke, whether the same editing operations apply to `input_.text` (search box) or to a temporary buffer for the row being edited, then commits that buffer to the target `Settings` field (running prefix validation if applicable) on Enter or discards it on Esc.

### Why per-action toggles don't hide their config rows

A disabled hotkey binding still shows its row with a "Disabled" state (`FormatBinding` returns `L"Disabled"`) rather than disappearing - the user can see and reconfigure it without re-enabling first. Per-action Obsidian toggles follow the same precedent: turning off `taskAddEnabled` stops `T ` from being recognized, but the row showing its prefix/label stays visible and editable. Only the master toggle removes rows entirely, because a fully-disabled integration has nothing left to configure.

## Data Flow

1. `LoadSettings()` reads `ObsidianEnabled` from the registry; if the value has never been written (fresh install or pre-this-feature install), the default is computed from whether `VaultPath` is already non-empty - migration-safe for the current install, opt-in for a clean one.
2. If `obsidianEnabled` is false, `UpdateResults()` never calls `TryParsePrefix` for any of the three actions, and `NoteIndex::Instance().Start()`/`Restart()` are never called - the app behaves as if the feature doesn't exist.
3. If `obsidianEnabled` is true, each action additionally checks its own sub-toggle before parsing its (now-configurable) prefix.
4. In Settings, activating a prefix/label row (Enter, or click) sets `editingRow_` to that row's index and seeds a temporary edit buffer from the current value; subsequent keystrokes route to that buffer via the same operations `input_` already supports.
5. Enter on an editing row: for prefix rows, validate (non-empty, no case-insensitive collision with the other two configured prefixes) - on failure, show an inline error and keep the row in edit mode; on success, commit the buffer to the `Settings` field, call `SaveSettings()`, and exit edit mode. For label rows, commit unconditionally (no validation needed - they're just display strings).
6. Esc while editing discards the buffer and exits edit mode without modifying the setting.
7. Daily-note override fields (folder/format) are plain optional text: empty means "keep auto-detecting", non-empty overrides that one piece. `ResolveDailyNoteConfig` is called everywhere `ReadDailyNoteConfig` is called today (load, vault-cycle) and overlays these on top of the auto-detected result.

## Settings UI Layout

Obsidian tab, in order:
1. **Enable Obsidian integration** (master toggle) - always visible, always the first row
2. *(rows below only rendered when the master toggle is on)*
3. **Obsidian Vault** (existing cycle-on-Enter picker, unchanged)
4. **Vault search**: enabled toggle, prefix (default `O`), pill label (default `Jump`) - no preview-prefix row, since its result rows show real note titles, not a fixed template
5. **Add task**: enabled toggle, prefix (default `T`), pill label (default `Task`), preview prefix (default `Add task: `)
6. **Add to note**: enabled toggle, prefix (default `a`), pill label (default `Note`), preview prefix (default `Add to today's note: `)
7. **Daily note folder override** (optional text, empty = auto-detect)
8. **Daily note format override** (optional text, empty = auto-detect)

~15 rows total when expanded; the existing scrollable card layout and scrollbar already handle a card taller than the viewport, so no new scrolling mechanism is needed.

## Validation

- Prefix: reject empty; reject case-insensitive duplicate of either other configured prefix. Error surfaces inline (same `settingsStatus_` mechanism already used for other Settings failures) and the row stays in edit mode with the rejected text still shown, so the user can correct it without retyping.
- Labels (pill, preview prefix) and daily-note overrides: no validation - free text, committed as typed, empty allowed (empty override = auto-detect; empty pill/preview label is a cosmetic choice the user can make).

## Error Handling

- Registry write failure on save: same existing `settingsStatus_ = L"Could not save this setting."` path, unchanged.
- A prefix collision is caught before any write happens (see Validation) - never reaches `SaveSettings()` in an invalid state.
- If `obsidianEnabled` is toggled off while `NoteIndex` is mid-index, `Stop()` runs the same shutdown path already exercised by vault changes today.

## Testing

- `TryParsePrefix` unit tests: generic parameterized coverage (replaces the three existing prefix-specific test blocks with one parameterized set run against each configured default), plus a case-insensitivity and trimming check per existing convention.
- Validation unit tests: duplicate prefix rejected, empty prefix rejected, valid change accepted.
- `ResolveDailyNoteConfig` unit tests: empty overrides pass through auto-detected values unchanged; non-empty folder/format overrides take precedence independently of each other.
- Settings load/save round-trip test extended to cover the new fields, including the migration-default computation for `obsidianEnabled`.
- Manual verification: toggle master off → confirm zero Obsidian rows beyond the toggle and zero prefix recognition; toggle a single action off → confirm its prefix stops matching while its config row stays visible; edit a prefix/label via the new inline editor → confirm Enter commits, Esc cancels, and a duplicate is rejected with the row remaining editable.

## Key Decisions Carried Forward

- Single-vault model (from US-002/EPIC-001) is unchanged - these settings configure actions within that one vault, not multi-vault selection.
- The existing row-numbering scheme (`IsRowInCategory`, `NextSettingsRow`, `SettingsRowAtPoint`, etc.) is extended in place rather than refactored to a generic per-category row list - the diff stays mechanical and reviewable, consistent with how the original `Vault` row was added as a single extra row on top of the v1 0-8 scheme.
