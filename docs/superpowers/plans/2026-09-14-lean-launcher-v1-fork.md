# Lean Launcher v1 (Quick Task Add) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fork `akiraeng/takeoff-launcher` into an independent "Lean Launcher" repo and ship v1's one feature: typing `task <text>` and pressing Enter appends `- [ ] <text>` to today's Obsidian daily note, with no Obsidian window ever opening or focusing.

**Architecture:** Direct file append (no Obsidian process involved) using vault/daily-note config auto-detected from Obsidian's own `obsidian.json` and `daily-notes.json`. Two new header-only modules (`obsidian_config.h`, `daily_note.h`) plug into the existing `AppCategory`-driven result pipeline the same way the existing Calculator inline-eval feature does (a synthetic result row is inserted in `UpdateResults()`, then special-cased in `LaunchSelected()`/`RunAction()`).

**Tech Stack:** C++17, Win32 + Direct2D/DirectWrite (no Electron), CMake 3.21+, WinHTTP (updater only, disabled by default in v1), Windows Registry for config.

**Spec:** `D:\Lean Notes\01 Projects\LP Products\Lean Launcher\20 Architecture.md`

## Global Constraints

- Independent repo, fresh git history, no upstream tracking. MIT `LICENSE` retained as-is; README/NOTICE credits `akiraeng/takeoff-launcher` as upstream base.
- v1 scope is quick task-add only. Do not build: theme switch, plugin enable/disable, vault-stats novelty result, multi-vault support, template-aware note creation, conflict detection for notes open with unsaved edits. These are explicitly backlog.
- Rebrand identity (exact values, from the spec): window class `LeanLauncherWindow`, mutex `Local\LeanLauncher.SingleInstance`, registry root `HKCU\Software\LeanLauncher`, Start Menu shortcut `Lean Launcher.lnk`.
- Auto-updater is repointed at Lean Launcher's own (future) GitHub releases and **disabled by default** until a real release pipeline exists.
- Internal C++ namespace `takeoff` (and its `quicklaunch` alias) is **not** renamed — it's an implementation-internal detail, not part of the spec's rebrand list, and touches ~50+ call sites across `launcher.h` for zero user-visible benefit. Do not rename it.
- Write mechanism is direct file append via `CreateFileW`/`WriteFile` — never through an Obsidian URI or the Advanced URI plugin (rejected alternatives, see spec).
- All new pure-function logic (date-token formatting, JSON field extraction, task-line construction, task-prefix parsing) must be unit-testable without touching a real vault, following the existing `tests/core_tests.cpp` pattern (plain `Check(condition, description)` assertions, no test framework).

---

### Task 1: Fork the source into an independent repo with renamed identity files

**Files:**
- Create (copy from scratchpad clone): entire tree from `C:\Users\Sascha\AppData\Local\Temp\claude\D--Lean-Notes\e00954c3-91fb-4833-a108-b1d23d4db1bf\scratchpad\takeoff-launcher` (re-clone from `https://github.com/akiraeng/takeoff-launcher.git` into a scratch dir first if that path no longer exists) into `D:\GitProjects\lean-launcher`, excluding `.git`.
- Rename: `src/Takeoff.rc` → `src/LeanLauncher.rc`
- Rename: `src/Takeoff.manifest` → `src/LeanLauncher.manifest`
- Rename: `Takeoff.sln` → `LeanLauncher.sln`
- Rename: `Takeoff.vcxproj` → `LeanLauncher.vcxproj`
- Modify: `CMakeLists.txt`
- Modify: `.github/workflows/ci.yml`
- Modify: `.github/workflows/release.yml`
- Modify: `README.md` (attribution notice)
- Keep as-is: `LICENSE` (MIT, unmodified, per spec)

**Interfaces:**
- Produces: a git repo at `D:\GitProjects\lean-launcher` with one initial commit, CMake target names `LeanLauncher`, `LeanLauncherCoreTests`, `LeanLauncherUiTests`, and a build that still behaves exactly like stock Takeoff (identity/rebrand behavior changes happen in Task 2, not here).

- [ ] **Step 1: Copy the source tree**

```bash
mkdir -p "D:/GitProjects/lean-launcher"
SRC="C:/Users/Sascha/AppData/Local/Temp/claude/D--Lean-Notes/e00954c3-91fb-4833-a108-b1d23d4db1bf/scratchpad/takeoff-launcher"
# If $SRC no longer exists, re-clone first:
#   git clone https://github.com/akiraeng/takeoff-launcher.git "$SRC"
cp -r "$SRC"/. "D:/GitProjects/lean-launcher/"
rm -rf "D:/GitProjects/lean-launcher/.git"
```

