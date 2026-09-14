# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- **Built-in Calculator**: Real-time evaluation of mathematical expressions directly within the launcher search bar (e.g. `125 * 8`, `(10 + 20) * 3`, `sqrt(144)`, `2^10`, `10 % 3`, `200 * 15%`).
- **Instant Result Display & Copy**: Top-ranked calculation result displayed with dedicated calculator badge, immediate `Enter` shortcut to copy the result and close, and `Ctrl+C` to copy without closing.
- **Calculator Actions**: Context actions (`Ctrl+K`) for copying the numeric result, copying the full calculation (`expression = result`), or opening Windows Calculator.

## [1.0.3] - 2026-09-12

### Added
- **Automatic Silent Update Downloading**: Releases are downloaded in the background from GitHub Releases using WinHTTP streaming with HTTP 302 cross-domain redirect following (GitHub to AWS S3).
- **Restart to Update**: Interactive "Restart to Update" button in the footer and tray menu once an update has been silently downloaded and validated.
- **Robust In-Place Executable Swap**: Atomic executable replacement with rollback protection, retry loops for transient antivirus scanner locks, and UAC elevation fallback for protected install locations.
- **PE Executable Verification**: Integrity check verifying DOS magic, `IMAGE_NT_SIGNATURE`, and x64 architecture before any update can be staged or installed.

## [1.0.2] - 2026-09-09

### Added
- **Broad File & Folder Search**: Comprehensive in-memory file search indexing primary user folders and all fixed/removable drives with deep directory traversal (up to depth 8) and up to 150,000 files.
- **Folder & Path-Aware Search**: Direct matching of directory names as first-class items, multi-token path queries (e.g. `takeoff main` or `X:/takeoff-launcher`), and path substring matching.
- **Scrollable Settings**: Direct2D primitive-clipped settings viewport preventing footer overlap, with smooth mouse wheel scrolling, keyboard navigation, and custom scrollbar indicator.

## [1.0.0] - 2026-09-09

### Added
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
