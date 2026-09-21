# Full-Disk File Search + User Exclusions - Design

**Goal:** Make file search actually cover the whole disk (except things excluded on purpose), without making the launcher feel slower or heavier, and give the user an easy way to exclude their own folders/file types on top of the built-in exclusions.

## Background

A beta tester reported file search feeling "inconsistent" - some files/folders found, others not, and asked whether it just needed more time to index. Investigation of `src/file_index.h` found this isn't a timing issue:

1. **Hard 50,000-file global cap (`kMaxFiles`)**, consumed in a fixed phase order (repo root -> Desktop/Documents/Downloads/Pictures/Music/Videos -> other `%USERPROFILE%` subfolders -> drives, C: first). Once the cap is hit, every later phase is skipped entirely, forever, every run. On a machine with large Documents/Pictures/Downloads, an entire secondary drive can be silently excluded every single launch.
2. **No "still indexing" signal** while typing - the only existing message (`"Still indexing - try again in a moment."`, `launcher.h:2612-2620`) fires only on Enter-with-zero-results falling through to web search, not during normal browsing. A miss looks identical whether the file doesn't exist, isn't indexed yet, or was never going to be indexed.
3. **Only 3 folders get live change-notifications** (Desktop/Documents/Downloads, `file_index.h:685-733`). New files elsewhere only appear after the next periodic full rescan, which happens every 5 minutes and re-walks the *entire* machine from scratch every time.
4. **No user control over what's excluded.** The hardcoded skip-list (`ShouldSkipDirectory`) and extension allowlist (`IsUserRelevantFile`) are reasonable defaults but fixed - a user can't add their own folder or file-type exclusions (e.g. a large media archive they never want surfaced).

## Considered approaches

Three ways to close the "index everything, except what's excluded on purpose" gap were evaluated:

- **A - Just remove/raise the cap.** Trivial change, but doesn't fix the real problem: the periodic rescan still walks the *entire* disk from scratch every 5 minutes, forever. That recurring full-disk walk - not the one-time cost - is the actual performance risk. Rejected as insufficient on its own.
- **B - A, plus directory-mtime pruning to skip reprocessing unchanged subtrees.** Better, but still requires opening/stat-ing every directory on every pass just to read its mtime, so it doesn't remove the periodic full traversal, only some of the per-file work inside it.
- **C - Persisted index + NTFS USN Journal for true live updates (the "Everything"-style approach).** The only option with near-zero ongoing cost (no periodic re-walk at all, real-time change events volume-wide). Rejected: reading a volume's USN Journal requires an elevated/administrator token to open a raw volume handle, which conflicts with this app's `asInvoker` (no-UAC) manifest (`src/LeanLauncher.manifest:8`) and current lightweight positioning. It also only covers NTFS fixed drives, needing a walker fallback for removable/exFAT/FAT32 media anyway.

**Chosen: Option D** - a capless walker with mtime-pruned incremental rescans, a persisted on-disk cache so most launches skip walking entirely, I/O-priority throttling, and a leaner in-memory layout so "index everything" doesn't cost unbounded memory. No elevation, no NTFS-specific code.

## Chosen approach: Option D

### Phases

1. **First-run full walk** (once per machine, or once per cache-format version bump). No file-count cap. Walks every fixed/removable drive, applying the existing hardcoded skip-list/allowlist *plus* the new user exclusion file (additive, see below), with I/O-priority throttling (below). Publishes chunks incrementally as today, so partial results stay searchable throughout. If interrupted (crash, kill, log-off) before completion, no partial cache is written - the next launch simply restarts this phase from scratch. This only ever happens again on a cache-format version bump, so paying for true crash-resumability wasn't judged worth the added complexity.
2. **Startup cache load** (every launch after the first). Reads the persisted binary cache directly into memory - no disk walk, ready near-instantly - then immediately kicks off phase 3 in the background to catch up on anything that changed while the app was closed.
3. **Incremental mtime-pruned rescan.** Runs once at startup and then on the existing 5-minute idle timer, plus the existing Desktop/Documents/Downloads live-change triggers. For each directory, compares its current mtime against the value stored at last scan; unchanged directories are skipped rather than re-walked and re-added. **Before anything else, each pass does a cheap top-level `fs::exists()` check per drive root represented in the cache** - if a drive isn't currently present (e.g. an unplugged USB drive), every cached entry under it is pruned immediately from the in-memory snapshot and the next cache write. This is a single existence check per drive letter, not a walk, so it's effectively free, and it self-heals in both directions: unplug prunes within the next pass, replug gets re-scanned and merged back in like any other changed directory.

Indexing continues in the background for the life of the process regardless of window visibility - hiding the launcher (`ShowWindow(hwnd_, SW_HIDE)`, e.g. Escape or losing focus) does not stop the `FileIndex` singleton's background thread; only true process exit (`WM_DESTROY`, `launcher.h:471`) does, via `FileIndex::Instance().Stop()`. Re-opening the window afterward reflects the same live, continuously-updated in-memory state, never a stale or reset one.

### Indexing status UI

A persistent status line, e.g. `"Indexing files... (128,402 so far)"`, driven by the existing chunked `Count()` plus a new `Phase` accessor (`FirstWalk` / `Loaded` / `IncrementalRescan`) on `FileIndex`. Shown in the launcher's existing status area whenever the window is open during phase 1, and disappears for good on that machine once the first walk completes (barring a future cache-version bump). This only appears once, ever, per machine under normal operation.

### Memory layout: path interning