The `docs/superpowers/plans/` folder (this file) already exists in the destination from before the copy — verify the copy did not overwrite it (it won't; `cp -r` only adds/overwrites source-tree paths).

- [ ] **Step 2: Rename the four identity files**

```bash
cd "D:/GitProjects/lean-launcher"
git mv 2>/dev/null || true  # no-op; not yet a git repo, use plain mv
mv src/Takeoff.rc src/LeanLauncher.rc
mv src/Takeoff.manifest src/LeanLauncher.manifest
mv Takeoff.sln LeanLauncher.sln
mv Takeoff.vcxproj LeanLauncher.vcxproj
```

- [ ] **Step 3: Update `LeanLauncher.vcxproj` and `LeanLauncher.sln` internal references**

Open `LeanLauncher.vcxproj` and `LeanLauncher.sln`; both contain literal references to `Takeoff.rc`, `Takeoff.manifest`, and project name `Takeoff`. Replace every occurrence of `Takeoff` with `LeanLauncher` (project name, GUID labels are untouched, only text names). Confirm with:

```bash
grep -n "Takeoff" LeanLauncher.vcxproj LeanLauncher.sln
```

Expected: no output (all replaced).

- [ ] **Step 4: Update `CMakeLists.txt`**

Replace:
```cmake
project(Takeoff VERSION 1.0.3 LANGUAGES CXX)
```
with:
```cmake
project(LeanLauncher VERSION 0.1.0 LANGUAGES CXX)
```

Replace the source list's manifest/rc references:
```cmake
    src/Takeoff.manifest
    src/Takeoff.rc
```
with:
```cmake
    src/LeanLauncher.manifest
    src/LeanLauncher.rc
```

Replace:
```cmake
add_launcher(Takeoff)
```
with:
```cmake
add_launcher(LeanLauncher)
```

Replace the test block:
```cmake
    add_executable(TakeoffCoreTests tests/core_tests.cpp)
    target_compile_features(TakeoffCoreTests PRIVATE cxx_std_17)
    target_compile_options(TakeoffCoreTests PRIVATE /W4 /permissive- /utf-8)
    target_link_libraries(TakeoffCoreTests PRIVATE winhttp ole32 shell32 shlwapi)
    add_test(NAME TakeoffCoreTests COMMAND TakeoffCoreTests)

    # Separate class, mutex, fixture apps, and no global shortcut. Never attaches
    # to or replaces a user's running launcher.
    add_launcher(TakeoffUiTests)
    target_compile_definitions(TakeoffUiTests PRIVATE TAKEOFF_UI_TEST)
```
with:
```cmake
    add_executable(LeanLauncherCoreTests tests/core_tests.cpp)
    target_compile_features(LeanLauncherCoreTests PRIVATE cxx_std_17)
    target_compile_options(LeanLauncherCoreTests PRIVATE /W4 /permissive- /utf-8)
    target_link_libraries(LeanLauncherCoreTests PRIVATE winhttp ole32 shell32 shlwapi)
    add_test(NAME LeanLauncherCoreTests COMMAND LeanLauncherCoreTests)

    # Separate class, mutex, fixture apps, and no global shortcut. Never attaches
    # to or replaces a user's running launcher.
    add_launcher(LeanLauncherUiTests)
    target_compile_definitions(LeanLauncherUiTests PRIVATE LEANLAUNCHER_UI_TEST)
```

- [ ] **Step 5: Update CI workflows**

In `.github/workflows/ci.yml`, replace:
```yaml
      - name: Verify Visual Studio Solution Build
        run: msbuild Takeoff.sln /p:Configuration=Release /p:Platform=x64 /v:m
```
with:
```yaml
      - name: Verify Visual Studio Solution Build
        run: msbuild LeanLauncher.sln /p:Configuration=Release /p:Platform=x64 /v:m
```

In `.github/workflows/release.yml`, replace every `Takeoff` occurrence: target name (`--target Takeoff`, `--target TakeoffCoreTests`), exe filenames (`Takeoff.exe`, `Takeoff-$tag.exe`, `Takeoff-$tag-windows-x64.zip`) with the `LeanLauncher` equivalents (`LeanLauncher.exe`, `LeanLauncher-$tag.exe`, `LeanLauncher-$tag-windows-x64.zip`). Verify with:

```bash
grep -rn "Takeoff" .github/workflows/
```

Expected: no output.

- [ ] **Step 6: Update `README.md` with upstream attribution**

Add a section near the top of `README.md` (after the title, before the rest of the existing content, which can otherwise stay as inherited documentation for now — full README rewrite is not v1 scope):

```markdown
> Lean Launcher is an independent fork of [Takeoff](https://github.com/akiraeng/takeoff-launcher)
> by akiraeng, distributed under the same MIT license. See `LICENSE` for the full license text.
```

- [ ] **Step 7: Verify the CMake build still configures and builds before rebranding behavior**

```bash
cd "D:/GitProjects/lean-launcher"
cmake -S . -B build -A x64
cmake --build build --config Release --target LeanLauncher
```

Expected: build succeeds (it will still behave identically to stock Takeoff at runtime — window class, mutex, registry path, etc. are unchanged until Task 2).

- [ ] **Step 8: Init git and make the initial commit**

```bash
cd "D:/GitProjects/lean-launcher"
git init
git add -A
git commit -m "Initial fork of akiraeng/takeoff-launcher as Lean Launcher

Independent repo, fresh history, MIT license retained with upstream
attribution. Identity files renamed (Takeoff.rc/.manifest/.sln/.vcxproj
-> LeanLauncher equivalents); CMake/CI target names updated. Runtime
identity (window class, mutex, registry root, window title, updater
endpoints) is unchanged in this commit and is rebranded in the next."
```

---

### Task 2: Rebrand pass — runtime identity and updater endpoints

**Files:**
- Modify: `src/main.cpp`
- Modify: `src/launcher.h`
- Modify: `src/settings.h`
- Modify: `src/updates.h`
- Modify: `tests/core_tests.cpp`

**Interfaces:**
- Consumes: nothing new (pure rename/constant-value changes to existing code from Task 1).
- Produces: `kWindowClass = L"LeanLauncherWindow"`, `kMutexName = L"Local\\LeanLauncher.SingleInstance"`, `kSettingsRegistryPath = L"Software\\LeanLauncher"`, `Settings::checkForUpdates` defaults to `false`. These exact identifiers are consumed by Task 5/6 when they read/write the same registry key for the vault path.

- [ ] **Step 1: Rename window class, mutex, and UI-test macro in `src/main.cpp`**

Replace:
```cpp
#if defined(TAKEOFF_UI_TEST) || defined(QUICKLAUNCH_UI_TEST)
constexpr bool kUiTest = true;
constexpr wchar_t kWindowClass[] = L"TakeoffTestWindow";
constexpr wchar_t kMutexName[] = L"Local\\Takeoff.UiTest";
#else
constexpr bool kUiTest = false;
constexpr wchar_t kWindowClass[] = L"TakeoffWindow";
constexpr wchar_t kMutexName[] = L"Local\\Takeoff.SingleInstance";
#endif
```
with:
```cpp
#if defined(LEANLAUNCHER_UI_TEST)
constexpr bool kUiTest = true;
constexpr wchar_t kWindowClass[] = L"LeanLauncherTestWindow";
constexpr wchar_t kMutexName[] = L"Local\\LeanLauncher.UiTest";
#else
constexpr bool kUiTest = false;
constexpr wchar_t kWindowClass[] = L"LeanLauncherWindow";
constexpr wchar_t kMutexName[] = L"Local\\LeanLauncher.SingleInstance";
#endif
```

- [ ] **Step 2: Rename window title, registry root, startup value name, and drop the legacy `QuickLaunch` registry fallback in `src/launcher.h`**

Replace:
```cpp
        hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            kWindowClass, L"Takeoff", WS_POPUP | WS_THICKFRAME,
```
with:
```cpp
        hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            kWindowClass, L"Lean Launcher", WS_POPUP | WS_THICKFRAME,
```

Replace:
```cpp
    static constexpr wchar_t kSettingsRegistryPath[] = L"Software\\Takeoff";
    static constexpr wchar_t kStartupRegistryPath[] =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    static constexpr wchar_t kStartupValueName[] = L"Takeoff";
```
with:
```cpp
    static constexpr wchar_t kSettingsRegistryPath[] = L"Software\\LeanLauncher";
    static constexpr wchar_t kStartupRegistryPath[] =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    static constexpr wchar_t kStartupValueName[] = L"LeanLauncher";
```

In `LoadSettings()`, replace the two-path lookup:
```cpp
            HKEY key = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsRegistryPath, 0, KEY_READ, &key) ==
                    ERROR_SUCCESS ||
                RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\QuickLaunch", 0, KEY_READ, &key) ==
                    ERROR_SUCCESS) {
```
with the single new-path lookup (no legacy fallback — this is a fresh product with no prior installs to migrate from):
```cpp
            HKEY key = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsRegistryPath, 0, KEY_READ, &key) ==
                    ERROR_SUCCESS) {
```

Replace the tray tooltip:
```cpp
            wcscpy_s(data.szTip, L"Takeoff");
```
with:
```cpp
            wcscpy_s(data.szTip, L"Lean Launcher");
```

Replace the Start Menu shortcut name and description:
```cpp
        std::wstring shortcutPath = std::wstring(programsPath) + L"\\Takeoff.lnk";
```
with:
```cpp
        std::wstring shortcutPath = std::wstring(programsPath) + L"\\Lean Launcher.lnk";
```
and:
```cpp
        shellLink->SetDescription(L"Takeoff App Launcher");
```
with:
```cpp
        shellLink->SetDescription(L"Lean Launcher - Quick Obsidian Task Capture");
```

- [ ] **Step 3: Disable update checking by default in `src/settings.h`**

Replace:
```cpp
    bool checkForUpdates = true;
```
with:
```cpp
    bool checkForUpdates = false;  // No release pipeline yet; user can opt in via Settings.
```

- [ ] **Step 4: Repoint the updater at Lean Launcher's own (future) releases in `src/updates.h`**

Replace:
```cpp
inline constexpr wchar_t kAppVersion[] = L"1.0.3";
inline constexpr wchar_t kDefaultReleasesUrl[] = L"https://github.com/akiraeng/takeoff-launcher/releases";
inline constexpr wchar_t kDefaultApiHost[] = L"api.github.com";
inline constexpr wchar_t kDefaultApiPath[] = L"/repos/akiraeng/takeoff-launcher/releases/latest";
```
with:
```cpp
inline constexpr wchar_t kAppVersion[] = L"0.1.0";
inline constexpr wchar_t kDefaultReleasesUrl[] = L"https://github.com/sdkasper/lean-launcher/releases";
inline constexpr wchar_t kDefaultApiHost[] = L"api.github.com";
inline constexpr wchar_t kDefaultApiPath[] = L"/repos/sdkasper/lean-launcher/releases/latest";
```

Replace the default asset name in `ExtractAssetDownloadUrl`:
```cpp
inline std::wstring ExtractAssetDownloadUrl(std::string_view json, std::wstring_view tag,
                                           std::wstring_view preferredName = L"Takeoff.exe") {
```
with:
```cpp
inline std::wstring ExtractAssetDownloadUrl(std::string_view json, std::wstring_view tag,
                                           std::wstring_view preferredName = L"LeanLauncher.exe") {
```
and its fallback-URL construction:
```cpp
        std::wstring fallback = L"https://github.com/akiraeng/takeoff-launcher/releases/download/";
```
with:
```cpp
        std::wstring fallback = L"https://github.com/sdkasper/lean-launcher/releases/download/";
```

Replace both WinHTTP user-agent strings (`DownloadUpdateFile` and `QueryLatestReleaseInfo`):
```cpp
    HINTERNET session = WinHttpOpen(L"Takeoff-Launcher/1.0",
```
with (both occurrences):
```cpp
    HINTERNET session = WinHttpOpen(L"LeanLauncher/0.1",
```

Replace the staging-folder and backup-file naming in `GetUpdateStagingPath`, `RestartToUpdate`-adjacent code (`main.cpp`'s `RestartToUpdate`... actually this constant lives only in `updates.h`/`launcher.h`), `ValidateExecutableFile`'s comment, and `CleanupOldUpdates`:
```cpp
        std::filesystem::path dir = std::filesystem::path(localAppData) / L"Takeoff" / L"updates";
```
→
```cpp
        std::filesystem::path dir = std::filesystem::path(localAppData) / L"LeanLauncher" / L"updates";
```
(two occurrences: `GetUpdateStagingPath`'s `%LOCALAPPDATA%` branch, keep the `%TEMP%\Takeoff` fallback branch's directory also renamed to `%TEMP%\LeanLauncher`), and:
```cpp
        std::filesystem::path updateDir = std::filesystem::path(localAppData) / L"Takeoff" / L"updates";
```
in `launcher.h`'s `RestartToUpdate()` → same `LeanLauncher` rename.

Filename prefix `L"Takeoff_"` → `L"LeanLauncher_"`, and `L"Takeoff_update.exe"` → `L"LeanLauncher_update.exe"`.

Leave the live-GitHub-query integration test in `tests/core_tests.cpp` (the block querying `api.github.com/repos/akiraeng/takeoff-launcher/releases/latest` directly, around the `QueryLatestReleaseInfo(L"api.github.com", ...)` call) **unchanged** — it is a live smoke test of the generic WinHTTP download/PE-validation pipeline against a real, stable public repo, not an assertion about Lean Launcher's own (nonexistent-yet) releases.

- [ ] **Step 5: Update the now-flipped default-settings test in `tests/core_tests.cpp`**

Replace:
```cpp
    Check(settings.checkForUpdates,
        "automatic update checking enabled by default");
```
with:
```cpp
    Check(!settings.checkForUpdates,
        "automatic update checking disabled by default (no release pipeline yet)");
```

- [ ] **Step 6: Build and run the full test suite**

```bash
cd "D:/GitProjects/lean-launcher"
cmake --build build --config Release --target LeanLauncherCoreTests
ctest --test-dir build -C Release --output-on-failure -R LeanLauncherCoreTests
```

Expected: all checks pass, including the flipped `checkForUpdates` assertion.

- [ ] **Step 7: Confirm no stray "Takeoff" identity strings remain in the five rebranded categories**

```bash
grep -rn "TakeoffWindow\|Takeoff\.SingleInstance\|Software\\\\Takeoff\|Takeoff\.lnk\|akiraeng/takeoff-launcher" src/ tests/ CMakeLists.txt
```

Expected: no output (the one intentional exception — the live-test URL and the `README.md`/`LICENSE` attribution — live outside `src/`/`CMakeLists.txt` and are excluded from this grep by design).

- [ ] **Step 8: Commit**

```bash
git add -A
git commit -m "Rebrand runtime identity: window class, mutex, registry root, shortcut, and updater endpoints

Window class -> LeanLauncherWindow, mutex -> Local\\LeanLauncher.SingleInstance,
registry root -> HKCU\\Software\\LeanLauncher, Start Menu shortcut -> Lean
Launcher.lnk. Auto-updater repointed at sdkasper/lean-launcher and disabled
by default until a real release pipeline exists."
```

---

### Task 3: `obsidian_config.h` — vault discovery and daily-note config

**Files:**
- Create: `src/obsidian_config.h`
- Modify: `tests/core_tests.cpp` (add tests)
- Modify: `CMakeLists.txt` (add new header to the `add_launcher` source list so it shows up in IDEs; header-only, no separate compilation unit needed)

**Interfaces:**
- Produces: `leanlauncher::obsidian::VaultInfo` is *not* needed (v1 only needs paths) — produces `std::vector<std::wstring> leanlauncher::obsidian::FindKnownVaults()`, `struct leanlauncher::obsidian::DailyNoteConfig { std::wstring folder; std::wstring format = L"YYYY-MM-DD"; bool found = false; }`, `leanlauncher::obsidian::DailyNoteConfig leanlauncher::obsidian::ReadDailyNoteConfig(const std::wstring& vaultPath)`, plus the pure helpers `leanlauncher::obsidian::ParseJsonStringAt`, `leanlauncher::obsidian::ExtractStringField`, `leanlauncher::obsidian::FindVaultPathsInJson`, `leanlauncher::obsidian::ParseDailyNoteConfigJson` used by Task 4/5/6.

- [ ] **Step 1: Write the failing tests for the pure JSON-parsing helpers**

Add near the top of `tests/core_tests.cpp` (after the existing `#include` list, add `#include "../src/obsidian_config.h"`), inside `main()` after the existing calculator tests block (before the final `std::cout << "All search..."` line):

```cpp
    // --- Obsidian config parsing tests (pure functions, no filesystem) ---
    using namespace leanlauncher::obsidian;

    {
        std::wstring value;
        Check(ExtractStringField("{\"folder\":\"06 BJ/10 Daily\"}", "folder", value) &&
              value == L"06 BJ/10 Daily", "ExtractStringField finds folder value");
        Check(ExtractStringField("{\"format\":\"YYYY/MM/YYYY-MM-DD\"}", "format", value) &&
              value == L"YYYY/MM/YYYY-MM-DD", "ExtractStringField finds format value");
        Check(!ExtractStringField("{\"other\":\"x\"}", "folder", value),
              "ExtractStringField returns false when key is absent");
        Check(ExtractStringField("{\"path\":\"C:\\\\Users\\\\Sascha\\\\Lean Notes\"}", "path", value) &&
              value == L"C:\\Users\\Sascha\\Lean Notes",
              "ExtractStringField unescapes backslashes in Windows paths");
    }

    {
        const std::string obsidianJson =
            "{\"vaults\":{"
            "\"a1b2\":{\"path\":\"C:\\\\Users\\\\Sascha\\\\Lean Notes\",\"ts\":1,\"open\":true},"
            "\"c3d4\":{\"path\":\"D:\\\\Work\\\\Notes\",\"ts\":2}"
            "}}";
        auto paths = FindVaultPathsInJson(obsidianJson);
        Check(paths.size() == 2, "FindVaultPathsInJson finds both vault paths");
        Check(paths[0] == L"C:\\Users\\Sascha\\Lean Notes", "first vault path matches");
        Check(paths[1] == L"D:\\Work\\Notes", "second vault path matches");
        Check(FindVaultPathsInJson("{}").empty(), "FindVaultPathsInJson returns empty for no vaults key");
    }

    {
        const std::string dailyNotesJson = "{\"folder\":\"06 BJ/10 Daily\",\"format\":\"YYYY/MM/YYYY-MM-DD\"}";
        DailyNoteConfig config = ParseDailyNoteConfigJson(dailyNotesJson);
        Check(config.found, "ParseDailyNoteConfigJson marks config as found");
        Check(config.folder == L"06 BJ/10 Daily", "ParseDailyNoteConfigJson reads folder");
        Check(config.format == L"YYYY/MM/YYYY-MM-DD", "ParseDailyNoteConfigJson reads format");

        DailyNoteConfig missingFolder = ParseDailyNoteConfigJson("{\"format\":\"YYYY-MM-DD\"}");
        Check(missingFolder.found && missingFolder.folder.empty(),
              "ParseDailyNoteConfigJson tolerates a missing folder (vault-root notes)");

        DailyNoteConfig unparseable = ParseDailyNoteConfigJson("not json at all");
        Check(!unparseable.found, "ParseDailyNoteConfigJson reports not-found for garbage input");

        DailyNoteConfig empty = ParseDailyNoteConfigJson("{}");
        Check(!empty.found, "ParseDailyNoteConfigJson reports not-found for empty object");
    }
```

- [ ] **Step 2: Run the tests to verify they fail to compile (the header does not exist yet)**

```bash
cd "D:/GitProjects/lean-launcher"
cmake --build build --config Release --target LeanLauncherCoreTests
```

Expected: compile error, `obsidian_config.h: No such file or directory`.

- [ ] **Step 3: Write `src/obsidian_config.h`**

```cpp
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace leanlauncher {
namespace obsidian {

namespace fs = std::filesystem;

struct DailyNoteConfig {
    std::wstring folder;
    std::wstring format = L"YYYY-MM-DD";
    bool found = false;
};

// Reads a whole file as raw UTF-8 bytes. Returns an empty string if the file
// does not exist or cannot be read (never throws).
inline std::string ReadFileUtf8(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return {};
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

// Parses one JSON string value starting at `pos` (which must point just past
// the field's key, e.g. right after `"folder"`). Advances `pos` past the
// closing quote. Handles `\"` and `\\` escapes; anything else (unicode
// escapes) is passed through raw, which is fine for the plain paths and
// folder names Obsidian's own config files contain.
inline bool ParseJsonStringAt(std::string_view json, size_t& pos, std::wstring& out) {
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':' ||
           json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '"') return false;
    ++pos;
    std::string raw;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            raw.push_back(json[pos + 1]);
            pos += 2;
            continue;
        }
        raw.push_back(json[pos]);
        ++pos;
    }
    if (pos < json.size()) ++pos;  // consume closing quote
    if (raw.empty()) {
        out.clear();
        return true;
    }
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
    if (wlen <= 0) return false;
    out.assign(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, raw.data(), static_cast<int>(raw.size()), out.data(), wlen);
    return true;
}

