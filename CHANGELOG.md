# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Lean Launcher is an independent fork of [Takeoff](https://github.com/akiraeng/takeoff-launcher)
(MIT licensed, by akiraeng). Versioning restarts at 1.0.0 for the fork's own
release history below; the inherited pre-fork Takeoff version history is kept
further down for reference.

## [1.6.0] - 2026-09-23

### Added
- **Capture target notes** - task (`t`), note (`a`), and log (`l`) capture can each write to a specific note (e.g. `Inbox/Tasks`) instead of today's daily note, configured per action under Settings > Obsidian. Leave a field empty to keep using the daily note. Paths outside the vault are rejected when saving, and the capture preview now names a non-default destination.
- **Quick open with `o .`** - typing the vault search prefix followed by a dot opens today's daily note in Obsidian, or the task/note/log target note chosen in the new "Quick open" setting. If the note doesn't exist yet, the result says so and nothing is created.
- **Pinned results** - pin up to 5 apps, files, or folders via the actions menu (`Ctrl+K` → Pin). Pinned items lead the empty search box (pinned files included) and rise to the top of results whenever they match the query. Pins persist across restarts; uninstalled apps and deleted files are dropped silently.
- **Index counts in the About tab** - Settings > About shows how many files and vault notes are currently indexed.

### Changed
- The default "Add to note" preview text is now "Add to note:" (was "Add to today's note:"), since a capture can now target a note other than the daily note. A customized preview is left unchanged.

### Fixed
- **Captures created a new daily note in the vault root when the Lazy Plugin Loader was in use** - Lazy Plugin Loader removes lazily-started plugins (such as Journals) from `community-plugins.json`, so Lean Launcher treated them as disabled and fell back to the vault root. Plugin detection now also reads the Lazy Plugin Loader's own startup settings.
- **Daily-note folder settings could point outside the vault** - a rooted (`\folder`) or drive-relative (`C:folder`) value in a vault's daily-notes config wasn't recognized as unsafe and, joined onto the vault path, escaped it. Such values are now rejected like absolute paths and `..` segments.
- The UI smoke test script (`tests/ui_smoke.py`) now targets the current `LeanLauncherUiTests.exe` build output and window class.

## [1.5.5] - 2026-09-22

### Fixed
- **Obsidian vault picker dropdown silently truncated for users with many vaults** - the dropdown drew and hit-tested its entire vault list at a fixed height with no scrolling, an assumption ("never more than a handful of real-world entries") that broke for a user with 14 vaults: entries past the settings panel's bottom edge were clipped from view and unreachable by mouse, and keyboard arrow-key highlighting could land on an off-screen, invisible item. The dropdown now clamps to the available viewport height, scrolls via mouse wheel or arrow keys (auto-scrolling to keep the highlighted item visible), and shows a scrollbar thumb when the list overflows.

## [1.5.4] - 2026-09-22

### Documentation
- **Windows Defender false-positive note** - added a README warning and this Known Issues entry after a user report of `LeanLauncher.exe` being flagged as `Trojan:Script/Sabsik.EN.A!ml` on first run. This is a cloud ML heuristic false positive, not a real detection - the release build is not yet code-signed, which makes an unsigned, low-download-count, self-updating executable (the built-in updater replaces its own binary in place) look statistically similar to a dropper. See the README's Windows Defender note for workarounds; a permanent fix via code signing is planned. No application behavior changed in this release.

### Known Issues
- Windows Defender may flag this and prior releases as `Trojan:Script/Sabsik.EN.A!ml` - false positive, see above.

## [1.5.3] - 2026-09-22

