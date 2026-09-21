# File Search

Lean Launcher indexes files and folders across your whole disk so you can find them instantly from the launcher. Two layers keep that index relevant and safe:

## Built-in exclusions

Lean Launcher never indexes:

- Windows system folders (`C:\Windows`, `system32`, `WinSxS`, driver stores, and similar)
- Build/dependency folders (`node_modules`, `bin`, `obj`, `dist`, `.git`, `venv`, and similar)
- Files whose extension isn't on the built-in allowlist (documents, media, code, archives, and a handful of extensionless files like `README` and `Makefile` - not every file type on disk)

These rules are fixed and cannot be turned off or narrowed from the exclusions file below - they exist to keep search results relevant and to avoid indexing files Windows itself depends on.

## Your own exclusions

If you have a large personal archive, an unusual project layout, or a file type you never want to see in search results, add your own exclusions on top of the built-in list:

1. Open Settings → Search, and select **Edit exclusions...**. The first time you do this, Lean Launcher creates `%LOCALAPPDATA%\LeanLauncher\file_search_excludes.txt` with a short instructional header, then opens it in your default text editor.
2. Add one entry per line:
   - **Folder exclusion** - a full path starting with a drive letter, `\\` (a UNC network path), or `/`:
     ```
     D:\Personal Archive
     \\NAS\Backups
     ```
     Excludes that folder and everything under it.
   - **Extension exclusion** - a dot followed by the extension, nothing else:
     ```
     .iso
     ```
     Excludes every file with that extension, anywhere on disk.
3. Save the file. Your exclusions take effect on the next scan pass (Lean Launcher's normal startup scan or periodic background rescan) - no need to restart the app, though changes aren't applied the instant you save either.

Blank lines, lines starting with `#`, and any line that doesn't match one of the two shapes above are silently ignored.

This file is **additive only**: it can only exclude more, never re-include something the built-in rules already exclude. There's no way to make Lean Launcher index `system32`, no matter what you put in this file.