// Finds the first `"key":"value"` occurrence anywhere in `json` and returns
// its decoded value. Not a real JSON parser (no nesting awareness) - matches
// this codebase's existing ad-hoc parsing style in updates.h.
inline bool ExtractStringField(std::string_view json, std::string_view key, std::wstring& out) {
    const std::string keyPattern = "\"" + std::string(key) + "\"";
    size_t pos = json.find(keyPattern);
    if (pos == std::string_view::npos) return false;
    pos += keyPattern.size();
    return ParseJsonStringAt(json, pos, out);
}

// Finds every `"path":"..."` value in obsidian.json's `vaults` object.
inline std::vector<std::wstring> FindVaultPathsInJson(std::string_view json) {
    std::vector<std::wstring> paths;
    const std::string key = "\"path\"";
    size_t pos = 0;
    while ((pos = json.find(key, pos)) != std::string_view::npos) {
        pos += key.size();
        std::wstring value;
        if (ParseJsonStringAt(json, pos, value) && !value.empty()) {
            paths.push_back(std::move(value));
        }
    }
    return paths;
}

// Parses a daily-notes.json-shaped object: {"folder": "...", "format": "..."}.
// `folder` may legitimately be absent (daily notes live at the vault root);
// `format` defaults to "YYYY-MM-DD" when absent. Returns found=false only
// when neither field is present (i.e. this isn't a daily-notes config at all).
inline DailyNoteConfig ParseDailyNoteConfigJson(std::string_view json) {
    DailyNoteConfig config;
    std::wstring folder;
    std::wstring format;
    const bool hasFolder = ExtractStringField(json, "folder", folder);
    const bool hasFormat = ExtractStringField(json, "format", format);
    if (!hasFolder && !hasFormat) {
        return config;  // found = false
    }
    config.folder = hasFolder ? folder : L"";
    config.format = (hasFormat && !format.empty()) ? format : L"YYYY-MM-DD";
    config.found = true;
    return config;
}

// Reads %APPDATA%\obsidian\obsidian.json and returns every known vault path
// that still exists on disk. Empty vector if Obsidian has never run, the
// config is unreadable, or no known vault paths still exist.
inline std::vector<std::wstring> FindKnownVaults() {
    PWSTR appDataPath = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, nullptr, &appDataPath)) ||
        !appDataPath) {
        return {};
    }
    const fs::path configPath = fs::path(appDataPath) / L"obsidian" / L"obsidian.json";
    CoTaskMemFree(appDataPath);

    const std::string json = ReadFileUtf8(configPath);
    if (json.empty()) return {};

    std::vector<std::wstring> existing;
    for (auto& path : FindVaultPathsInJson(json)) {
        std::error_code ec;
        if (fs::exists(path, ec)) existing.push_back(std::move(path));
    }
    return existing;
}

// Reads the daily-notes plugin config for a vault, falling back to the
// Periodic Notes plugin's "daily" section if the core plugin's config is
// missing or unparseable. Returns found=false if neither is available -
// callers should then fall back to vault root + "YYYY-MM-DD.md".
inline DailyNoteConfig ReadDailyNoteConfig(const std::wstring& vaultPath) {
    const fs::path base(vaultPath);

    std::string json = ReadFileUtf8(base / L".obsidian" / L"daily-notes.json");
    if (!json.empty()) {
        DailyNoteConfig config = ParseDailyNoteConfigJson(json);
        if (config.found) return config;
    }

    json = ReadFileUtf8(base / L".obsidian" / L"plugins" / L"periodic-notes" / L"data.json");
    if (!json.empty()) {
        const size_t dailyPos = json.find("\"daily\"");
        if (dailyPos != std::string::npos) {
            DailyNoteConfig config = ParseDailyNoteConfigJson(std::string_view(json).substr(dailyPos));
            if (config.found) return config;
        }
    }

    return DailyNoteConfig{};  // found = false
}

}  // namespace obsidian
}  // namespace leanlauncher
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build --config Release --target LeanLauncherCoreTests
ctest --test-dir build -C Release --output-on-failure -R LeanLauncherCoreTests
```

Expected: all new `ExtractStringField` / `FindVaultPathsInJson` / `ParseDailyNoteConfigJson` checks pass.

- [ ] **Step 5: Add the header to `CMakeLists.txt`'s source list (IDE visibility only)**

In the `add_launcher` function's `add_executable` call, add `src/obsidian_config.h` alongside the other headers:
```cmake
add_executable(${target} WIN32
    src/main.cpp
    src/launcher.h
    src/file_index.h
    src/search.h
    src/settings.h
    src/updates.h
    src/calculator.h
    src/obsidian_config.h
    src/LeanLauncher.manifest
    src/LeanLauncher.rc
)
```

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "Add obsidian_config.h: vault discovery and daily-note config parsing

FindKnownVaults() reads %APPDATA%\\obsidian\\obsidian.json for known vault
paths; ReadDailyNoteConfig() reads a vault's daily-notes.json (falling back
to the Periodic Notes plugin's config) for the daily note folder/format.
All JSON field extraction is pure and unit tested without touching disk."
```

---

### Task 4: `daily_note.h` — task-prefix parsing, path resolution, and file append

**Files:**
- Create: `src/daily_note.h`
- Modify: `tests/core_tests.cpp` (add tests)
- Modify: `CMakeLists.txt` (add header to source list)