### Fixed
- **"Open containing folder" opened the wrong folder for files at a drive root** - `GetLogicalDriveStringsW` interns drive roots with a trailing backslash (e.g. `D:\`), and the file-search result path builder unconditionally appended another separator, producing a doubled backslash (e.g. `D:\\file.txt`) for any file directly under a drive root. `explorer.exe`'s `/select` argument doesn't reliably resolve that, so it silently fell back to Explorer's own default folder instead. "Open" and "Copy file path" were unaffected in terms of functioning, but the copied path itself carried the same doubled backslash. Files in subdirectories were never affected.

## [1.5.2] - 2026-09-22

### Fixed
- **CI/release pipeline** - the full-disk search benchmark's two latency assertions (path-matching and fuzzy name-matching, both tuned against the primary dev machine) failed on GitHub's shared CI runner the first time this code was ever pushed to origin, blocking the Release workflow from publishing. Both ceilings raised to account for CI-class hardware; see `tests/core_tests.cpp` for details. No application behavior changed.

Note: `v1.5.0` and `v1.5.1` were both tagged but their Release workflows failed before publishing (one per latency assertion), so neither release exists - this release is the first one actually published for the features below.

### Added
- **Full-disk file search** - the file-count cap (previously 50,000 files) has been removed; search now covers every fixed and removable drive. The first index is a one-time full walk with a persisted, versioned on-disk cache for instant startup afterward, followed by a mtime-pruned incremental rescan (startup and periodic) so only changed folders are re-walked. Unplugging a removable drive prunes its files from results until it's reconnected.
- **User-configurable file search exclusions** - add your own folder or file-extension exclusions on top of the built-in skip-list via a new "Edit exclusions..." row in Settings > Search, which opens `%LOCALAPPDATA%\LeanLauncher\file_search_excludes.txt` (auto-created with a header template) in your default text editor. Strictly additive - cannot re-include anything the built-in rules already exclude. A new "Help" row explains both exclusion layers.

### Known Limitations
- File search indexing progress is only surfaced as a simple "Indexing..." state, not a live item count.
- Fuzzy name-matching search latency has not yet been re-validated against the new uncapped full-disk item counts and may exceed the app's usual search-latency budget on very large drives.

## [1.4.1] - 2026-09-18

### Fixed
- **In-app version display and update-check comparison were stuck at 1.2.2** - `kAppVersion` in `src/updates.h` is a separate hardcoded constant from the `CMakeLists.txt`/`LeanLauncher.rc` version and was missed by the last two version bumps (v1.3.0, v1.4.0), so the Settings footer and the `checkForUpdates` newer-version comparison were both reading a stale value. Now in sync.

## [1.4.0] - 2026-09-18

### Added
- **Customizable search-scope prefixes** - three new prefixes narrow or force a search: `w <query>` (default) forces a single top web-search result regardless of other matches, respecting the Web search toggle; `f <query>` (default) shows only file/folder matches; `p <query>` (default) shows only installed-app matches and is always active. All three prefixes are inline-editable in Settings > Search and validated as mutually unique against each other and the existing Obsidian `t`/`a`/`l`/`o` prefixes.

## [1.3.0] - 2026-09-18

### Added
- **Configurable web search engine** - the "Web search" fallback now supports Google, Bing, DuckDuckGo, Startpage, Ecosia, Brave, Kagi, or a custom search URL, picked from a new "Search engine" row in Settings > Search. A custom URL template must contain a literal `{query}` token and use `http://`/`https://`; its display name is auto-derived from the hostname. Existing installs default to Google with no behavior change.

## [1.2.2] - 2026-09-18

### Fixed
- **Search no longer hides apps whose brand name fuses "uninstall(er)" without a space** (e.g. BC Uninstaller) - the filter meant to hide auto-generated "Uninstall \<App\>" helper shortcuts matched on an unanchored substring instead of a word boundary, so an app's own primary shortcut could be mistaken for its uninstall helper and excluded from the index entirely

### Changed
- Search placeholder text now varies based on whether Obsidian integration is enabled ("Launch apps, search files, capture thoughts…" vs "Launch apps, search files…"), so it never implies capture prefixes that aren't active
- Internal: closed the CMake/MSBuild Release binary-size gap (~17KB down to ~0.5KB) by adding the `/sdl` compiler flag to `CMakeLists.txt` that the hand-maintained `.vcxproj` already had

## [1.2.1] - 2026-09-17

A full-codebase security and code review round (2 security findings, 4 high, 8 medium, and 6 low-severity code review findings) - no user-facing feature changes, but several fixes affect behavior users could actually notice.

### Security
- **Untrusted vault config could redirect note writes outside the vault** - `folder`/`format` values read from a vault's own `daily-notes.json`/Periodic Notes/Journals config are now validated before use; a synced/shared vault with a tampered config could otherwise use a UNC path (triggering an outbound network authentication handshake) or `..` traversal to redirect captures
- **Update endpoints could be silently redirected via undocumented registry keys** - removed the unvalidated `HKCU\Software\LeanLauncher` override for the update host/path/releases URL; these are compile-time constants only now

### Fixed
- Crash (heap corruption) during app-index builds caused by a double-free of a Shell API string result
- Crash (`std::terminate`) if a removable drive was unplugged mid-scan during file indexing
- A daily note could be truncated to zero bytes if a log-capture write failed partway through (e.g. full disk, sync client lock) - log capture now writes to a temp file and swaps it in atomically
- Update checks silently defaulted to enabled the moment the settings registry key existed for any unrelated reason, even though update checking is opt-in
- Update checks fired on every launch instead of respecting the intended 24-hour throttle
- Opening a file or folder result could silently evict genuinely recent apps from the "Recent" list
- Update-asset selection used substring matching, so a published checksum file (e.g. `LeanLauncher.exe.sha256`) could be picked over the real executable and permanently break auto-update
- The auto-update fallback script could corrupt file paths - and silently fail to relaunch - for Windows usernames containing non-ASCII characters
- Daily-note detection via the Periodic Notes plugin could pick up a sibling section's folder (e.g. "weekly") instead of "daily"'s own
- App shutdown could hang for up to ~45 seconds (triggering a Windows "not responding" prompt) if an update check/download was in progress
- `DDDD`-style date formats could silently render as garbled filenames instead of falling back to the default format
- Two background-thread result messages could leak their heap payload if the app was closed before they were processed

### Changed
- Internal: search-scoring and Settings row-list hot paths no longer allocate on every keystroke/mouse-move (performance only, no visible behavior change)

## [1.2.0] - 2026-09-16

### Added
- **Quick log capture** (`l <text>`, default prefix) - inserts a timestamped line (`- HH:MM: <text>`) at the end of a configured heading's section (default `## Log`) in today's daily note, replicating a QuickAdd-style running log natively in the launcher. Like task/note capture, it writes directly to the note file on disk - Obsidian never needs to be open. Fully configurable from Settings alongside the other three actions (enable toggle, prefix, result label, preview text, and the target heading).

## [1.1.0] - 2026-09-16

### Added
- **Journals plugin support for daily-note discovery** - `t <text>`/`a <text>` now correctly detect the daily-note folder and date format from vaults using the [Journals plugin](https://github.com/gerrywastaken/obsidian-journals) instead of core Daily Notes or Periodic Notes

### Fixed
- **Stale, disabled daily-notes config no longer wins** - if a vault previously used core Daily Notes and later switched to Periodic Notes or Journals, Obsidian leaves the old `daily-notes.json` file behind even after disabling the plugin; daily-note detection now checks `core-plugins.json`/`community-plugins.json` to confirm a source is actually enabled before trusting its config, rather than just checking whether the file exists

## [1.0.2] - 2026-09-16

### Fixed
- **"Reset to Defaults" now also clears the configured Obsidian vault** - previously it reset every other setting but left the vault path selected
- **Vault detection no longer blocks the UI thread** - opening Settings used to check whether every known vault still exists synchronously, which could stall the whole launcher popup if a vault lived on a disconnected network drive; this scan now runs on a background thread
- **Release notes now reflect actual changes** - GitHub's auto-generated release notes were bare (just a compare link) because this repo has no pull-request history for GitHub to summarize from; releases now pull their notes from the matching `CHANGELOG.md` section instead

## [1.0.1] - 2026-09-15

### Added
- **About tab** in Settings: shows the app name and version, an author/attribution line, and a "View on GitHub" link that opens the repository in your browser

### Fixed
- **System tray icon tooltip**: hovering the tray icon now shows "Lean Launcher", matching other tray apps. Previously the tooltip text was set but never rendered, because Windows requires the `NIF_SHOWTIP` flag once an icon opts into modern (`NOTIFYICON_VERSION_4`) notification behavior - `NIF_TIP` alone isn't enough

## [1.0.0] - 2026-09-15

### Added
- **Obsidian Integration** (optional, off by default until a vault is configured): quick task capture (`t <text>` appends a checklist item to today's daily note), quick note capture (`a <text>` appends a plain line), and instant note jump (`o <text>` fuzzy-matches note titles and opens the match via Obsidian's own CLI) - vault and daily-note location are auto-detected by reading Obsidian's own config files, no plugin dependency
- **Configurable Obsidian Settings**: master enable/disable toggle, a per-action enable toggle for vault search/add task/add-to-note, inline-editable prefix/result-label/preview text per action with prefix-uniqueness validation, and an optional manual daily-note folder/format override for vaults where auto-detection doesn't fit
- **Vault picker dropdown**: click or press Enter on the Obsidian Vault row to open an overlay list of every detected vault (mouse or Up/Down/Enter/Esc)
- **Collapsible Settings sections**: each Obsidian action block (and the daily-note-overrides block) collapses to a one-line summary by default, expanding one collapses whichever other was open
- New application icon

### Changed
- Rebranded as an independent fork of Takeoff: distinct window class (`LeanLauncherWindow`), single-instance mutex, registry root (`HKCU\Software\LeanLauncher`), and Start Menu shortcut, so it can run alongside a real Takeoff install without colliding
- Auto-updater repointed at this project's own GitHub releases; disabled by default until this release (see `NFR-003`)

## Pre-fork history (inherited from Takeoff)

The entries below predate the fork and describe functionality already present when Lean Launcher branched off - kept for reference, not part of this project's own version numbering.

### Calculator

- **Built-in Calculator**: Real-time evaluation of mathematical expressions directly within the launcher search bar (e.g. `125 * 8`, `(10 + 20) * 3`, `sqrt(144)`, `2^10`, `10 % 3`, `200 * 15%`).
- **Instant Result Display & Copy**: Top-ranked calculation result displayed with dedicated calculator badge, immediate `Enter` shortcut to copy the result and close, and `Ctrl+C` to copy without closing.
- **Calculator Actions**: Context actions (`Ctrl+K`) for copying the numeric result, copying the full calculation (`expression = result`), or opening Windows Calculator.

### [1.0.3] - 2026-09-12

#### Added
- **Automatic Silent Update Downloading**: Releases are downloaded in the background from GitHub Releases using WinHTTP streaming with HTTP 302 cross-domain redirect following (GitHub to AWS S3).
- **Restart to Update**: Interactive "Restart to Update" button in the footer and tray menu once an update has been silently downloaded and validated.
- **Robust In-Place Executable Swap**: Atomic executable replacement with rollback protection, retry loops for transient antivirus scanner locks, and UAC elevation fallback for protected install locations.
- **PE Executable Verification**: Integrity check verifying DOS magic, `IMAGE_NT_SIGNATURE`, and x64 architecture before any update can be staged or installed.

### [1.0.2] - 2026-09-09

#### Added
- **Broad File & Folder Search**: Comprehensive in-memory file search indexing primary user folders and all fixed/removable drives with deep directory traversal (up to depth 8) and up to 150,000 files.
- **Folder & Path-Aware Search**: Direct matching of directory names as first-class items, multi-token path queries (e.g. `takeoff main` or `X:/takeoff-launcher`), and path substring matching.
- **Scrollable Settings**: Direct2D primitive-clipped settings viewport preventing footer overlap, with smooth mouse wheel scrolling, keyboard navigation, and custom scrollbar indicator.

### [1.0.0] - 2026-09-09

#### Added
- **Native Windows Acrylic Interface**: High-performance Win32 UI powered by Direct2D and DirectWrite with real DWM acrylic blur on Windows 11 and compositor acrylic accent policy on Windows 10.
- **Fast Fuzzy Search**: In-memory app indexing and weighted fuzzy matching supporting exact matches, acronyms (e.g. `vsc` for Visual Studio Code, `tm` for Task Manager), multi-token prefixes, and custom aliases.
- **System & Settings Applet Indexing**: Search for Control Panel tools and Windows settings (e.g., Device Manager, Services, Windows Update).
- **Session Recency Ranking**: Frequently and recently launched applications automatically rank higher in query results.
- **Configurable Hotkeys**: Customizable key bindings for global launcher summon (`Alt+Space`), action menu (`Ctrl+K`), administrator launch (`Ctrl+Enter`), and quick-launch numbers (`Alt+1` through `Alt+8`).
- **Context Actions Menu**: Run as administrator, copy application name, or copy target launch path via keyboard shortcut or right-click context menu.
- **System Tray Integration**: Optional notification area icon in Windows taskbar for opening launcher, accessing settings, or exiting.
- **Startup Integration**: Optional auto-start on Windows login with `--startup` switch to initialize silently into background.
- **Privacy-Respecting Update Checker**: Optional daily check against GitHub Releases to notify users of new versions, with zero telemetry or personal data transmission.
- **Automated Test Suite**: Regression test suite for search scoring, text caret navigation, hotkey migration, and version comparison, plus interactive UI smoke tests.
