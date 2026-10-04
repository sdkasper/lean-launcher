# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Lean Launcher is an independent fork of [Takeoff](https://github.com/akiraeng/takeoff-launcher)
(MIT licensed, by akiraeng). Versioning restarts at 1.0.0 for the fork's own
release history below; the inherited pre-fork Takeoff version history is kept
further down for reference.

## [Unreleased]

### Fixed
- **Editing a Settings text field now shows the whole value** - the field being edited used to be a small right-aligned box that only showed the end of a long value (a vault path, a note override), and its caret was always drawn at the end even after you moved it with the arrow keys (#6). The field now takes the full width of the row, is left-aligned, scrolls to keep the caret in view, and shows the real caret position and selection.

## [2.0.4] - 2026-10-04

### Fixed
- **Typed paths and folders that would not open** - on some PCs, pressing `Enter` or clicking a folder in the path completion results (for example after typing `C:\Users\`) showed "Could not open this item", and `Shift+Enter` just closed the launcher (#11). If Windows' normal "open folder" action fails, the launcher now opens the folder with Explorer directly. `Shift+Enter` also shows "Could not open this path." instead of closing silently when nothing could be opened.

## [2.0.3] - 2026-09-30

### Fixed
- **High CPU after every edit in your Obsidian vault** - with Obsidian integration on, the launcher's note index rescanned the whole vault after every content change (including adding a line from the launcher) and did extra disk work for every note each time, which could use about 35% CPU for 15 to 20 seconds on a small laptop (#7). It now only rescans when a note or folder is created, renamed, or deleted, and building the list no longer touches the disk per note (10,000 notes took about 940 ms before and about 9 ms now in the test). This is separate from the File search setting: turning File search off never stopped the note index, which follows the Obsidian integration setting.
- **Ctrl+V works in Settings text fields** - pasting into a text row such as the daily note overrides, prefixes, or the snippets file did nothing (#6). It now pastes the first line of the clipboard, trimmed.

### Added
- **Accent-insensitive search** - typing `apre` finds `âpre`, `delivrance` finds `délivrance`, `cafe` finds `Café`, and the other way round (#4). It works for apps, files, and notes. Only Latin letters are folded; letters without a plain base such as ø, æ, ł, and ß are unchanged, and other scripts are left alone. The file index rebuilds once after updating so its saved names match.
- **Stay open when clicking elsewhere** - new opt-in setting (Settings > General > WINDOW, off by default): clicking another window no longer hides the launcher. `Esc`, the hotkey, or launching something still closes it. If the launcher is visible but not in front, the hotkey brings it to the front (#6).
- **Remember last search** - new opt-in setting (Settings > General > WINDOW, off by default): reopening the launcher brings back your previous search with the text selected, so typing replaces it and `Enter` still works on it (#6). The text is only held in memory while the setting is on.

## [2.0.2] - 2026-09-29

### Changed
- **Graphics memory is released while the launcher is hidden** - after the launcher has been hidden for 15 minutes (or has never been opened for 15 minutes after starting), it releases its Direct2D render target and factory, which hands back the graphics driver's memory: about 35 MB of private bytes on a test machine with a dedicated graphics card. Nothing visible is lost. The next time you open the launcher the graphics are rebuilt before the window appears, which took about 125 ms on the test machine; opening it again within 15 minutes is unchanged. The release never happens while the launcher is visible. The README now explains private bytes versus the working set that Task Manager shows by default.

## [2.0.1] - 2026-09-29

### Fixed
- **Import from Espanso keeps your structure** - the import used to merge every Espanso file into one flat list and rewrite your snippets file without its comments. It now groups the snippets it adds under a `# From Espanso: <file>` header for each Espanso file (for example `# From Espanso: 3SS.yml`), keeps the section comments from your Espanso files (such as `# Invoices`), and leaves the comments and layout of your own snippets file as they are. The written file has a blank line between snippets and uses `\n` for line breaks in multi-line text, so it is easy to read and edit. Section headers of entries that are skipped (for example ones that use variables) are kept and moved to the next snippet. Comments are only read during an import; nothing extra is held in memory while snippets run.

## [2.0.0] - 2026-09-29

Snippets and a reorganised Settings page make this a milestone release. Nothing is removed: every setting, shortcut, and saved value keeps working, and settings exports from earlier versions import unchanged.

### Changed
- **Settings is reorganised into six tabs** - **General**, **Search**, **Tools**, **Snippets**, **Obsidian**, and **About**. The crowded Search tab (27 rows) is split up: File search, Web search, the search engine, prefixes, exclusions, and the preview panel stay in Search; unit and time zone converters, typed URLs, path completion, system commands, and Pomodoro move to Tools; the text expander gets its own Snippets tab. General combines the old Shortcuts and System tabs and also holds Export settings... / Import settings... in a BACKUP card (moved from About). Each tab groups its rows into titled cards. The "All" tab is gone, and Settings now opens on General.
- **"Check for updates when Lean Launcher starts" moved to About** - it now sits in the UPDATES card as "Check on startup", right above the "Check now" row (previously called "Check for updates"), so both update controls are in one place.

### Added
- **Snippets (text expander)** - opt-in and off by default (Settings > Snippets): type a trigger such as `:sig` in any app and it is replaced by your text. Snippets are kept in `%APPDATA%\LeanLauncher\snippets.yml`, created with two samples the first time you turn the feature on; the Settings row "Edit snippets" opens it. The file uses an Espanso-compatible subset: `matches:` with `trigger`, `replace`, and `label`, with multi-line text through `|` blocks or `\n` in quoted text. Variables, `word`, multiple `triggers`, and other Espanso features are not supported yet; such entries are skipped with a warning in Settings. Limits: file 1 MB, 5,000 snippets, replacement 64 KB, trigger 2-32 characters without spaces; a shorter trigger that is the start of a longer one fires first. Short single-line text is typed. Long or multi-line text is pasted only when the clipboard is empty or holds only plain text (the clipboard is then restored afterwards and the pasted text is kept out of Windows clipboard history); otherwise it is typed key by key, and a multi-line snippet typed this way sends Enter for each line, so editors with auto-indent may re-indent it (tip: use short single-line snippets, or avoid having rich text such as a copied web page on the clipboard, if that matters). The `,` prefix searches your snippets: type `, ` and part of a label (`, sig`), then `Enter` inserts into the window you came from, or copies to the clipboard if there is none. "Import from Espanso" asks first, saves a timestamped backup, skips entries with variables, and never turns the feature on. Settings export and import never include or turn on the Snippets toggle. **Privacy:** while on, a keyboard listener sees what you type to spot triggers, keeps at most the last 32 characters in memory only, never stores or sends them, and is fully removed when you turn Snippets off. It cannot see into or type into windows running as administrator and may not work in some games or remote desktop windows.

### Known limits
- Snippets do not work in windows running as administrator, and may not work in some games or remote desktop windows.
- Espanso variables, `word`, and multiple `triggers` are not supported yet; those entries are skipped with a warning.
- The snippets file is read when Snippets is turned on and each time the launcher opens, not while you edit it.
- A mouse click that moves the caret inside the same window does not reset the typed text, which can cause a rare false expansion.
- Password fields still expand snippets, because the listener cannot tell them apart.
- Triggers are case-sensitive, so Caps Lock changes the typed character.
- If expansions ever stop working (Windows removes keyboard hooks that answer too slowly), turn Snippets off and on again.
- Type the last trigger character and release it before pressing the next key; a fast rollover cancels the expansion.

## [1.9.1] - 2026-09-29

### Fixed
- **Restart to Update in a protected folder** - when Lean Launcher is installed somewhere it cannot write to, such as `C:\Program Files\LeanLauncher`, Restart to Update now asks for administrator permission once (a single Windows prompt) and installs the update, instead of closing the launcher without updating and leaving the old version to start again. The administrator prompt appears once, the downloaded file is checked against its download-time checksum and against the checksum published for the release on GitHub before it is installed (if GitHub cannot be reached, nothing is installed and the launcher restarts unchanged), the launcher starts again as your normal user, and if you choose No at the prompt it stays open and tells you how to retry. A background loop that could keep running after a failed update is gone.

### Notes
- If you installed Lean Launcher in a protected folder such as C:\Program Files and are on 1.9.0 or older, download 1.9.1 manually once: those versions cannot update themselves there.

## [1.9.0] - 2026-09-29

### Added
- **Preview panel** - press `Ctrl+P` to show the selected note, text file, folder, or image beside the results, read straight from disk - Obsidian is never started, and nothing is written to disk. Notes show a compact property line (tags, created) and light formatting: larger bold headings, bullets, ☐/☑ checkboxes, bold/italic, and `[[links]]`/`#tags` in the accent colour; any `.md` file is shown this way, even a loose file outside the vault. Text and code files on the allow-list show as plain monospaced text, and a folder lists its first 20 items. Loading starts 80 ms after the selection settles and reads at most 64 KB on a background thread; the panel shows the start of the file, with a "Preview shows the start of the file" notice whenever the shown text is cut. Images (`.png .jpg .jpeg .gif .bmp .webp`) show a thumbnail from the Windows thumbnail cache, plus the file name, dimensions, and size below it. A long file path wraps onto up to three lines under the title instead of being cut off. Online-only OneDrive or Dropbox files show "Not downloaded - open to download" and are never downloaded. Scroll the panel with the mouse wheel or Shift+PgUp/PgDn; plain PgUp/PgDn still move through the results. The window grows from 750 to about 1,170 px to fit the 420 px panel, or falls back to an overlay over the results on a narrow screen. Whether the panel is open is remembered between launches, but that state isn't included in a settings export. The "Preview panel" toggle and its shortcut (default `Ctrl+P`, configurable and conflict-checked like `Ctrl+K`) are in Settings > Search; turning the toggle off, or hiding the launcher, frees the preview worker, buffers, layouts, and thumbnail bitmaps.

## [1.8.0] - 2026-09-24

### Added
- **Pomodoro timer** - `pomo` starts a 25-minute focus timer (`pomo 50 write intro` sets the length and a label, `pomo break` starts a 5-minute break, `pomo stop` stops it). While it runs, the tray tooltip and the launcher footer show the minutes left. At the end a notification appears; clicking it after a focus timer starts a break. Finished focus timers are added to your log heading (the same note as `l`) as `- HH:MM: 🍅 25 min - write intro`, but only if that note already exists - the timer never creates a daily note and never starts Obsidian. Timers stopped early, breaks, and timers that ended while the PC was asleep aren't logged. A running timer survives a restart of Lean Launcher. Starting a new timer while one runs needs a second `Enter`. The prefix, focus and break lengths, and logging are set in Settings > Search.
- **Time zones** - `time in Tokyo` shows the current time there; `10am PST in CET`, `15:00 London in New York`, and `3pm in Berlin` (from your own time zone) convert a time. Daylight saving is applied for the actual date using Windows' own time zone rules, a time that doesn't exist or happens twice on a clock-change day is flagged, and ambiguous abbreviations are read one fixed way and say so (IST = India, CST = US Central, BST = British Summer Time). A time needs am/pm or a colon, so ordinary searches aren't mistaken for times. Turn it off with "Time zone converter" in Settings > Search.
- **Unit converter** - type `5 km in mi`, `72 f to c`, `3 GB in GiB`, `2.5 l in cups`, `100 kmh in mph`, or `180 lb to kg` (also with `to`, `->`, or `=`); the number part can be a calculation, like `(3+2) km in mi`. `Enter` copies the result; the actions menu also copies `5 km = 3.107 mi`. Length, mass, volume, temperature, data sizes (KB = 1000 bytes, KiB = 1024), speed, area, and time are supported; gallons, pints, cups, and fluid ounces are US units unless you type `imp gal` and so on. A decimal comma like `2,5 km` works when Windows uses one. Turn it off with "Unit converter" in Settings > Search.
- **Export and import settings** - Settings > About has a new BACKUP card. "Export settings..." saves your preferences to a `.json` file (hotkeys, toggles, prefixes, labels, capture targets, web search engine, daily-note overrides); ticking a box also includes your vault path, pins, or file-search exclusions. "Import settings..." checks every value with the same rules as the Settings screen, shows what will change and what was skipped (for example a hotkey that's taken or a target note outside the vault), and only applies after you confirm. Your current settings are saved to `%LOCALAPPDATA%\LeanLauncher\settings-before-import.json` first, so you can import that file to undo. A settings file never contains recent apps, and importing never turns on run-at-startup.
- **Path completion** - type a path like `C:\Us`, `%APPDATA%\Mi`, `~\Doc`, or `\\server\share\` and the results switch to that folder's contents (folders first, then files; hidden and system items only when you type a leading `.`). `Tab` completes the selected entry (outside a path, `Tab` still moves the selection), `Enter` opens it, and `Shift+Enter` opens the typed path itself. Folders are read in the background, so a slow or unreachable network share never freezes the launcher: it shows "Looking up \\server..." and, after 10 seconds, "Can't reach \\server". Very large folders show their first 2,000 entries. Turn it off with "Path completion" in Settings > Search; nothing is kept in memory once the launcher closes.
- **Open typed URLs** - type `https://...`, `http://...`, or `www...` and the first row opens it in your browser; a bare address like `github.com/sdkasper` gets an "Open URL" row too, placed below the top match so a file such as `readme.md` still comes first. Only http and https are ever opened - typing `javascript:`, `file:`, or `ms-settings:` never gives a URL row. No URL history is kept. Turn it off with "Typed URLs" in Settings > Search.
- **System commands** - `lock`, `sleep`, `hibernate`, `restart`, `shut down`, `sign out`, and `empty recycle bin` run straight from the launcher, and `s ` lists them all (`s re` narrows; the prefix is configurable). An exact name or alias beats settings pages, so `lock` locks rather than opening Lock Screen settings. Restart, shut down, sign out, and Empty Recycle Bin ask "Press Enter again to ..."; the prompt clears when you type, move the selection, close the launcher, or after 5 seconds, and `Ctrl+Enter`, `Alt+1`-`8`, and the actions menu can't skip it. Restart and shut down use Windows' `shutdown.exe` without forcing apps closed, so they can still ask you to save work. The Empty Recycle Bin row shows its item count and size and is greyed out when the bin is empty. Commands never appear on the empty search box, can't be pinned, and don't enter recent items. A "System commands" switch in Settings > Search turns them off.

### Changed
- **Reset to defaults restarts file search** - resetting now also turns file search back on and restarts its index (and stops the note index) right away, instead of leaving the old state running until the next restart.
- **Turning File search off frees its memory** - the file index, its folder watchers, and the periodic rescan now stop as soon as File search is switched off, instead of running until the next restart; switching it back on reloads the saved index. With File search off, the index is no longer built at startup at all. On a test machine this is about 58 MB.
- **Settings keyboard navigation follows the screen order** - Up/Down in Settings now moves through rows in the order they're shown, including the capture target and quick-open rows in the Obsidian section.

## [1.7.0] - 2026-09-24

### Added
- **Multi-line captures** - `\n` or ` // ` in a task (`t`), note (`a`), or log (`l`) capture starts a new line, e.g. `t buy milk // oat, 1 litre`. Extra lines of a task or log entry are indented two spaces under its bullet so Obsidian keeps it one list item (blank extra lines are dropped); note captures keep each line, including blank ones in the middle. The capture preview shows each break as `⏎`. `\\n` writes a literal `\n`; every other backslash is kept as typed. A capture that is only line breaks is treated as empty.
- **Check for updates from the About tab** - Settings > About has a new UPDATES card with a "Check for updates" row showing when the last check ran. Clicking it checks GitHub right away, even with automatic update checks turned off, and downloads a newer release in the background; the row then reads "vX.Y.Z ready - Restart to update" and a second click installs it and restarts. A check that can't reach GitHub now says so ("Check failed - try again") instead of looking like "up to date". A failed later check keeps offering an update that is already downloaded or available.

### Security
- **Stricter checks on daily-note and capture target paths** - the daily-note folder, format, and capture target notes now also reject `:` (drive-relative paths and hidden NTFS streams), reserved Windows device names such as `CON` or `NUL`, and path parts made only of dots and spaces, which Windows would treat as `..`. The date format is checked after it is expanded, so `[literal text]` can't point a capture outside the vault. Anything unsafe falls back to `YYYY-MM-DD` inside the vault, as before.
- The "open release page" link in About only opens this repository's GitHub pages; any other URL in the release data opens the default releases page instead.

### Fixed
- **Captures went into a new, bare daily note when the daily-note format used month or weekday names** - a format like `YYYY/MM-MMMM/YYYY-MM-DD-dddd` wasn't understood, so Lean Launcher fell back to `YYYY-MM-DD` and wrote to a different file than the one Obsidian opens. Daily-note formats (and the format override) now support `YYYY YY MMMM MMM MM M DD D dddd ddd` and `[literal text]`, with English month and weekday names. Formats with other tokens (for example week numbers), or with an unclosed `[`, still fall back to `YYYY-MM-DD`. Lean Launcher still doesn't apply the daily-note template when it creates today's note itself; open today's note in Obsidian first (or turn on Daily notes' "Open daily note on startup") so captures land in the template-based note.
- **Pasted multi-line text was glued into one line** (`line1line2`) - the search box dropped the line breaks. Pasted line breaks now become `\n`, so a capture writes them as real lines; breaks at the start and end of the pasted text are dropped, so pasting a single copied line into a normal search is unaffected.
- **File search was about 3x over its 20ms latency budget on large drives** (listed under 1.5.0's Known Limitations). Name matching allocated two strings for nearly every indexed file on every keystroke, and nothing skipped names that couldn't match. Name scoring no longer allocates, each indexed name carries a character summary that rules out impossible matches before scoring, and search stops collecting results that can't make the top of the list. On a synthetic 500,000-file index a typical name query dropped from ~67ms to ~13ms, and a query that matches every file runs in ~19ms. Scores are unchanged - a new test checks the new scorer against a frozen copy of the old one over 26,820 name/query pairs.

## [1.6.1] - 2026-09-23

### Fixed
- **Windows Defender quarantined Lean Launcher as `Behavior:Win32/Persistence.A!ml`** - on every launch the app silently (re)wrote its own `HKCU\...\CurrentVersion\Run` startup entry whenever it didn't match the running exe's path. For a new, unsigned executable that self-registration is exactly what Defender's behavior model treats as malware persistence, and it quarantined the exe within seconds of starting - with or without an update in progress. Lean Launcher now never writes the startup entry on launch; it only reads it, to show the **Run at startup** toggle's real state, and writes it solely when you turn the toggle on in Settings.

### Changed
- **Run at startup is now off by default** for new installs, and **Reset to defaults** turns it off. Existing installs keep a working startup entry as long as it points at the same exe; if it points elsewhere (for example after moving the exe), the toggle shows off until you turn it on again.

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