**Interfaces:**
- Consumes: `leanlauncher::obsidian::DailyNoteConfig` from Task 3.
- Produces: `bool leanlauncher::obsidian::TryParseTaskPrefix(const std::wstring& input, std::wstring& outText)`, `std::wstring leanlauncher::obsidian::FormatDateTokens(const std::wstring& format, int year, int month, int day)`, `std::wstring leanlauncher::obsidian::ResolveTodayPath(const DailyNoteConfig& config, const std::wstring& vaultPath, int year, int month, int day)`, `void leanlauncher::obsidian::GetTodayYmd(int& year, int& month, int& day)`, `std::wstring leanlauncher::obsidian::BuildTaskLine(std::wstring_view text)`, `bool leanlauncher::obsidian::AppendTask(const std::wstring& notePath, std::wstring_view taskText)`. Task 5/6 call `TryParseTaskPrefix`, `GetTodayYmd` + `ResolveTodayPath`, and `AppendTask` directly.

- [ ] **Step 1: Write the failing tests**

Add to `tests/core_tests.cpp`, right after the obsidian_config tests block from Task 3 (add `#include "../src/daily_note.h"` to the includes):

```cpp
    // --- daily_note.h: pure-function tests ---
    Check(FormatDateTokens(L"YYYY-MM-DD", 2026, 9, 14) == L"2026-09-14",
        "FormatDateTokens basic YYYY-MM-DD");
    Check(FormatDateTokens(L"YYYY/MM/YYYY-MM-DD", 2026, 1, 5) == L"2026/01/2026-01-05",
        "FormatDateTokens repeated tokens across folder+filename format");
    Check(FormatDateTokens(L"YYYY", 2026, 9, 14) == L"2026", "FormatDateTokens year only");
    Check(FormatDateTokens(L"literal", 2026, 9, 14) == L"literal", "FormatDateTokens no tokens passes through");

    {
        DailyNoteConfig config;
        config.folder = L"06 BJ/10 Daily";
        config.format = L"YYYY/MM/YYYY-MM-DD";
        const std::wstring path = ResolveTodayPath(config, L"D:\\Vault", 2026, 9, 14);
        Check(path == L"D:\\Vault\\06 BJ/10 Daily\\2026/09/2026-09-14.md",
            "ResolveTodayPath joins vault + folder + formatted filename");
    }
    {
        DailyNoteConfig rootConfig;  // no folder: notes live at vault root
        rootConfig.format = L"YYYY-MM-DD";
        const std::wstring path = ResolveTodayPath(rootConfig, L"D:\\Vault", 2026, 9, 14);
        Check(path == L"D:\\Vault\\2026-09-14.md",
            "ResolveTodayPath with empty folder falls back to vault root");
    }

    {
        std::wstring text;
        Check(TryParseTaskPrefix(L"task buy milk", text) && text == L"buy milk",
            "TryParseTaskPrefix parses basic 'task <text>'");
        Check(TryParseTaskPrefix(L"Task buy milk", text) && text == L"buy milk",
            "TryParseTaskPrefix is case-insensitive on the prefix");
        Check(TryParseTaskPrefix(L"task   buy milk", text) && text == L"buy milk",
            "TryParseTaskPrefix trims extra spaces after the prefix");
        Check(!TryParseTaskPrefix(L"task", text), "TryParseTaskPrefix rejects bare 'task' with no text");
        Check(!TryParseTaskPrefix(L"task ", text), "TryParseTaskPrefix rejects 'task ' with only trailing space");
        Check(!TryParseTaskPrefix(L"tasker 5", text), "TryParseTaskPrefix requires a space after 'task'");
        Check(!TryParseTaskPrefix(L"notepad", text), "TryParseTaskPrefix rejects unrelated queries");
        Check(!TryParseTaskPrefix(L"", text), "TryParseTaskPrefix rejects empty input");
    }

    {
        Check(BuildTaskLine(L"buy milk") == L"- [ ] buy milk\n", "BuildTaskLine basic construction");
        Check(BuildTaskLine(L"line1\r\nline2") == L"- [ ] line1line2\n",
            "BuildTaskLine strips embedded CR/LF so one task never becomes two lines");
        Check(BuildTaskLine(L"") == L"- [ ] \n", "BuildTaskLine tolerates empty text");
        Check(BuildTaskLine(L"[[Some Note]] and #tag") == L"- [ ] [[Some Note]] and #tag\n",
            "BuildTaskLine passes through wikilinks and tags unescaped (valid Markdown as-is)");
    }
```

- [ ] **Step 2: Run the tests to verify they fail to compile**

```bash
cmake --build build --config Release --target LeanLauncherCoreTests
```

Expected: compile error, `daily_note.h: No such file or directory`.

- [ ] **Step 3: Write `src/daily_note.h`**

```cpp
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cwctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include "obsidian_config.h"

namespace leanlauncher {
namespace obsidian {

namespace fs = std::filesystem;

// Replaces YYYY/MM/DD tokens in a Moment.js-style date format string with
// zero-padded values. Only the three tokens Obsidian's daily-notes format
// actually needs for this codebase's vault conventions are supported;
// anything else in the format string passes through unchanged.
inline std::wstring FormatDateTokens(const std::wstring& format, int year, int month, int day) {
    std::wstring result;
    result.reserve(format.size());
    wchar_t buf[8];
    size_t i = 0;
    while (i < format.size()) {
        if (format.compare(i, 4, L"YYYY") == 0) {
            swprintf_s(buf, L"%04d", year);
            result += buf;
            i += 4;
        } else if (format.compare(i, 2, L"MM") == 0) {
            swprintf_s(buf, L"%02d", month);
            result += buf;
            i += 2;
        } else if (format.compare(i, 2, L"DD") == 0) {
            swprintf_s(buf, L"%02d", day);
            result += buf;
            i += 2;
        } else {
            result.push_back(format[i]);
            ++i;
        }
    }
    return result;
}

inline std::wstring ResolveTodayPath(const DailyNoteConfig& config, const std::wstring& vaultPath,
    int year, int month, int day) {
    fs::path base(vaultPath);
    if (!config.folder.empty()) {
        base /= config.folder;
    }
    const std::wstring filename = FormatDateTokens(config.format, year, month, day) + L".md";
    return (base / filename).wstring();
}

inline void GetTodayYmd(int& year, int& month, int& day) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    year = st.wYear;
    month = st.wMonth;
    day = st.wDay;
}

// Recognizes "task <text>" (case-insensitive prefix, at least one
// non-whitespace character required after it). Leading spaces immediately
// after "task" are trimmed; the rest of the input is used verbatim as the
// task text (not further parsed).
inline bool TryParseTaskPrefix(const std::wstring& input, std::wstring& outText) {
    constexpr wchar_t kPrefix[] = L"task ";
    constexpr size_t kPrefixLen = 5;
    if (input.size() <= kPrefixLen) return false;
    if (_wcsnicmp(input.c_str(), kPrefix, kPrefixLen) != 0) return false;

    std::wstring rest = input.substr(kPrefixLen);
    const size_t start = rest.find_first_not_of(L' ');
    if (start == std::wstring::npos) return false;

    outText = rest.substr(start);
    return !outText.empty();
}

// Builds a Markdown checklist line for one task. Strips embedded CR/LF so a
// pasted multi-line "task" can never split into more than one list item.
inline std::wstring BuildTaskLine(std::wstring_view text) {
    std::wstring clean;
    clean.reserve(text.size());
    for (wchar_t ch : text) {
        if (ch == L'\r' || ch == L'\n') continue;
        clean.push_back(ch);
    }
    return L"- [ ] " + clean + L"\n";
}

inline std::wstring FormatIsoTimestamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[32];
    swprintf_s(buf, L"%04d-%02d-%02dT%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}

// Appends one task line to the note at notePath, creating the file (with
// parent directories and minimal frontmatter) if it doesn't exist yet.
// Returns false on any I/O failure - the caller must surface this to the
// user rather than silently dropping the task (spec: "task never silently
// dropped").
inline bool AppendTask(const std::wstring& notePath, std::wstring_view taskText) {
    std::error_code ec;
    const fs::path path(notePath);
    fs::create_directories(path.parent_path(), ec);

    const bool isNewFile = !fs::exists(path, ec);

    HANDLE file = CreateFileW(notePath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    std::wstring content;
    if (isNewFile) {
        content = L"---\ncreated: " + FormatIsoTimestamp() + L"\n---\n\n";
    }
    content += BuildTaskLine(taskText);

    bool ok = false;
    const int utf8Len = WideCharToMultiByte(CP_UTF8, 0, content.data(), static_cast<int>(content.size()),
        nullptr, 0, nullptr, nullptr);
    if (utf8Len > 0) {
        std::string utf8(static_cast<size_t>(utf8Len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, content.data(), static_cast<int>(content.size()),
            utf8.data(), utf8Len, nullptr, nullptr);
        DWORD written = 0;
        ok = WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr) &&
             written == utf8.size();
    }
    CloseHandle(file);
    return ok;
}

}  // namespace obsidian
}  // namespace leanlauncher
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build --config Release --target LeanLauncherCoreTests
ctest --test-dir build -C Release --output-on-failure -R LeanLauncherCoreTests
```

Expected: all `FormatDateTokens` / `ResolveTodayPath` / `TryParseTaskPrefix` / `BuildTaskLine` checks pass.

- [ ] **Step 5: Add a temp-directory `AppendTask` integration test**

