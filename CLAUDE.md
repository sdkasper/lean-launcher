# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

Lean Launcher is a native Windows application launcher (Alt+Space, type, Enter), built as an independent fork of [Takeoff](https://github.com/akiraeng/takeoff-launcher). Pure C++17, Win32, Direct2D/DirectWrite — no Electron, no third-party UI frameworks. Design priorities: <500 KB binary, ~10 MB RAM, <1ms search, zero telemetry.

**Naming note:** the project is mid-rebrand from "Takeoff" to "Lean Launcher". CMake targets, the repo, and internal identifiers (`LeanLauncher.exe`, `kMutexName`, window title "Lean Launcher") use the new name; the README's `<h1>`, some comments, and `scripts/package.ps1` still say "Takeoff". Don't be surprised by the mismatch — prefer "Lean Launcher"/`LeanLauncher` in new code and treat leftover "Takeoff" references as not-yet-migrated rather than intentional.

## Build

```powershell
# CMake (primary)
cmake -S . -B build -A x64
cmake --build build --config Release
# Binary at build/Release/LeanLauncher.exe

# Or Visual Studio 2022: open LeanLauncher.sln, set Release/x64, Ctrl+Shift+B
```

Requires Windows 10 1809+ or Windows 11, VS2022 with the Desktop C++ workload, Windows SDK. Compiler flags: `/W4 /permissive- /utf-8`, C++17, `UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN`. Keep builds clean at `/W4` — CI enforces both the CMake build and the raw `msbuild LeanLauncher.sln` build.

## Test

```powershell
# Core regression tests (fuzzy matching, text navigation, hotkeys, recency, version compare)
ctest --test-dir build -C Release --output-on-failure
# or directly:
.\build\Release\LeanLauncherCoreTests.exe

# Run a single check: core_tests.cpp is one flat main() of sequential Check(condition, "label")
# assertions (see tests/core_tests.cpp) — there's no test-name filter, so isolate by
# temporarily commenting out other Check() calls, or grep the label to find the line.

# Interactive UI smoke test (window creation, acrylic, caret, selection, actions overlay)
py tests/ui_smoke.py build/Release/LeanLauncherUiTests.exe
```

`LeanLauncherUiTests` is a second executable built from the same sources (`add_launcher` in CMakeLists.txt is a function invoked twice) but compiled with `LEANLAUNCHER_UI_TEST` defined, giving it a separate window class/mutex and no global hotkey registration — it never attaches to or replaces a real running launcher instance. This is the mechanism to know about before adding anything that behaves differently under test vs. production; check `kUiTest` (from `LEANLAUNCHER_UI_TEST`) at the call site.

No linter is configured; `/W4 /permissive-` warnings-as-errors-in-spirit is the correctness bar.

## Architecture

Everything lives in `src/` as headers included by `main.cpp` (single translation unit); there is no `.cpp`/`.h` split for the app logic itself.

- **`main.cpp`** — `wWinMain` entry point, single-instance mutex handling (replaces a running instance via `kExitLauncherMessage` + process wait/terminate rather than allowing two instances), the app-scanning worker thread (`BuildAppIndex`, Start Menu + common install dirs via bounded recursive directory scan with skip-lists for noise dirs like `node_modules`/`temp`), and the `AppEntry`/`RankedResult` data model. Wraps `LauncherWindow` (from `launcher.h`).
- **`launcher.h`** — the `LauncherWindow` class (~3700 lines): owns the Win32 window, Direct2D render target, all UI drawing (`DrawSearch`, `DrawResults`, `DrawSettings`, `DrawActions`, `DrawHotkeyWarningModal`, etc.), input handling (`HandleKeyDown`, `HandleClick`, IME composition), settings UI state machine (row navigation, hotkey recording), and two background workers started from `Create()`: `IconWorkerMain` (icon extraction/caching) and `IndexWorkerMain` (file index rebuild triggering). This is the largest and most central file — read the relevant `Draw*`/`Handle*` method rather than the whole file when making UI changes.
- **`search.h`** — `Normalize`/`Condense` string prep and `MatchScore` fuzzy-matching (acronym, prefix, word-boundary, multi-token, condensed matching with weighted scoring — see `tests/core_tests.cpp` for the scoring contract, since it's easier to infer intent from the test expectations than the scoring code itself). Also `SearchInput` (text field state: selection, caret, IME) and Explorer PIDL helpers.
- **`file_index.h`** — `FileIndex` singleton: background-threaded, chunked (`IndexChunk`/`IndexSnapshot`, copy-on-write via `shared_ptr<const IndexChunk>`) file/folder index for file search, separate from the app index built in `main.cpp`.
- **`settings.h`** — `Settings` struct (hotkey bindings, startup/tray/update/search toggles) and hotkey serialization/naming helpers. Persisted via `LauncherWindow::LoadSettings`/`SaveSettings` in `launcher.h`.
- **`calculator.h`** — standalone expression lexer/parser (`Lexer`, `Parser`, `CalculationResult`) for inline math evaluation (`sqrt(144)`, `2^10`, etc.), no dependency on the rest of the app.
- **`obsidian_config.h`** / **`daily_note.h`** — minimal hand-rolled JSON field extraction (`ParseJsonStringAt`, not a general parser) to read Obsidian vault config (daily-notes folder/date format) and append entries to the current daily note; used by the `task <text>` quick-capture flow.
- **`updates.h`** — GitHub Releases version check over WinHTTP (`api.github.com`) and self-update/restart flow. Update checks are opt-in (`checkForUpdates = false` by default — "no release pipeline yet").

**Threading model:** the app index (`main.cpp::BuildAppIndex`), the file index (`FileIndex::WorkerLoop`), and icon extraction (`LauncherWindow::IconWorkerMain`) each run on their own background thread and hand results back to the UI thread via `PostMessageW` with custom `WM_APP + n` messages (`kAppsReadyMessage`, `kFilesReadyMessage`, `kIconReadyMessage`, etc., defined near the top of `main.cpp`). When touching any of these paths, follow the message flow rather than assuming direct calls across threads — nothing touches Direct2D/window state off the UI thread.

**Shell integration:** `SHChangeNotifyRegister` (in `LauncherWindow::Create`) watches for shell-level changes (new/removed Start Menu entries etc.) and triggers reindexing via `kShellNotifyMessage`; this is skipped under `kUiTest`.

## CI

`.github/workflows/ci.yml`: CMake configure → Release x64 build → `ctest` → a second `msbuild LeanLauncher.sln` build (as a fully independent verification that the `.sln`/`.vcxproj` hasn't drifted from `CMakeLists.txt`). Both build paths must stay in sync when adding new source files — update `CMakeLists.txt`'s `add_launcher` sources list *and* `LeanLauncher.vcxproj`.

## Design Philosophy (from CONTRIBUTING.md)

- Native and dependency-free: standard Win32/Direct2D/DirectWrite/C++17 only — no heavy third-party runtimes or frameworks.
- Instant response: keystroke handling and search must feel instantaneous (<5ms).
- Privacy first: zero telemetry/tracking; the only network access is the opt-in GitHub Releases check.
- Prefer RAII (`Microsoft::WRL::ComPtr` for COM, unique handles for Win32 primitives) over manual resource management.
- Naming: PascalCase types/functions, `kCamelCase`/`UPPER_SNAKE_CASE` constants, `camelCase`/`m_camelCase` members.
