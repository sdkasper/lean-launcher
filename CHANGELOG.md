# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Lean Launcher is an independent fork of [Takeoff](https://github.com/akiraeng/takeoff-launcher)
(MIT licensed, by akiraeng). Versioning restarts at 1.0.0 for the fork's own
release history below; the inherited pre-fork Takeoff version history is kept
further down for reference.

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