Append to the same test block (this one does touch disk, but only a scratch temp directory, never a real vault - matches spec's testing section):

```cpp
    {
        wchar_t tempDir[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tempDir);
        const std::wstring notePath = std::wstring(tempDir) + L"LeanLauncherTest_2026-09-14.md";
        DeleteFileW(notePath.c_str());  // ensure clean starting state

        Check(AppendTask(notePath, L"first task"), "AppendTask creates a new file and returns true");

        std::ifstream check(notePath, std::ios::binary);
        std::ostringstream ss;
        ss << check.rdbuf();
        const std::string firstContent = ss.str();
        Check(firstContent.find("---\ncreated:") != std::string::npos,
            "AppendTask writes minimal frontmatter for a new file");
        Check(firstContent.find("- [ ] first task\n") != std::string::npos,
            "AppendTask writes the task line for a new file");

        Check(AppendTask(notePath, L"second task"), "AppendTask appends to an existing file and returns true");
        check.close();
        std::ifstream check2(notePath, std::ios::binary);
        std::ostringstream ss2;
        ss2 << check2.rdbuf();
        const std::string secondContent = ss2.str();
        Check(secondContent.find("- [ ] first task\n") != std::string::npos &&
              secondContent.find("- [ ] second task\n") != std::string::npos,
            "AppendTask preserves the first task and adds the second");
        Check(std::count(secondContent.begin(), secondContent.end(), '\n') ==
              static_cast<long>(std::count(firstContent.begin(), firstContent.end(), '\n')) + 1,
            "AppendTask on an existing file adds exactly one new line, no duplicate frontmatter");

        DeleteFileW(notePath.c_str());
    }
```

This requires `#include <fstream>` and `#include <sstream>` in `tests/core_tests.cpp` if not already present (they are, transitively, via `obsidian_config.h`, but add explicit includes at the top of the file for clarity).

- [ ] **Step 6: Run the tests to verify the temp-directory test passes**

```bash
cmake --build build --config Release --target LeanLauncherCoreTests
ctest --test-dir build -C Release --output-on-failure -R LeanLauncherCoreTests
```

- [ ] **Step 7: Add the header to `CMakeLists.txt`'s source list**

```cmake
add_executable(${target} WIN32
    src/main.cpp
    src/launcher.h
    src/file_index.h
    src/search.h
    src/settings.h
    src/updates.h
    src/calculator.h
    src/obsidian_config.h
    src/daily_note.h
    src/LeanLauncher.manifest
    src/LeanLauncher.rc
)
```

- [ ] **Step 8: Commit**

```bash
git add -A
git commit -m "Add daily_note.h: task-prefix parsing, date formatting, and file append

TryParseTaskPrefix recognizes 'task <text>'; FormatDateTokens/ResolveTodayPath
turn a DailyNoteConfig + today's date into an absolute note path; AppendTask
does the direct file write (creating minimal frontmatter if the note doesn't
exist yet). All pure logic is unit tested; AppendTask is integration tested
against a temp directory only."
```

---

### Task 5: Wire the "task <text>" result into the launcher's search/launch pipeline

**Files:**
- Modify: `src/search.h` (new `AppCategory::TaskAdd` enum value)
- Modify: `src/launcher.h` (`UpdateResults()`, `LaunchSelected()`, `RunAction()`, `DrawResults()`/Paint category label, `DrawFooter()`, `PointInAdminAction()`, `DrawActions()`, plus new member state for the resolved vault path and cached daily-note config)
- Modify: `tests/core_tests.cpp` (one new integration-shaped check using the mock-app pattern already used for the Calculator category)

**Interfaces:**
- Consumes: `leanlauncher::obsidian::TryParseTaskPrefix`, `GetTodayYmd`, `ResolveTodayPath`, `AppendTask` from Task 4; `leanlauncher::obsidian::DailyNoteConfig`, `ReadDailyNoteConfig` from Task 3.
- Consumes (from Task 6, forward reference): `LauncherWindow::obsidianVaultPath_` (a `std::wstring` member) - Task 6 adds the Settings UI that lets the user set it, but this task must add the member itself since `UpdateResults()`/`LaunchSelected()` need to read it. Task 6 adds the *write* path (Settings row) plus registry persistence; this task adds the member and a *read-only* default of empty string so the feature degrades gracefully (shows "Set up your vault in Settings") before Task 6 exists.
- Produces: `AppCategory::TaskAdd` is a new value later consumed by Task 6's rendering code for the Settings row status text (no coupling - just documented for context).

- [ ] **Step 1: Add `AppCategory::TaskAdd` to `src/search.h`**

Replace:
```cpp
enum class AppCategory : uint8_t {
    Application,
    System,
    File,
    Folder,
    Calculator
};
```
with:
```cpp
enum class AppCategory : uint8_t {
    Application,
    System,
    File,
    Folder,
    Calculator,
    TaskAdd
};
```

- [ ] **Step 2: Add vault-path and daily-note-config members to `LauncherWindow` in `src/launcher.h`**

Near the other private members that hold cross-cutting state (search for `Settings settings_;` inside the class and add just after it). Note the new headers live in the `leanlauncher::obsidian` namespace, not `takeoff::obsidian` — this codebase's existing namespace (`takeoff`/`quicklaunch`) is deliberately left unrenamed per Task 2's constraints; the new files use `leanlauncher` since they're new Lean Launcher code, not inherited Takeoff code:

```cpp
    quicklaunch::Settings settings_;
    std::wstring obsidianVaultPath_;
    leanlauncher::obsidian::DailyNoteConfig dailyNoteConfig_;
```

Add the include near the top of `main.cpp` (where `search.h`, `settings.h`, etc. are included, since `launcher.h` is `#include`d *inside* `main.cpp`'s anonymous namespace and relies on `main.cpp`'s top-level includes):

```cpp
#include "resource.h"
#include "search.h"
#include "settings.h"
#include "updates.h"
#include "file_index.h"
#include "calculator.h"
#include "obsidian_config.h"
#include "daily_note.h"
```

- [ ] **Step 3: Load the vault path from the registry in `LoadSettings()`**

Inside `LoadSettings()`'s existing `if constexpr (!kUiTest)` block, after the existing `RegGetValueW` calls for `UpdateApiHost`/`UpdateApiPath` (reuse the same open `key` handle, same buffer pattern already in that function):

```cpp
                bufSize = sizeof(buf);
                if (RegGetValueW(key, nullptr, L"VaultPath", RRF_RT_REG_SZ, nullptr, buf, &bufSize) == ERROR_SUCCESS && buf[0]) {
                    obsidianVaultPath_ = buf;
                }
```

Immediately after the existing `RegCloseKey(key);` call that ends that block, add:
```cpp
            if (!obsidianVaultPath_.empty()) {
                dailyNoteConfig_ = leanlauncher::obsidian::ReadDailyNoteConfig(obsidianVaultPath_);
            }
```

- [ ] **Step 4: Persist the vault path in `SaveSettings()`**

At the end of `SaveSettings()`'s existing `if constexpr (!kUiTest)` block, after the `for (const auto& entry : entries)` loop and before `RegCloseKey(key);`:

```cpp
            if (RegSetValueExW(key, L"VaultPath", 0, REG_SZ,
                    reinterpret_cast<const BYTE*>(obsidianVaultPath_.c_str()),
                    static_cast<DWORD>((obsidianVaultPath_.size() + 1) * sizeof(wchar_t))) != ERROR_SUCCESS) {
                saved = false;
            }
```

(Match the existing `saved` accumulator variable already in that function so a write failure surfaces via the existing `if (!saved) settingsStatus_ = L"Could not save this setting.";` path.)

- [ ] **Step 5: Detect the "task <text>" prefix in `UpdateResults()`**

In `UpdateResults()`, right after the existing calculator block (the one that does `apps_.push_back(std::move(entry)); results_.insert(results_.begin(), calcIdx);`), add:

```cpp
        std::wstring taskText;
        if (leanlauncher::obsidian::TryParseTaskPrefix(input_.text, taskText)) {
            AppEntry entry;
            entry.category = AppCategory::TaskAdd;
            entry.parameters = taskText;
            if (obsidianVaultPath_.empty()) {
                entry.name = L"Set up your vault in Settings";
                entry.iconPath = L"notepad.exe";
            } else {
                int year = 0, month = 0, day = 0;
                leanlauncher::obsidian::GetTodayYmd(year, month, day);
                entry.path = leanlauncher::obsidian::ResolveTodayPath(
                    dailyNoteConfig_, obsidianVaultPath_, year, month, day);
                entry.name = L"Add to today's note: " + taskText;
                entry.iconPath = L"notepad.exe";
            }
            entry.normalizedName = Normalize(entry.name);
            const size_t taskIdx = apps_.size();
            apps_.push_back(std::move(entry));
            results_.insert(results_.begin(), taskIdx);
        }
```

- [ ] **Step 6: Handle `AppCategory::TaskAdd` in `LaunchSelected()`**

At the very top of `LaunchSelected()`, right after the existing `if (app.category == takeoff::AppCategory::Calculator) { ... }` block and before `const std::wstring& path = app.path;`, add:

```cpp
        if (app.category == takeoff::AppCategory::TaskAdd) {
            if (app.path.empty()) {
                // No vault configured yet - open Settings instead of silently
                // doing nothing (spec: result row reads "Set up your vault in
                // Settings" and must not add anything when activated).
                OpenSettings();
                return;
            }
            Hide();
            const bool ok = leanlauncher::obsidian::AppendTask(app.path, app.parameters);
            if (!ok) {
                ShowWindow(hwnd_, SW_SHOWNORMAL);
                SetForegroundWindow(hwnd_);
                SetFocus(hwnd_);
                status_ = L"Could not write to your vault. Check Settings.";
                ResetCaret();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return;
        }
```

Note `AppCategory` here is unqualified inside `takeoff::` because `search.h` defines it inside `namespace takeoff` (Task 2 deliberately did not rename that namespace) - so the new enum value is `takeoff::AppCategory::TaskAdd`, matching every other reference to `AppCategory` in this file (e.g. `takeoff::AppCategory::Calculator` two lines above it).

- [ ] **Step 7: Handle `AppCategory::TaskAdd` in `RunAction()` (right-click / Ctrl+K actions menu)**

Right after the existing Calculator `if` block in `RunAction()` (the one handling `action == 0/1/2` for Calculator), add:

```cpp
        if (app.category == takeoff::AppCategory::TaskAdd) {
            if (action == 0) {
                LaunchSelected(false);
                return;
            } else if (action == 1) {
                const bool copied = CopyText(app.parameters);
                status_ = copied ? L"Task text copied" : L"Clipboard is busy. Try again.";
                ResetCaret();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            } else if (action == 2) {
                if (!app.path.empty()) {
                    ShellExecuteW(nullptr, L"open", app.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
                Hide();
                return;
            }
        }
```

- [ ] **Step 8: Add a `TaskAdd` label set to `DrawActions()`**

Replace:
```cpp
        const bool isCalc = (app.category == takeoff::AppCategory::Calculator);
        const bool isFileOrFolder = (app.category == takeoff::AppCategory::File ||
                                     app.category == takeoff::AppCategory::Folder);
        const wchar_t* appLabels[] = {L"Open as Administrator", L"Copy app name", L"Copy launch path"};
        const wchar_t* fileLabels[] = {L"Open", L"Open containing folder", L"Copy file path"};
        const wchar_t* calcLabels[] = {L"Copy result", L"Copy calculation", L"Open Windows Calculator"};
        const wchar_t** labels = isCalc ? calcLabels : (isFileOrFolder ? fileLabels : appLabels);
```
with:
```cpp
        const bool isCalc = (app.category == takeoff::AppCategory::Calculator);
        const bool isTaskAdd = (app.category == takeoff::AppCategory::TaskAdd);
        const bool isFileOrFolder = (app.category == takeoff::AppCategory::File ||
                                     app.category == takeoff::AppCategory::Folder);
        const wchar_t* appLabels[] = {L"Open as Administrator", L"Copy app name", L"Copy launch path"};
        const wchar_t* fileLabels[] = {L"Open", L"Open containing folder", L"Copy file path"};
        const wchar_t* calcLabels[] = {L"Copy result", L"Copy calculation", L"Open Windows Calculator"};
        const wchar_t* taskLabels[] = {L"Add to note", L"Copy task text", L"Open today's note"};
        const wchar_t** labels = isCalc ? calcLabels : (isTaskAdd ? taskLabels : (isFileOrFolder ? fileLabels : appLabels));
```

- [ ] **Step 9: Skip the "Open as Administrator" footer/click affordances for `TaskAdd` rows**

In `PointInAdminAction()`, replace:
```cpp
        const AppEntry& app = apps_[results_[selected_]];
        if (app.category == takeoff::AppCategory::Calculator) return false;
```
with:
```cpp
        const AppEntry& app = apps_[results_[selected_]];
        if (app.category == takeoff::AppCategory::Calculator ||
            app.category == takeoff::AppCategory::TaskAdd) return false;
```

In `DrawFooter()`, replace:
```cpp
        } else if (HasResult()) {
            const AppEntry& app = apps_[results_[selected_]];
            if (app.category == takeoff::AppCategory::Calculator) {
                Text(L"Copy result", D2D1::RectF(24, top, 156, height_),
                    hintFormat_.Get(), actionsOpen_ ? Foreground() : Muted());
                Key(L"\u21B5", 96, top + (kFooterHeight - 22) / 2, 24);
            } else {
```
with:
```cpp
        } else if (HasResult()) {
            const AppEntry& app = apps_[results_[selected_]];
            if (app.category == takeoff::AppCategory::Calculator) {
                Text(L"Copy result", D2D1::RectF(24, top, 156, height_),
                    hintFormat_.Get(), actionsOpen_ ? Foreground() : Muted());
                Key(L"\u21B5", 96, top + (kFooterHeight - 22) / 2, 24);
            } else if (app.category == takeoff::AppCategory::TaskAdd) {
                Text(L"Add to note", D2D1::RectF(24, top, 156, height_),
                    hintFormat_.Get(), actionsOpen_ ? Foreground() : Muted());
                Key(L"\u21B5", 96, top + (kFooterHeight - 22) / 2, 24);
            } else {
```

In `HandleClick()`'s footer administrator-launch branch, replace:
```cpp
            } else if (HasResult()) {
                const AppEntry& app = apps_[results_[selected_]];
                if (app.category == takeoff::AppCategory::Calculator) {
                    CopyText(app.path);
                    Hide();
                } else if (!settings_.administratorHotkey.disabled) {
                    LaunchSelected(true);
                }
            }
```
with:
```cpp
            } else if (HasResult()) {
                const AppEntry& app = apps_[results_[selected_]];
                if (app.category == takeoff::AppCategory::Calculator) {
                    CopyText(app.path);
                    Hide();
                } else if (app.category == takeoff::AppCategory::TaskAdd) {
                    LaunchSelected(false);
                } else if (!settings_.administratorHotkey.disabled) {
                    LaunchSelected(true);
                }
            }
```

- [ ] **Step 10: Add a "Note" category label in the results-list Paint code**

Replace:
```cpp
                const wchar_t* categoryLabel = recent ? L"Recent"
                    : (app.category == takeoff::AppCategory::System ? L"System"
                    : (app.category == takeoff::AppCategory::Folder ? L"Folder"
                    : (app.category == takeoff::AppCategory::File ? L"File" : L"Application")));
```
with:
```cpp
                const wchar_t* categoryLabel = recent ? L"Recent"
                    : (app.category == takeoff::AppCategory::System ? L"System"
                    : (app.category == takeoff::AppCategory::Folder ? L"Folder"
                    : (app.category == takeoff::AppCategory::File ? L"File"
                    : (app.category == takeoff::AppCategory::TaskAdd ? L"Note" : L"Application")));
```

(This adds one more closing `)` to balance the new nested ternary - count parens carefully when editing.)

- [ ] **Step 11: Build**

```bash
cd "D:/GitProjects/lean-launcher"
cmake --build build --config Release --target LeanLauncher
cmake --build build --config Release --target LeanLauncherCoreTests
```

Expected: both build cleanly (the app target exercises the real Win32/Direct2D code paths this task touched; there's no automated UI test harness for them - manual smoke testing happens in Task 7).

- [ ] **Step 12: Add a mock-pipeline test mirroring the existing Calculator integration test**

In `tests/core_tests.cpp`, right after the existing "12. Launcher integration & ranking invariants" block (the one building a `MockApp` vector and inserting a calculator result), add:

```cpp
    // 13. Task-add integration: prefix detection -> synthetic result -> append
    {
        std::wstring taskText;
        Check(TryParseTaskPrefix(L"task write plan", taskText) && taskText == L"write plan",
            "task-add pipeline: prefix parses correctly before result construction");

        wchar_t tempDir[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tempDir);
        const std::wstring vaultRoot = std::wstring(tempDir) + L"LeanLauncherTestVault";
        CreateDirectoryW(vaultRoot.c_str(), nullptr);

        DailyNoteConfig config;  // empty folder + default format = vault root note
        int year = 0, month = 0, day = 0;
        GetTodayYmd(year, month, day);
        const std::wstring notePath = ResolveTodayPath(config, vaultRoot, year, month, day);
        DeleteFileW(notePath.c_str());

        Check(AppendTask(notePath, taskText), "task-add pipeline: AppendTask writes the resolved path");
        std::ifstream check(notePath, std::ios::binary);
        std::ostringstream ss;
        ss << check.rdbuf();
        Check(ss.str().find("- [ ] write plan\n") != std::string::npos,
            "task-add pipeline: end-to-end content matches what was typed");
        check.close();
        DeleteFileW(notePath.c_str());
        RemoveDirectoryW(vaultRoot.c_str());
    }
```

- [ ] **Step 13: Run the full test suite**

```bash
cmake --build build --config Release --target LeanLauncherCoreTests
ctest --test-dir build -C Release --output-on-failure -R LeanLauncherCoreTests
```

Expected: all checks pass, including the new end-to-end pipeline test.

- [ ] **Step 14: Commit**

```bash
git add -A
git commit -m "Wire 'task <text>' into the search/launch pipeline

UpdateResults() detects the task prefix and inserts a synthetic TaskAdd
result exactly like the existing calculator inline-eval feature. Enter
appends via AppendTask() and hides the launcher; a missing vault opens
Settings instead of silently doing nothing; write failures surface an
inline status message rather than dropping the task."
```

---

### Task 6: Settings UI — "Obsidian Vault" row

**Files:**
- Modify: `src/launcher.h` (`SettingsCategory` enum, `IsRowInCategory`, `FirstRowInCategory`, `LastRowInCategory`, `CategoryTabRect`, `SettingsRowTop`, `SettingsContentBottom`, `EnsureSettingsVisible`, `NextSettingsRow`, `SettingsRowAtPoint`, `ChangeSetting`, `HandleClick`'s tab/reset-button dispatch, `DrawSettingsRow`, `DrawSettings`, `OpenSettings`)

**Interfaces:**
- Consumes: `leanlauncher::obsidian::FindKnownVaults()` (Task 3), `obsidianVaultPath_`/`dailyNoteConfig_` members and `SaveSettings()` (Task 5).
- Produces: a 5th settings tab "Vault" containing row index 9 ("Obsidian Vault"); the "Reset to default" action moves from row index 9 to row index 10 everywhere it's referenced.

This task renumbers the Reset button from row 9 to row 10 and adds a new content row 9. Every one of the following edits must land together (they're mutually inconsistent individually) - this is one task, not splittable, because a partial application breaks keyboard navigation.

- [ ] **Step 1: Add the `Vault` category and a `knownVaults_` member**

Replace:
```cpp
    enum class SettingsCategory : uint8_t { All, Shortcuts, System, Search };
```
with:
```cpp
    enum class SettingsCategory : uint8_t { All, Shortcuts, System, Search, Vault };
```

Add one new member directly below the existing `obsidianVaultPath_`/`dailyNoteConfig_` members Task 5 already added (do not re-declare those two — only add the line below):
```cpp
    std::wstring obsidianVaultPath_;
    leanlauncher::obsidian::DailyNoteConfig dailyNoteConfig_;
    std::vector<std::wstring> knownVaults_;  // <-- new in this task
```

- [ ] **Step 2: Refresh `knownVaults_` when Settings opens**

In `OpenSettings()`, add one line before `InvalidateRect(hwnd_, nullptr, FALSE);`:
```cpp
        knownVaults_ = leanlauncher::obsidian::FindKnownVaults();
```

- [ ] **Step 3: Extend row-range helpers to cover row 9 (Vault) and move Reset to row 10**

Replace:
```cpp
    static bool IsRowInCategory(int row, SettingsCategory cat) {
        if (cat == SettingsCategory::All) return row >= 0 && row <= 8;
        if (cat == SettingsCategory::Shortcuts) return row >= 0 && row <= 3;
        if (cat == SettingsCategory::System) return row >= 4 && row <= 6;
        if (cat == SettingsCategory::Search) return row >= 7 && row <= 8;
        return false;
    }

    static int FirstRowInCategory(SettingsCategory cat) {
        if (cat == SettingsCategory::Shortcuts) return 0;
        if (cat == SettingsCategory::System) return 4;
        if (cat == SettingsCategory::Search) return 7;
        return 0;
    }

    static int LastRowInCategory(SettingsCategory cat) {
        if (cat == SettingsCategory::Shortcuts) return 3;
        if (cat == SettingsCategory::System) return 6;
        if (cat == SettingsCategory::Search) return 8;
        return 8;
    }

    int NextSettingsRow(int current, int delta) const {
        std::vector<int> activeRows;
        for (int r = 0; r <= 8; ++r) {
            if (IsRowInCategory(r, settingsCategory_)) {
                activeRows.push_back(r);
            }
        }
        activeRows.push_back(9);
```
with:
```cpp
    static bool IsRowInCategory(int row, SettingsCategory cat) {
        if (cat == SettingsCategory::All) return row >= 0 && row <= 9;
        if (cat == SettingsCategory::Shortcuts) return row >= 0 && row <= 3;
        if (cat == SettingsCategory::System) return row >= 4 && row <= 6;
        if (cat == SettingsCategory::Search) return row >= 7 && row <= 8;
        if (cat == SettingsCategory::Vault) return row == 9;
        return false;
    }

    static int FirstRowInCategory(SettingsCategory cat) {
        if (cat == SettingsCategory::Shortcuts) return 0;
        if (cat == SettingsCategory::System) return 4;
        if (cat == SettingsCategory::Search) return 7;
        if (cat == SettingsCategory::Vault) return 9;
        return 0;
    }

    static int LastRowInCategory(SettingsCategory cat) {
        if (cat == SettingsCategory::Shortcuts) return 3;
        if (cat == SettingsCategory::System) return 6;
        if (cat == SettingsCategory::Search) return 8;
        if (cat == SettingsCategory::Vault) return 9;
        return 9;
    }

    int NextSettingsRow(int current, int delta) const {
        std::vector<int> activeRows;
        for (int r = 0; r <= 9; ++r) {
            if (IsRowInCategory(r, settingsCategory_)) {
                activeRows.push_back(r);
            }
        }
        activeRows.push_back(10);
```

- [ ] **Step 4: Add the Vault tab's rectangle**

Replace:
```cpp
    D2D1_RECT_F CategoryTabRect(SettingsCategory cat) const {
        constexpr float y = 11.0f;
        constexpr float h = 24.0f;
        float x = 160.0f;
        float w = 40.0f;
        if (cat == SettingsCategory::Shortcuts) {
            x = 160.0f + 40.0f + 6.0f;
            w = 82.0f;
        } else if (cat == SettingsCategory::System) {
            x = 160.0f + 40.0f + 6.0f + 82.0f + 6.0f;
            w = 68.0f;
        } else if (cat == SettingsCategory::Search) {
            x = 160.0f + 40.0f + 6.0f + 82.0f + 6.0f + 68.0f + 6.0f;
            w = 68.0f;
        }
        return D2D1::RectF(x, y, x + w, y + h);
    }
```
with:
```cpp
    D2D1_RECT_F CategoryTabRect(SettingsCategory cat) const {
        constexpr float y = 11.0f;
        constexpr float h = 24.0f;
        float x = 160.0f;
        float w = 40.0f;
        if (cat == SettingsCategory::Shortcuts) {
            x = 160.0f + 40.0f + 6.0f;
            w = 82.0f;
        } else if (cat == SettingsCategory::System) {
            x = 160.0f + 40.0f + 6.0f + 82.0f + 6.0f;
            w = 68.0f;
        } else if (cat == SettingsCategory::Search) {
            x = 160.0f + 40.0f + 6.0f + 82.0f + 6.0f + 68.0f + 6.0f;
            w = 68.0f;
        } else if (cat == SettingsCategory::Vault) {
            x = 160.0f + 40.0f + 6.0f + 82.0f + 6.0f + 68.0f + 6.0f + 68.0f + 6.0f;
            w = 68.0f;
        }
        return D2D1::RectF(x, y, x + w, y + h);
    }
```

- [ ] **Step 5: Extend `SettingsContentBottom` for the new Vault-only tab and the taller All view**

Replace:
```cpp
    float SettingsContentBottom() const {
        if (settingsCategory_ == SettingsCategory::All) {
            return 551.0f;
        } else if (settingsCategory_ == SettingsCategory::Shortcuts) {
            return 240.0f;
        } else if (settingsCategory_ == SettingsCategory::System) {
            return 193.0f;
        } else if (settingsCategory_ == SettingsCategory::Search) {
            return 146.0f;
        }
        return 200.0f;
    }
```
with:
```cpp
    float SettingsContentBottom() const {
        if (settingsCategory_ == SettingsCategory::All) {
            return 636.0f;  // was 551; +85 for the new one-row Vault card + its header/gap
        } else if (settingsCategory_ == SettingsCategory::Shortcuts) {
            return 240.0f;
        } else if (settingsCategory_ == SettingsCategory::System) {
            return 193.0f;
        } else if (settingsCategory_ == SettingsCategory::Search) {
            return 146.0f;
        } else if (settingsCategory_ == SettingsCategory::Vault) {
            return 99.0f;  // 36 header offset + 1 row (47) + 16 bottom padding
        }
        return 200.0f;
    }
```

- [ ] **Step 6: Extend `SettingsRowTop` for row 9 in both the All view and the Vault-only view**

Replace:
```cpp
    float SettingsRowTop(int row) const {
        if (settingsCategory_ == SettingsCategory::All) {
            if (row < 4) return 36.0f + row * kSettingsRowHeight;
            if (row < 7) return 262.0f + (row - 4) * kSettingsRowHeight;
            return 441.0f + (row - 7) * kSettingsRowHeight;
        } else if (settingsCategory_ == SettingsCategory::Shortcuts) {
            return 36.0f + row * kSettingsRowHeight;
        } else if (settingsCategory_ == SettingsCategory::System) {
            return 36.0f + (row - 4) * kSettingsRowHeight;
        } else if (settingsCategory_ == SettingsCategory::Search) {
            return 36.0f + (row - 7) * kSettingsRowHeight;
        }
        return 0.0f;
    }
```
with:
```cpp
    float SettingsRowTop(int row) const {
        if (settingsCategory_ == SettingsCategory::All) {
            if (row < 4) return 36.0f + row * kSettingsRowHeight;
            if (row < 7) return 262.0f + (row - 4) * kSettingsRowHeight;
            if (row < 9) return 441.0f + (row - 7) * kSettingsRowHeight;
            return 573.0f;  // row 9 (Vault), All-view only section: header@553, card@573
        } else if (settingsCategory_ == SettingsCategory::Shortcuts) {
            return 36.0f + row * kSettingsRowHeight;
        } else if (settingsCategory_ == SettingsCategory::System) {
            return 36.0f + (row - 4) * kSettingsRowHeight;
        } else if (settingsCategory_ == SettingsCategory::Search) {
            return 36.0f + (row - 7) * kSettingsRowHeight;
        } else if (settingsCategory_ == SettingsCategory::Vault) {
            return 36.0f;
        }
        return 0.0f;
    }
```

- [ ] **Step 7: Extend `SettingsRowAtPoint` to scan row 9**

Replace:
```cpp
        for (int r = 0; r <= 8; ++r) {
            if (!IsRowInCategory(r, settingsCategory_)) continue;
```
with:
```cpp
        for (int r = 0; r <= 9; ++r) {
            if (!IsRowInCategory(r, settingsCategory_)) continue;
```
(inside `SettingsRowAtPoint`, not the other loops touched in Step 3 - there are two separate `for` loops over row ranges in this file; make sure this edit targets `SettingsRowAtPoint`, distinguishable by its surrounding `contentY`/`kSettingsRowHeight` comparison body.)

- [ ] **Step 8: Update `EnsureSettingsVisible` for the renumbered Reset row and the new Vault section header**

Replace:
```cpp
    void EnsureSettingsVisible(int row) {
        if (row == 9) {
            settingsScroll_ = 0.0f;
            return;
        }
        if (row < 0 || row > 8 || !IsRowInCategory(row, settingsCategory_)) return;
        const float rTop = SettingsRowTop(row);
        const float rBottom = rTop + kSettingsRowHeight;
        const float maxScroll = SettingsMaxScroll();
        float sectionHeaderTop = rTop;
        if (settingsCategory_ == SettingsCategory::All) {
            if (row == 0) sectionHeaderTop = 16.0f;
            else if (row == 4) sectionHeaderTop = 242.0f;
            else if (row == 7) sectionHeaderTop = 421.0f;
        } else {
            if (row == 0 || row == 4 || row == 7) sectionHeaderTop = 16.0f;
        }
```
with:
```cpp
    void EnsureSettingsVisible(int row) {
        if (row == 10) {
            settingsScroll_ = 0.0f;
            return;
        }
        if (row < 0 || row > 9 || !IsRowInCategory(row, settingsCategory_)) return;
        const float rTop = SettingsRowTop(row);
        const float rBottom = rTop + kSettingsRowHeight;
        const float maxScroll = SettingsMaxScroll();
        float sectionHeaderTop = rTop;
        if (settingsCategory_ == SettingsCategory::All) {
            if (row == 0) sectionHeaderTop = 16.0f;
            else if (row == 4) sectionHeaderTop = 242.0f;
            else if (row == 7) sectionHeaderTop = 421.0f;
            else if (row == 9) sectionHeaderTop = 553.0f;
        } else {
            if (row == 0 || row == 4 || row == 7 || row == 9) sectionHeaderTop = 16.0f;
        }
```

- [ ] **Step 9: Add `ChangeSetting(9)` (cycle known vaults) and renumber the Reset sentinel**

Replace:
```cpp
    void ChangeSetting(int row) {
        settingsStatus_.clear();
        if (row == 9) {
            ResetToDefaults();
            return;
        }
        if (row < 4) {
```
with:
```cpp
    void ChangeSetting(int row) {
        settingsStatus_.clear();
        if (row == 10) {
            ResetToDefaults();
            return;
        }
        if (row == 9) {
            CycleVaultSelection();
            return;
        }
        if (row < 4) {
```

Add the new method right after `ChangeSetting` (or right before it - anywhere in the class body is fine, but keep it next to `ChangeSetting` for readability):
```cpp
    void CycleVaultSelection() {
        if (knownVaults_.empty()) {
            settingsStatus_ = L"No Obsidian vaults found. Open a vault in Obsidian, then reopen Settings.";
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        size_t nextIndex = 0;
        const auto it = std::find(knownVaults_.begin(), knownVaults_.end(), obsidianVaultPath_);
        if (it != knownVaults_.end()) {
            nextIndex = (static_cast<size_t>(std::distance(knownVaults_.begin(), it)) + 1) % knownVaults_.size();
        }
        obsidianVaultPath_ = knownVaults_[nextIndex];
        dailyNoteConfig_ = leanlauncher::obsidian::ReadDailyNoteConfig(obsidianVaultPath_);
        SaveSettings();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
```

- [ ] **Step 10: Renumber the Reset button's row references in `HandleClick` and `DrawSettings`**

In `HandleClick()`, replace:
```cpp
                const auto resetRect = ResetButtonRect();
                if (x >= resetRect.left && x <= resetRect.right && y >= resetRect.top && y <= resetRect.bottom) {
                    settingsSelected_ = 9;
                    ResetToDefaults();
                    return;
                }
```
with:
```cpp
                const auto resetRect = ResetButtonRect();
                if (x >= resetRect.left && x <= resetRect.right && y >= resetRect.top && y <= resetRect.bottom) {
                    settingsSelected_ = 10;
                    ResetToDefaults();
                    return;
                }
```

In `DrawSettings()`, replace:
```cpp
        const bool resetSelected = (settingsSelected_ == 9);
```
with:
```cpp
        const bool resetSelected = (settingsSelected_ == 10);
```

Also in `HandleClick()`, extend the tab-hit-test array (and the matching array in `DrawSettings()`, Step 12 below) to include `Vault`:
```cpp
                const SettingsCategory categories[] = {
                    SettingsCategory::All,
                    SettingsCategory::Shortcuts,
                    SettingsCategory::System,
                    SettingsCategory::Search,
                    SettingsCategory::Vault
                };
```

- [ ] **Step 11: Add a `plainValue` rendering mode to `DrawSettingsRow`**

The existing `value` parameter renders through `DrawKeyBadges`, which is designed for hotkey badges (boxed key names), not a file path. Add a plain-text trailing-value mode instead of misusing that path. Replace:
```cpp
    void DrawSettingsRow(int index, float top, std::wstring_view title,
        std::wstring_view description, std::wstring_view value = {}, bool toggle = false,
        bool enabled = false) {
```
with:
```cpp
    void DrawSettingsRow(int index, float top, std::wstring_view title,
        std::wstring_view description, std::wstring_view value = {}, bool toggle = false,
        bool enabled = false, bool plainValue = false) {
```

Replace:
```cpp
        if (toggle) {
            DrawToggle(width_ - 36, top + kSettingsRowHeight / 2, enabled);
        } else if (index == recordingRow_) {
            Text(L"Press keys\u2026", D2D1::RectF(width_ - 240, top, width_ - 36, top + kSettingsRowHeight),
                hintFormat_.Get(), highContrast_ ? SystemColor(COLOR_HIGHLIGHTTEXT) : D2D1::ColorF(0x6EA8FE),
                DWRITE_TEXT_ALIGNMENT_TRAILING);
        } else {
            const float badgesW = KeyBadgesWidth(value);
            DrawKeyBadges(value, width_ - 36 - badgesW, top + kSettingsRowHeight / 2);
        }
    }
```
with:
```cpp
        if (toggle) {
            DrawToggle(width_ - 36, top + kSettingsRowHeight / 2, enabled);
        } else if (index == recordingRow_) {
            Text(L"Press keys\u2026", D2D1::RectF(width_ - 240, top, width_ - 36, top + kSettingsRowHeight),
                hintFormat_.Get(), highContrast_ ? SystemColor(COLOR_HIGHLIGHTTEXT) : D2D1::ColorF(0x6EA8FE),
                DWRITE_TEXT_ALIGNMENT_TRAILING);
        } else if (plainValue) {
            Text(value, D2D1::RectF(width_ - 260, top, width_ - 36, top + kSettingsRowHeight),
                hintFormat_.Get(), secondary, DWRITE_TEXT_ALIGNMENT_TRAILING);
        } else {
            const float badgesW = KeyBadgesWidth(value);
            DrawKeyBadges(value, width_ - 36 - badgesW, top + kSettingsRowHeight / 2);
        }
    }
```

- [ ] **Step 12: Draw the Vault card in `DrawSettings()` and add the 5th tab**

Replace:
```cpp
        if (settingsCategory_ == SettingsCategory::All || settingsCategory_ == SettingsCategory::Search) {
            const float hY = (settingsCategory_ == SettingsCategory::All) ? 421.0f : 16.0f;
            const float cY = (settingsCategory_ == SettingsCategory::All) ? 441.0f : 36.0f;
            drawCard(L"SEARCH & FEATURES", hY, cY, 2);

            DrawSettingsRow(7, cY + offsetY, L"File search",
                L"Search files and folders on your computer", {}, true, settings_.enableFileSearch);
            DrawSettingsRow(8, cY + kSettingsRowHeight + offsetY, L"Web search",
                L"Open Google when no results match your query", {}, true, settings_.enableWebSearch);
        }

        target_->PopAxisAlignedClip();
```
with:
```cpp
        if (settingsCategory_ == SettingsCategory::All || settingsCategory_ == SettingsCategory::Search) {
            const float hY = (settingsCategory_ == SettingsCategory::All) ? 421.0f : 16.0f;
            const float cY = (settingsCategory_ == SettingsCategory::All) ? 441.0f : 36.0f;
            drawCard(L"SEARCH & FEATURES", hY, cY, 2);

            DrawSettingsRow(7, cY + offsetY, L"File search",
                L"Search files and folders on your computer", {}, true, settings_.enableFileSearch);
            DrawSettingsRow(8, cY + kSettingsRowHeight + offsetY, L"Web search",
                L"Open Google when no results match your query", {}, true, settings_.enableWebSearch);
        }

        if (settingsCategory_ == SettingsCategory::All || settingsCategory_ == SettingsCategory::Vault) {
            const float hY = (settingsCategory_ == SettingsCategory::All) ? 553.0f : 16.0f;
            const float cY = (settingsCategory_ == SettingsCategory::All) ? 573.0f : 36.0f;
            drawCard(L"OBSIDIAN", hY, cY, 1);

            std::wstring vaultValue = L"None found";
            std::wstring vaultDescription = L"No Obsidian vaults found. Install Obsidian and open a vault, then reopen Settings.";
            if (!knownVaults_.empty()) {
                if (obsidianVaultPath_.empty()) {
                    vaultValue = L"Not set";
                    vaultDescription = L"Press Enter to select a detected vault";
                } else {
                    vaultValue = fs::path(obsidianVaultPath_).filename().wstring();
                    vaultDescription = dailyNoteConfig_.found
                        ? L"Tasks are added to today's daily note in this vault"
                        : L"Could not read this vault's daily notes config — using vault root + YYYY-MM-DD.md";
                }
            }
            DrawSettingsRow(9, cY + offsetY, L"Obsidian Vault", vaultDescription, vaultValue,
                false, false, true);
        }

        target_->PopAxisAlignedClip();
```

Replace:
```cpp
        const SettingsCategory categories[] = {
            SettingsCategory::All,
            SettingsCategory::Shortcuts,
            SettingsCategory::System,
            SettingsCategory::Search
        };
        const wchar_t* catLabels[] = {L"All", L"Shortcuts", L"System", L"Search"};
        for (int i = 0; i < 4; ++i) {
```
with:
```cpp
        const SettingsCategory categories[] = {
            SettingsCategory::All,
            SettingsCategory::Shortcuts,
            SettingsCategory::System,
            SettingsCategory::Search,
            SettingsCategory::Vault
        };
        const wchar_t* catLabels[] = {L"All", L"Shortcuts", L"System", L"Search", L"Vault"};
        for (int i = 0; i < 5; ++i) {
```

- [ ] **Step 13: Build**

```bash
cd "D:/GitProjects/lean-launcher"
cmake --build build --config Release --target LeanLauncher
cmake --build build --config Release --target LeanLauncherCoreTests
ctest --test-dir build -C Release --output-on-failure -R LeanLauncherCoreTests
```

Expected: clean build, all existing tests still pass (this task touches no pure-function logic covered by `core_tests.cpp`, so no new automated assertions are added here - the Settings UI itself is verified manually in Task 7).

- [ ] **Step 14: Commit**

```bash
git add -A
git commit -m "Add Obsidian Vault row to Settings

New Vault settings category/tab (row 9) lets the user cycle through
Obsidian's own known-vaults list and persists the choice to
HKCU\\Software\\LeanLauncher\\VaultPath. Reset to Default moves from row 9
to row 10 to make room. DrawSettingsRow gains a plain-text value mode for
displaying a vault path instead of a hotkey badge."
```

---

### Task 7: Manual smoke test against a scratch vault

**Files:** none (verification only, per spec's testing section: "Manual smoke test against a scratch vault before calling v1 done.")

- [ ] **Step 1: Create a throwaway scratch vault**

```bash
mkdir -p "C:/Users/Sascha/AppData/Local/Temp/lean-launcher-scratch-vault/.obsidian"
```

Open this folder as a vault in Obsidian at least once (so it's registered in `%APPDATA%\obsidian\obsidian.json`), then close Obsidian.

- [ ] **Step 2: Run the built executable and confirm no collision with a real Takeoff/Lean Launcher install**

```bash
"D:/GitProjects/lean-launcher/build/Release/LeanLauncher.exe"
```

Confirm: no crash, tray icon appears, global hotkey (Alt+Space by default) opens the launcher window.

- [ ] **Step 3: Verify vault selection in Settings**

Open Settings → Vault tab. Confirm the scratch vault (and any other real vaults on the machine) appear when cycling the "Obsidian Vault" row. Select the scratch vault.

- [ ] **Step 4: Verify the "no vault" path before selecting one**

(Do this before Step 3, or reset the registry value to test it in isolation: `reg delete HKCU\Software\LeanLauncher /v VaultPath /f`.) Type `task test before vault setup` in the launcher. Confirm the result row reads "Set up your vault in Settings" and confirm pressing Enter opens Settings rather than writing anything.

- [ ] **Step 5: Verify the golden path**

With the scratch vault selected, type `task buy milk` and press Enter. Confirm:
- The launcher hides immediately (no Obsidian window ever opens/focuses).
- `C:\Users\Sascha\AppData\Local\Temp\lean-launcher-scratch-vault\<today's date>.md` exists and contains `- [ ] buy milk`.
- Frontmatter (`created:` timestamp) is present since the note didn't exist before.

- [ ] **Step 6: Verify a second task appends without duplicating frontmatter**

Type `task walk the dog`, press Enter. Confirm the note now has both tasks and only one frontmatter block.

- [ ] **Step 7: Verify the Ctrl+K actions menu for a task-add row**

Type `task check calendar` (don't press Enter yet). Press Ctrl+K (or the configured Actions hotkey). Confirm the menu shows "Add to note", "Copy task text", "Open today's note", and that each does what its label says.

- [ ] **Step 8: Verify existing features are undisturbed**

Confirm search still finds installed apps, the calculator still evaluates `2+2`, and Settings' other four tabs (All/Shortcuts/System/Search) and the Reset to Default button still work exactly as before.

- [ ] **Step 9: Clean up the scratch vault**

```bash
rm -rf "C:/Users/Sascha/AppData/Local/Temp/lean-launcher-scratch-vault"
```

- [ ] **Step 10: Mark v1 done**

If all of the above pass, v1 (quick task-add) is complete. No commit needed for this task (verification only) - if any step surfaced a bug, fix it in a new commit and re-run the affected steps before considering v1 done.