`FileItem` currently stores the full absolute path twice per file (`path` + `normPath`, `file_index.h:25-31`) - the dominant cost behind the existing test's ~550 bytes/item estimate, which has only ever been measured against this repo's own file count (a few thousand), never at real-disk scale. Replaced with a `DirectoryPool`: each unique parent directory is stored once (`path`, `normPath`, plus its `lastKnownMtime` for pruning), and each `FileItem` holds only `{name, normName, parentPoolIndex, isDirectory}`. `Search()`'s path-substring matching, which currently reads `item.normPath` directly, reconstructs the full normalized path on demand from `pool[parentPoolIndex] + name` - but only for candidates that don't already match by name, so the hot path stays cheap. This is expected to cut per-item memory roughly 3-5x for typical deep/wide trees, which is what actually makes "index everything" safe, more than the walking strategy does.

### Persisted cache

A versioned custom binary format at `%LOCALAPPDATA%\LeanLauncher\file_index.cache` (same folder already used for update staging, `updates.h:201`) - header (magic + format version + counts), the directory pool, then item records. JSON/text isn't viable at the scale this can reach (hundreds of thousands to millions of entries); a version mismatch is treated identically to no-cache-present (fall back to a full walk) rather than attempting migration.

### I/O throttling

`THREAD_PRIORITY_BELOW_NORMAL` (already set today, `file_index.h:680`) only affects CPU scheduling, not disk queue priority, which is the actual bottleneck on an I/O-bound walk. Add `SetFileInformationByHandle(..., FileIoPriorityHintInfo, IoPriorityHintLow, ...)` on the scan thread's directory handles, plus a short periodic sleep every N directories as cheap insurance for older Windows versions or drivers that don't fully honor the I/O priority hint.

### User exclusions

A single flat file at `%LOCALAPPDATA%\LeanLauncher\file_search_excludes.txt`, one entry per line, type auto-detected by shape: a line starting with a drive letter or `\\`/`/` is a folder-path exclusion; a line starting with `.` is an extension exclusion. Blank/comment/ambiguous lines are silently ignored rather than guessed at. Re-read at the start of every full/incremental pass (cheap), so edits take effect on the next scan without requiring a restart.

This is **strictly additive** on top of the existing hardcoded skip-list and extension allowlist - those stay fixed and non-editable (there's no good reason to let a user re-include something like `system32`, and widening the allowlist itself is a different feature). `ShouldSkipDirectory` and `IsUserRelevantFile` each get one extra check against the loaded exclusion sets, layered on their existing logic.

### Settings & docs

One new Settings row area: "File search exclusions", with an "Edit..." action that creates the exclusions file with a short header-comment template if it doesn't exist yet, then opens it via `ShellExecuteW` (same pattern as the existing repo-link action, `launcher.h:2177`); and a "Help" action opening a new `docs/file-search.md` via a new `kFileSearchHelpUrl` constant (mirroring `kRepoUrl`, `updates.h:23`), following the exact same `ShellExecuteW(open, url)` call already used elsewhere. `docs/file-search.md` documents both exclusion layers (fixed built-in vs. user file) and the exclusion file's line-shape convention; README gets a one-line pointer to it under Features. As with the last two Settings-row additions, this shifts every subsequent row constant - an already-established, documented cost from prior sessions.

## Error handling

- Missing/corrupt/version-mismatched cache -> treated as no-cache-present, falls back to a full walk.
- Missing exclusions file -> empty exclusion set, not an error.
- Permission-denied directories during the walk -> unchanged from today (`skip_permission_denied` + existing try/catch).
- Absent removable drive -> pruned on the next incremental pass (see Phases above), not left as permanent stale entries.
- Cache write failure (disk full, permission issue) -> non-fatal; keeps running on the in-memory index, retries on the next completed walk/exit.
- No cross-process cache-write races - the app already enforces a single-instance mutex.

## Testing

- `DirectoryPool` interning: dedupe correctness, and that lazy path-reconstruction during search matches what the old always-stored `normPath` would have produced.
- Cache round-trip: build a small index via the existing `scanRootOverride` test hook, save, reload into a fresh `FileIndex`, verify identical search results.
- Version-mismatch handling: reject an old/wrong-version cache header, confirm fallback to a full walk rather than misreading garbage.
- Exclusion parsing: garbage/ambiguous lines ignored; folder-prefix vs. extension lines correctly classified by shape; an excluded subfolder under a scoped test root is verifiably absent from results.
- Drive-prune: deterministic unit test feeding the prune step a snapshot with entries under a non-existent path, verifying removal.
- Heap-budget test recalculated against the new interned-struct size estimate, plus one new synthetic test modeling a realistic large scale (e.g. 500K items) - the current test has only ever validated against this repo's own small file count, never at real disk scale.
- Existing 20ms/query search benchmark must still pass with lazy path-reconstruction added to the path-matching branch.
- Manual verification (TC-xxx): exclusions row + Help button open correctly; editing the exclusions file changes results after the next rescan; the indexing status appears/disappears correctly; cache persists across restarts; drive unplug/replug is observed to prune/restore live.

## Explicitly out of scope

- Crash-resumable first-run walk (mid-walk checkpointing) - restart-from-scratch is acceptable since this only happens once per machine.
- NTFS USN Journal / MFT-based enumeration - would require administrator elevation, conflicting with the app's current `asInvoker` positioning; revisit only if that trade-off is reconsidered.
- Letting users override or widen the built-in skip-list/extension allowlist - the new exclusion file is exclude-only, additive.
- Live (on-keystroke) reload of the exclusions file - it's read at the start of each scan pass, not watched for changes in real time.
