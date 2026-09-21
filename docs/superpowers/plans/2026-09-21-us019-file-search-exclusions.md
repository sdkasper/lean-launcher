# US-019 User-Configurable File Search Exclusions Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a Lean Launcher user add their own folder and file-extension exclusions on top of the app's built-in file-search skip-list, via a plain-text file they edit in their own text editor, with zero new Settings-struct state.

**Architecture:** A new `%LOCALAPPDATA%\LeanLauncher\file_search_excludes.txt` file is parsed once per scan pass into an in-memory `UserExclusions` struct (folder paths + extensions), threaded as an optional parameter into `FileIndex`'s two existing static gate functions (`ShouldSkipDirectory`, `IsUserRelevantFile`) so the built-in hardcoded rules always run first and the user file can only add exclusions, never remove them. Two new Settings rows ("Edit exclusions...", "Help") in the existing hand-rolled row-index UI trigger file creation/opening and a GitHub docs link.

**Tech Stack:** C++17, Win32 (`ShellExecuteW`, `MultiByteToWideChar`), `std::filesystem`, header-only `FileIndex`/`Settings` classes, custom `Check()`-macro test harness (`tests/core_tests.cpp`, built as `LeanLauncherCoreTests` via CTest).

**Spec:** `D:\Lean Notes\01 Projects\LP Products\Lean Launcher\US-019 User-Configurable File Search Exclusions.md` (acceptance criteria + design notes) and `docs/superpowers/specs/2026-09-21-full-disk-file-search-design.md` (parent design spec this US extends).

## Global Constraints

- **Additive-only:** the user file can only add exclusions on top of the hardcoded skip-list (`file_index.h:665` `ShouldSkipDirectory`) and extension allowlist (`file_index.h:128` `kAllowedExtensions`) - it must never be able to widen or re-include anything already excluded (e.g. `system32`). Achieved structurally: every hardcoded check still runs first and unconditionally; the user-exclusion check can only turn a `false` into a `true`, never the reverse.
- **Re-read once per scan pass, not live-watched:** the file is parsed fresh at the top of `BuildIndex()` and `IncrementalRescan()` (both invoked from `WorkerLoop()`, `file_index.h:1393`) - never on every `ShouldSkipDirectory`/`IsUserRelevantFile` call, and never via a filesystem watcher.
- **Static test-callable functions stay static:** `ShouldSkipDirectory` and `IsUserRelevantFile` are called with no `FileIndex` instance from ~40 existing tests in `tests/core_tests.cpp` (e.g. `FileIndex::ShouldSkipDirectory(L"System32")`). Any signature change must keep those call sites compiling unchanged - add an optional parameter defaulted to `nullptr`, never convert to non-static instance methods.
- **No new `Settings` struct field:** the two new Settings rows are pure actions (open a file, open a URL) with nothing to persist via `SaveSettings()` - confirmed by reading `src/settings.h` and the row-click precedents `kRowVaultPicker`/`kRowAboutGithubLink`, neither of which has a `SettingsTextFieldForRow` entry.
- **Path convention:** mirror `FileIndex::DefaultCachePath()` (`file_index.h:173-182`) exactly for `%LOCALAPPDATA%\LeanLauncher\file_search_excludes.txt` - same directory, same `GetEnvironmentVariableW(L"LOCALAPPDATA", ...)` + `create_directories` pattern, sibling of `file_index.cache` (not nested under `updates\`).
- **Text encoding:** the exclusions file must round-trip UTF-8 correctly (Notepad on Windows 10 1809+ saves new `.txt` files as UTF-8; users may have non-ASCII folder names). Read/write via `MultiByteToWideChar`/`WideCharToMultiByte` (`CP_UTF8`), not `std::wifstream`/`std::wofstream` locale conversion - mirrors the existing (but not directly reusable, see Task 1) precedent in `src/obsidian_config.h:90-119`.
- **Scoped test scans never touch real `%LOCALAPPDATA%`:** mirror the existing `cachePath_` safety rule in `FileIndex::Start()` (`file_index.h:202-212`) - when `scanRootOverride` is set and no explicit exclusions-path override is given, the effective exclusions path must be empty (no exclusions loaded), never silently fall back to the real user's file.
- Build/verify commands (from `LOOP.md`, run in this order, full paths since these tools are not on `PATH`):
  ```
  "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build cmake --config Release
  "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe" --test-dir cmake -C Release --output-on-failure
  ```
  (Run from `D:\GitProjects\lean-launcher`.)

---

### Task 1: `UserExclusions` struct + exclusions-file parser

**Files:**
- Modify: `src/file_index.h` (namespace-scope struct near `kAllowedExtensions`, new public static methods on `FileIndex`)
- Test: `tests/core_tests.cpp`

**Interfaces:**
- Produces: `struct UserExclusions { std::vector<std::wstring> excludedFolders; std::unordered_set<std::wstring> excludedExtensions; };` (namespace `takeoff`, defined before `class FileIndex`)
- Produces: `static std::wstring FileIndex::DefaultExclusionsPath()`
- Produces: `static std::wstring FileIndex::NormalizeForCompare(std::wstring s)` (lowercase, `/`→`\`, trailing `\` trimmed)
- Produces: `static UserExclusions FileIndex::LoadUserExclusions(const std::wstring& path)`
- Consumes: nothing from other tasks (this task is a self-contained foundation).

- [ ] **Step 1: Write the failing tests**

Add to `tests/core_tests.cpp`, after the existing `IsUserRelevantFile` extension-allowlist tests (the block ending around the `reject .mui` check, before the next `// ---` section divider - search for `"reject .mui"` to find the spot):

```cpp
    // -----------------------------------------------------------------------------
    // UserExclusions: exclusions-file parsing (US-019)
    // -----------------------------------------------------------------------------
    {
        wchar_t tempDirBuf[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tempDirBuf);
        fs::path scratchDir = fs::path(tempDirBuf) / L"llfi_exclusions_parse_test";
        std::error_code ec;
        fs::create_directories(scratchDir, ec);

        auto writeExclusionsFile = [&](const std::wstring& name, const std::string& utf8Content) {
            const std::wstring path = (scratchDir / name).wstring();
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << utf8Content;
            out.close();
            return path;
        };

        const std::wstring emptyPath = writeExclusionsFile(L"empty.txt", "");
        auto emptyResult = takeoff::FileIndex::LoadUserExclusions(emptyPath);
        Check(emptyResult.excludedFolders.empty() && emptyResult.excludedExtensions.empty(),
              "LoadUserExclusions: empty file yields no exclusions");

        Check(takeoff::FileIndex::LoadUserExclusions(L"").excludedFolders.empty(),
              "LoadUserExclusions: empty path yields no exclusions, no crash");
        Check(takeoff::FileIndex::LoadUserExclusions((scratchDir / L"does_not_exist.txt").wstring())
                  .excludedFolders.empty(),
              "LoadUserExclusions: missing file yields no exclusions, no crash");

        const std::string mixedContent =
            "# a comment line\r\n"
            "\r\n"
            "   \r\n"
            "D:\\Personal Archive\r\n"
            "d:\\already\\lower\\\r\n"
            "\\\\NAS\\Backups\r\n"
            "/mnt/data\r\n"
            ".iso\r\n"
            ".ISO\r\n"
            "relative\\path\r\n"
            "justaword\r\n"
            ".\r\n"
            "..hidden\r\n";
        const std::wstring mixedPath = writeExclusionsFile(L"mixed.txt", mixedContent);
        auto mixed = takeoff::FileIndex::LoadUserExclusions(mixedPath);
        Check(mixed.excludedFolders.size() == 4,
              "LoadUserExclusions: 4 folder-shaped lines recognized (drive letter x2, UNC, leading /)");
        Check(std::find(mixed.excludedFolders.begin(), mixed.excludedFolders.end(),
                  L"d:\\personal archive") != mixed.excludedFolders.end(),
              "LoadUserExclusions: folder path normalized to lowercase, backslashes, no trailing slash");
        Check(mixed.excludedExtensions.size() == 1 && mixed.excludedExtensions.count(L".iso") == 1,
              "LoadUserExclusions: .iso and .ISO both fold into a single lowercase .iso entry");
        Check(mixed.excludedExtensions.count(L".") == 0,
              "LoadUserExclusions: a bare '.' line is ambiguous and silently ignored");

        // Non-ASCII round-trip: UTF-8 bytes for "D:\Résumé" -> decoded wide path.
        const std::string utf8Accented = "D:\\R\xC3\xA9sum\xC3\xA9\r\n";
        const std::wstring accentedPath = writeExclusionsFile(L"accented.txt", utf8Accented);
        auto accented = takeoff::FileIndex::LoadUserExclusions(accentedPath);
        Check(accented.excludedFolders.size() == 1 &&
                  accented.excludedFolders[0] == L"d:\\r\u00e9sum\u00e9",
              "LoadUserExclusions: UTF-8 accented folder path decodes and normalizes correctly");

        Check(takeoff::FileIndex::DefaultExclusionsPath().find(L"file_search_excludes.txt") != std::wstring::npos,
              "DefaultExclusionsPath: points at file_search_excludes.txt");

        fs::remove_all(scratchDir, ec);
    }
```

- [ ] **Step 2: Run tests to verify they fail to compile**

Run: `"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build cmake --config Release`
Expected: FAIL - `UserExclusions`, `LoadUserExclusions`, `DefaultExclusionsPath` are not members of `FileIndex` yet.

- [ ] **Step 3: Add `UserExclusions` struct at namespace scope**

In `src/file_index.h`, insert immediately before `class FileIndex {` (currently line 160, right after the `kAllowedExtensions` set closes at line 158):

```cpp
// Additive-only, user-supplied exclusions layered on top of the hardcoded
// skip-list (ShouldSkipDirectory) and extension allowlist (IsUserRelevantFile)
// - see UserExclusions (US-019). Never used to re-include anything the
// hardcoded rules already exclude; every call site runs the hardcoded check
// first and unconditionally.
struct UserExclusions {
    std::vector<std::wstring> excludedFolders;   // NormalizeForCompare'd: lowercase, '\' separators, no trailing '\'
    std::unordered_set<std::wstring> excludedExtensions; // lowercase, includes leading '.'
};

class FileIndex {
```

(Delete the original `class FileIndex {` line once the struct is inserted above it - do not duplicate it.)

- [ ] **Step 4: Add `DefaultExclusionsPath()`, `NormalizeForCompare()`, and the UTF-8 helpers**

In `src/file_index.h`, insert immediately after `DefaultCachePath()`'s closing brace (currently ends at line 182, just before the `// scanRootOverride is test-only` comment that precedes `Start()`):

```cpp
    static std::wstring DefaultExclusionsPath() {
        wchar_t localAppData[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) > 0 && localAppData[0]) {
            std::filesystem::path dir = std::filesystem::path(localAppData) / L"LeanLauncher";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            return (dir / L"file_search_excludes.txt").wstring();
        }
        return L"";
    }

    // Lowercase, '/' -> '\', trailing '\' trimmed - the same comparison shape
    // ShouldSkipDirectory's Windows-directory check already uses inline, made
    // reusable here since both exclusions-file loading and folder-exclusion
    // matching need it.
    static std::wstring NormalizeForCompare(std::wstring s) {
        std::transform(s.begin(), s.end(), s.begin(),
            [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
        std::replace(s.begin(), s.end(), L'/', L'\\');
        while (!s.empty() && s.back() == L'\\') s.pop_back();
        return s;
    }

    // True if normalizedCandidate is normalizedBase itself or anywhere under it.
    // Both arguments must already be NormalizeForCompare'd.
    static bool IsPathUnderNormalizedFolder(const std::wstring& normalizedCandidate,
                                             const std::wstring& normalizedBase) {
        if (normalizedBase.empty()) return false;
        if (normalizedCandidate == normalizedBase) return true;
        return normalizedCandidate.size() > normalizedBase.size() &&
            normalizedCandidate.compare(0, normalizedBase.size(), normalizedBase) == 0 &&
            normalizedCandidate[normalizedBase.size()] == L'\\';
    }

    // file_index.h is deliberately self-contained (no include of
    // obsidian_config.h, which has its own copy of this exact UTF-8<->UTF-16
    // idiom) - duplicated locally rather than introducing a cross-header
    // dependency for two small functions.
    static std::wstring Utf8BytesToWide(const std::string& utf8) {
        if (utf8.empty()) return L"";
        const int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        if (wlen <= 0) return L"";
        std::wstring out(static_cast<size_t>(wlen), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), wlen);
        return out;
    }

    static std::string WideToUtf8Bytes(const std::wstring& wide) {
        if (wide.empty()) return {};
        const int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
            nullptr, 0, nullptr, nullptr);
        if (len <= 0) return {};
        std::string out(static_cast<size_t>(len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), len, nullptr, nullptr);
        return out;
    }

    // Parses one line per folder or extension exclusion. A line is a folder
    // exclusion if it starts with a drive letter ("D:"), "\\" (UNC), or "/";
    // an extension exclusion if it starts with "." and contains no path
    // separator or whitespace after that. Blank lines, "#" comments, and
    // anything else (relative paths, bare words) are silently ignored - this
    // file is additive-only, so there is no "include" syntax to parse at all.
    static UserExclusions LoadUserExclusions(const std::wstring& path) {
        UserExclusions result;
        if (path.empty()) return result;
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) return result;
        std::ifstream file(path, std::ios::binary);
        if (!file) return result;
        std::ostringstream ss;
        ss << file.rdbuf();
        const std::wstring content = Utf8BytesToWide(ss.str());

        size_t pos = 0;
        while (pos <= content.size()) {
            size_t nl = content.find(L'\n', pos);
            std::wstring line = (nl == std::wstring::npos) ? content.substr(pos) : content.substr(pos, nl - pos);
            pos = (nl == std::wstring::npos) ? content.size() + 1 : nl + 1;

            size_t start = line.find_first_not_of(L" \t\r");
            if (start == std::wstring::npos) continue;
            size_t end = line.find_last_not_of(L" \t\r");
            std::wstring trimmed = line.substr(start, end - start + 1);
            if (trimmed.empty() || trimmed[0] == L'#') continue;

            const bool looksLikeFolder =
                (trimmed.size() >= 2 && iswalpha(trimmed[0]) && trimmed[1] == L':') ||
                trimmed.rfind(L"\\\\", 0) == 0 ||
                trimmed[0] == L'/';
            if (looksLikeFolder) {
                result.excludedFolders.push_back(NormalizeForCompare(trimmed));
                continue;
            }

            if (trimmed[0] == L'.' && trimmed.size() > 1 &&
                trimmed.find_first_of(L"\\/ \t") == std::wstring::npos) {
                std::wstring ext = trimmed;
                std::transform(ext.begin(), ext.end(), ext.begin(),
                    [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
                result.excludedExtensions.insert(ext);
            }
        }
        return result;
    }

```

Add `#include <sstream>` to `src/file_index.h`'s include block (currently missing it - `ostringstream` is used above) next to the existing `#include <fstream>`.

- [ ] **Step 4: Run tests to verify they pass**

Run: `"...cmake.exe" --build cmake --config Release` then `"...ctest.exe" --test-dir cmake -C Release --output-on-failure`
Expected: `LeanLauncherCoreTests` PASS, including every new `LoadUserExclusions`/`DefaultExclusionsPath` check.

- [ ] **Step 5: Commit**

```bash
git add src/file_index.h tests/core_tests.cpp
git commit -m "Add UserExclusions struct and exclusions-file parser (US-019)"
```

---

### Task 2: Auto-create the exclusions file with a header template

**Files:**
- Modify: `src/file_index.h`
- Test: `tests/core_tests.cpp`

**Interfaces:**
- Consumes: nothing new from Task 1 (independent of parsing).
- Produces: `static bool FileIndex::EnsureExclusionsFileWithHeader(const std::wstring& path)` - creates the file (with parent directories) and writes the header template if it doesn't already exist; returns `true` if the file exists on return (either already did, or was just created), `false` on I/O failure or empty path.

- [ ] **Step 1: Write the failing tests**

Add to `tests/core_tests.cpp`, directly after Task 1's `UserExclusions` test block:

```cpp
    // -----------------------------------------------------------------------------
    // EnsureExclusionsFileWithHeader (US-019)
    // -----------------------------------------------------------------------------
    {
        wchar_t tempDirBuf[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tempDirBuf);
        fs::path scratchDir = fs::path(tempDirBuf) / L"llfi_exclusions_create_test";
        std::error_code ec;
        fs::remove_all(scratchDir, ec);

        const std::wstring newFilePath = (scratchDir / L"nested" / L"file_search_excludes.txt").wstring();
        Check(!fs::exists(newFilePath, ec), "Setup: exclusions file does not exist yet");
        Check(takeoff::FileIndex::EnsureExclusionsFileWithHeader(newFilePath),
              "EnsureExclusionsFileWithHeader creates a missing file (and its parent dir)");
        Check(fs::exists(newFilePath, ec), "EnsureExclusionsFileWithHeader: file now exists");

        auto readAll = [](const std::wstring& p) {
            std::ifstream f(p, std::ios::binary);
            std::ostringstream ss;
            ss << f.rdbuf();
            return ss.str();
        };
        const std::string headerContent = readAll(newFilePath);
        Check(headerContent.find("file_search_excludes") != std::string::npos ||
              headerContent.find("exclusion") != std::string::npos,
              "EnsureExclusionsFileWithHeader: header text describes the file's purpose");
        Check(headerContent.find(".iso") != std::string::npos,
              "EnsureExclusionsFileWithHeader: header shows an extension-exclusion example");

        // Second call on an already-existing file must not clobber user edits.
        {
            std::ofstream userEdit(newFilePath, std::ios::binary | std::ios::app);
            userEdit << "\r\nD:\\MyStuff\r\n";
        }
        const std::string beforeSecondCall = readAll(newFilePath);
        Check(takeoff::FileIndex::EnsureExclusionsFileWithHeader(newFilePath),
              "EnsureExclusionsFileWithHeader on an existing file returns true (no-op)");
        Check(readAll(newFilePath) == beforeSecondCall,
              "EnsureExclusionsFileWithHeader does not overwrite an already-existing file");

        Check(!takeoff::FileIndex::EnsureExclusionsFileWithHeader(L""),
              "EnsureExclusionsFileWithHeader: empty path fails cleanly, no crash");

        fs::remove_all(scratchDir, ec);
    }
```

- [ ] **Step 2: Run tests to verify they fail to compile**

Run: `"...cmake.exe" --build cmake --config Release`
Expected: FAIL - `EnsureExclusionsFileWithHeader` is not a member of `FileIndex` yet.

- [ ] **Step 3: Implement `EnsureExclusionsFileWithHeader`**

In `src/file_index.h`, insert directly after `LoadUserExclusions`'s closing brace (added in Task 1):

```cpp
    static constexpr const wchar_t* kExclusionsFileHeader =
        L"# Lean Launcher file search exclusions\r\n"
        L"# One entry per line, on top of the app's built-in system/build-folder\r\n"
        L"# exclusions. This file is additive only - it cannot un-exclude anything\r\n"
        L"# the app already skips (e.g. system32).\r\n"
        L"#\r\n"
        L"# Folder exclusion: a full path starting with a drive letter, \\\\ (UNC), or /\r\n"
        L"#   D:\\Personal Archive\r\n"
        L"#   \\\\NAS\\Backups\r\n"
        L"#\r\n"
        L"# Extension exclusion: a dot followed by the extension, nothing else\r\n"
        L"#   .iso\r\n"
        L"#\r\n"
        L"# Blank lines, lines starting with #, and anything else are ignored.\r\n"
        L"# Changes take effect on the next scan pass - no restart needed.\r\n"
        L"\r\n";

    // Creates path (and its parent directory) with the header template above
    // if it doesn't already exist. Never touches an existing file - called
    // every time the user clicks "Edit exclusions..." in Settings, so a
    // second/third click must be a pure no-op against their saved edits.
    static bool EnsureExclusionsFileWithHeader(const std::wstring& path) {
        if (path.empty()) return false;
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) return true;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        const std::string utf8Header = WideToUtf8Bytes(kExclusionsFileHeader);
        out << utf8Header;
        return true;
    }

```

- [ ] **Step 4: Run tests to verify they pass**

Run: `"...cmake.exe" --build cmake --config Release` then `"...ctest.exe" --test-dir cmake -C Release --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/file_index.h tests/core_tests.cpp
git commit -m "Add EnsureExclusionsFileWithHeader for first-time exclusions file creation (US-019)"
```

---

### Task 3: Wire folder exclusions into `ShouldSkipDirectory`

**Files:**
- Modify: `src/file_index.h`
- Test: `tests/core_tests.cpp`

**Interfaces:**
- Consumes: `UserExclusions`, `NormalizeForCompare`, `IsPathUnderNormalizedFolder` (Task 1).
- Produces: `static bool ShouldSkipDirectory(const fs::path& dirPath, const UserExclusions* userExclusions = nullptr)` - existing signature gains one defaulted parameter; all ~15 existing no-instance test call sites keep compiling and passing unchanged.

- [ ] **Step 1: Write the failing tests**

Add to `tests/core_tests.cpp`, directly after the existing `ShouldSkipDirectory` hardcoded-skip-list tests (the block ending at `"User project path not excluded"` - search for that string to find the spot, insert right after it and before the `FindVerifiedProjectRoot` tests that follow):

```cpp
    // User-supplied folder exclusions, additive on top of the hardcoded list (US-019)
    {
        takeoff::UserExclusions excl;
        excl.excludedFolders.push_back(L"d:\\personal archive");

        Check(!takeoff::FileIndex::ShouldSkipDirectory(L"D:\\Personal Archive"),
              "ShouldSkipDirectory with no userExclusions arg: unaffected by user exclusions (default nullptr)");
        Check(takeoff::FileIndex::ShouldSkipDirectory(L"D:\\Personal Archive", &excl),
              "ShouldSkipDirectory: exact-match user-excluded folder is skipped");
        Check(takeoff::FileIndex::ShouldSkipDirectory(L"D:\\Personal Archive\\Sub\\Deeper", &excl),
              "ShouldSkipDirectory: subtree of a user-excluded folder is skipped");
        Check(takeoff::FileIndex::ShouldSkipDirectory(L"d:\\PERSONAL ARCHIVE", &excl),
              "ShouldSkipDirectory: user-excluded folder match is case-insensitive");
        Check(!takeoff::FileIndex::ShouldSkipDirectory(L"D:\\Personal Archive2", &excl),
              "ShouldSkipDirectory: a folder that merely shares a prefix is NOT excluded (boundary check)");
        Check(!takeoff::FileIndex::ShouldSkipDirectory(L"D:\\Other Folder", &excl),
              "ShouldSkipDirectory: unrelated folder is unaffected by user exclusions");

        // Additive-only: a user exclusion list cannot un-exclude a hardcoded one.
        takeoff::UserExclusions emptyExcl;
        Check(takeoff::FileIndex::ShouldSkipDirectory(L"C:\\Windows\\System32", &emptyExcl),
              "ShouldSkipDirectory: hardcoded exclusion (system32) still applies with a UserExclusions* present");
    }
```

- [ ] **Step 2: Run tests to verify they fail to compile**

Run: `"...cmake.exe" --build cmake --config Release`
Expected: FAIL - `ShouldSkipDirectory` does not accept a second argument yet.

- [ ] **Step 3: Extend `ShouldSkipDirectory`'s signature and add the user-exclusion check**

In `src/file_index.h`, change the signature line (currently line 665):

```cpp
    static bool ShouldSkipDirectory(const fs::path& dirPath) {
```
to:
```cpp
    static bool ShouldSkipDirectory(const fs::path& dirPath, const UserExclusions* userExclusions = nullptr) {
```

Then, immediately before the function's final `return false;` (currently line 719, the line right after the long hardcoded-name `if` block closes at line 718), insert:

```cpp
        if (userExclusions && !userExclusions->excludedFolders.empty()) {
            const std::wstring normalizedCandidate = NormalizeForCompare(dirPath.wstring());
            for (const auto& excluded : userExclusions->excludedFolders) {
                if (IsPathUnderNormalizedFolder(normalizedCandidate, excluded)) return true;
            }
        }
        return false;
```

(This replaces the old bare `return false;` - the hardcoded checks above are untouched, so they always run first regardless of `userExclusions`.)

- [ ] **Step 4: Run tests to verify they pass**

Run: `"...cmake.exe" --build cmake --config Release` then `"...ctest.exe" --test-dir cmake -C Release --output-on-failure`
Expected: PASS, including all pre-existing `ShouldSkipDirectory` tests (proving the default-`nullptr` path is unchanged) and the new ones.

- [ ] **Step 5: Commit**

```bash
git add src/file_index.h tests/core_tests.cpp
git commit -m "Add user-supplied folder exclusions to ShouldSkipDirectory (US-019)"
```

---

### Task 4: Wire extension exclusions into `IsUserRelevantFile`

**Files:**
- Modify: `src/file_index.h`
- Test: `tests/core_tests.cpp`

**Interfaces:**
- Consumes: `UserExclusions` (Task 1).
- Produces: `static bool IsUserRelevantFile(const fs::path& filePath, const UserExclusions* userExclusions = nullptr)` - existing signature gains one defaulted parameter; all ~25 existing no-instance test call sites keep compiling and passing unchanged.

- [ ] **Step 1: Write the failing tests**

Add to `tests/core_tests.cpp`, directly after the existing `IsUserRelevantFile` tests (the block ending at `"reject .mui"` / `"reject strings.mui"` - insert right after the last `IsUserRelevantFile` reject-check and before Task 1's new `UserExclusions` parsing block, so this sits between the built-in allowlist tests and the parser tests):

```cpp
    // User-supplied extension exclusions, additive on top of the allowlist (US-019)
    {
        takeoff::UserExclusions excl;
        excl.excludedExtensions.insert(L".iso");

        Check(takeoff::FileIndex::IsUserRelevantFile(L"disk.iso"),
              "IsUserRelevantFile with no userExclusions arg: .iso still allowed (default nullptr)");
        Check(!takeoff::FileIndex::IsUserRelevantFile(L"disk.iso", &excl),
              "IsUserRelevantFile: user-excluded extension is rejected");
        Check(!takeoff::FileIndex::IsUserRelevantFile(L"DISK.ISO", &excl),
              "IsUserRelevantFile: user-excluded extension match is case-insensitive");
        Check(takeoff::FileIndex::IsUserRelevantFile(L"document.pdf", &excl),
              "IsUserRelevantFile: an unrelated allowed extension is unaffected");

        // Additive-only: a user exclusion list cannot widen the allowlist -
        // an extension not already in kAllowedExtensions stays rejected
        // regardless of what's in (or absent from) userExclusions.
        takeoff::UserExclusions emptyExcl;
        Check(!takeoff::FileIndex::IsUserRelevantFile(L"driver.inf", &emptyExcl),
              "IsUserRelevantFile: hardcoded allowlist rejection (.inf) still applies with a UserExclusions* present");
    }
```

- [ ] **Step 2: Run tests to verify they fail to compile**

Run: `"...cmake.exe" --build cmake --config Release`
Expected: FAIL - `IsUserRelevantFile` does not accept a second argument yet.

- [ ] **Step 3: Extend `IsUserRelevantFile`'s signature and add the user-exclusion check**

In `src/file_index.h`, change the signature line (currently line 819):

```cpp
    static bool IsUserRelevantFile(const fs::path& filePath) {
```
to:
```cpp
    static bool IsUserRelevantFile(const fs::path& filePath, const UserExclusions* userExclusions = nullptr) {
```

Then change the function's final line (currently line 834):

```cpp
        return kAllowedExtensions.find(std::wstring_view(lowerExt)) != kAllowedExtensions.end();
```
to:
```cpp
        if (kAllowedExtensions.find(std::wstring_view(lowerExt)) == kAllowedExtensions.end()) return false;
        if (userExclusions && userExclusions->excludedExtensions.count(lowerExt)) return false;
        return true;
```

(The extensionless `makefile`/`dockerfile`/`license`/`readme` branch above is untouched - those files have no extension to match against `excludedExtensions`, matching the AC which only defines extension-shaped exclusion lines.)

- [ ] **Step 4: Run tests to verify they pass**

Run: `"...cmake.exe" --build cmake --config Release` then `"...ctest.exe" --test-dir cmake -C Release --output-on-failure`
Expected: PASS, including all pre-existing `IsUserRelevantFile` tests and the new ones.

- [ ] **Step 5: Commit**

```bash
git add src/file_index.h tests/core_tests.cpp
git commit -m "Add user-supplied extension exclusions to IsUserRelevantFile (US-019)"
```

---

### Task 5: Load exclusions once per scan pass and thread them through every scan call site

**Files:**
- Modify: `src/file_index.h`
- Test: `tests/core_tests.cpp`

**Interfaces:**
- Consumes: `UserExclusions`, `LoadUserExclusions`, `DefaultExclusionsPath` (Task 1); the now-two-argument `ShouldSkipDirectory`/`IsUserRelevantFile` (Tasks 3-4).
- Produces: `FileIndex::Start()` gains a 4th defaulted parameter `exclusionsPathOverride`; a private `userExclusions_` member reflects the most-recently-loaded exclusions after any `BuildIndex()`/`IncrementalRescan()` pass.

- [ ] **Step 1: Write the failing test**

Add to `tests/core_tests.cpp`, directly after the existing "Incremental rescan" scoped-scan test block (search for `"Incremental rescan:"` to find that section, and insert this new block immediately after its closing `}`):

```cpp
    // -----------------------------------------------------------------------------
    // End-to-end: user exclusions are honored during a scoped BuildIndex() pass (US-019)
    // -----------------------------------------------------------------------------
    {
        FileIndex::Instance().Stop();
        FileIndex::Instance().ResetForTest();

        fs::path testRoot = fs::current_path() / L"llfi_user_exclusions_test_root";
        fs::path excludedSub = testRoot / L"excluded_sub";
        std::error_code ec;
        fs::remove_all(testRoot, ec);
        fs::create_directories(excludedSub, ec);
        {
            std::ofstream keep((testRoot / L"keep.txt").wstring());
            keep << "keep";
        }
        {
            std::ofstream excludedByExt((testRoot / L"movie.iso").wstring());
            excludedByExt << "iso";
        }
        {
            std::ofstream buried((excludedSub / L"buried.txt").wstring());
            buried << "buried";
        }

        wchar_t tempDirBuf[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tempDirBuf);
        fs::path scratchDir = fs::path(tempDirBuf) / L"llfi_user_exclusions_scratch";
        fs::create_directories(scratchDir, ec);
        const std::wstring exclusionsPath = (scratchDir / L"exclusions_e2e_test.txt").wstring();
        {
            std::ofstream out(exclusionsPath, std::ios::binary | std::ios::trunc);
            out << "# test exclusions\r\n"
                << takeoff::FileIndex::WideToUtf8Bytes(excludedSub.wstring()) << "\r\n"
                << ".iso\r\n";
        }
        const std::wstring cachePath = (scratchDir / L"exclusions_e2e_test_cache.bin").wstring();
        DeleteFileW(cachePath.c_str());

        auto waitSettled = []() {
            int stable = 0;
            for (int w = 0; w < 60 && stable < 3; ++w) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                if (FileIndex::Instance().IsReady() &&
                    FileIndex::Instance().GetPhase() == takeoff::FileIndex::Phase::Loaded) {
                    ++stable;
                } else {
                    stable = 0;
                }
            }
        };

        FileIndex::Instance().Start(nullptr, testRoot.wstring(), cachePath, exclusionsPath);
        waitSettled();
        FileIndex::Instance().Stop();

        auto keepResults = FileIndex::Instance().Search(L"keep.txt");
        Check(!keepResults.empty(), "US-019 e2e: non-excluded file is indexed and findable");

        auto isoResults = FileIndex::Instance().Search(L"movie.iso");
        Check(isoResults.empty(), "US-019 e2e: file with a user-excluded extension is not indexed");

        auto buriedResults = FileIndex::Instance().Search(L"buried.txt");
        Check(buriedResults.empty(), "US-019 e2e: file inside a user-excluded folder is not indexed");

        FileIndex::Instance().ResetForTest();
        fs::remove_all(testRoot, ec);
        fs::remove_all(scratchDir, ec);
    }
```

- [ ] **Step 2: Run test to verify it fails to compile**

Run: `"...cmake.exe" --build cmake --config Release`
Expected: FAIL - `Start()` does not accept a 4th argument yet, and it would pass even without the wiring below since exclusions aren't loaded at all yet.

- [ ] **Step 3: Add the `exclusionsPath_`/`userExclusions_` members and `ReloadUserExclusions()`**

In `src/file_index.h`, change the member declarations at the end of the class (currently lines 1476-1479):

```cpp
    std::wstring scanRootOverride_;
    // The effective cache path for this Start() cycle - empty means "this
    // cycle has no cache" (a scoped scan without an explicit cache path).
    std::wstring cachePath_;
};
```
to:
```cpp
    std::wstring scanRootOverride_;
    // The effective cache path for this Start() cycle - empty means "this
    // cycle has no cache" (a scoped scan without an explicit cache path).
    std::wstring cachePath_;
    // The effective exclusions-file path for this Start() cycle - empty means
    // "this cycle has no user exclusions" (a scoped scan without an explicit
    // exclusions path; see ReloadUserExclusions()). Same safety rule as
    // cachePath_ above - a scoped test scan must never read the real user's
    // exclusions file.
    std::wstring exclusionsPath_;
    // Reloaded fresh at the top of every BuildIndex()/IncrementalRescan()
    // pass by ReloadUserExclusions() - never mutated anywhere else, and only
    // ever read by the worker thread, so it needs no mutex_ protection (same
    // reasoning as scanRootOverride_/cachePath_).
    UserExclusions userExclusions_;
};
```

Then change `Start()`'s signature and body (currently lines 192-215):

```cpp
    void Start(HWND notifyHwnd = nullptr, const std::wstring& scanRootOverride = L"",
               const std::wstring& cachePathOverride = L"") {
        if (running_.exchange(true)) return;
        // A prior cycle (e.g. a direct LoadIndexCache() call, or this same
        // singleton's previous Start()/Stop() pair) may have left ready_
        // set to true; reset it so IsReady() reflects this cycle's worker
        // progress, not leftover state from before this Start() call.
        ready_ = false;
        notifyHwnd_ = notifyHwnd;
        scanRootOverride_ = scanRootOverride;
        // A scoped (test) scan must never read or write the production
        // cache: reading it would silently replace the scoped index with
        // the machine-wide one, and writing it would replace the user's
        // real index with a single directory tree - permanently, since a
        // successful cache load means BuildIndex() never runs again. So
        // when a scan root is overridden and no explicit cache path is
        // given, this cycle simply has no cache (every use site already
        // guards on the path being empty).
        cachePath_ = !cachePathOverride.empty() ? cachePathOverride
                   : scanRootOverride.empty()   ? DefaultCachePath()
                                                : std::wstring{};
        stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        worker_ = std::thread([this]() { WorkerLoop(); });
    }
```
to:
```cpp
    void Start(HWND notifyHwnd = nullptr, const std::wstring& scanRootOverride = L"",
               const std::wstring& cachePathOverride = L"", const std::wstring& exclusionsPathOverride = L"") {
        if (running_.exchange(true)) return;
        // A prior cycle (e.g. a direct LoadIndexCache() call, or this same
        // singleton's previous Start()/Stop() pair) may have left ready_
        // set to true; reset it so IsReady() reflects this cycle's worker
        // progress, not leftover state from before this Start() call.
        ready_ = false;
        notifyHwnd_ = notifyHwnd;
        scanRootOverride_ = scanRootOverride;
        // A scoped (test) scan must never read or write the production
        // cache: reading it would silently replace the scoped index with
        // the machine-wide one, and writing it would replace the user's
        // real index with a single directory tree - permanently, since a
        // successful cache load means BuildIndex() never runs again. So
        // when a scan root is overridden and no explicit cache path is
        // given, this cycle simply has no cache (every use site already
        // guards on the path being empty).
        cachePath_ = !cachePathOverride.empty() ? cachePathOverride
                   : scanRootOverride.empty()   ? DefaultCachePath()
                                                : std::wstring{};
        // Same reasoning as cachePath_ above, applied to the exclusions file:
        // a scoped test scan must never depend on whatever the real user has
        // configured in %LOCALAPPDATA%\LeanLauncher\file_search_excludes.txt.
        exclusionsPath_ = !exclusionsPathOverride.empty() ? exclusionsPathOverride
                         : scanRootOverride.empty()       ? DefaultExclusionsPath()
                                                           : std::wstring{};
        stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        worker_ = std::thread([this]() { WorkerLoop(); });
    }
```

Add a private `ReloadUserExclusions()` method immediately before `BuildIndex()` (currently line 1242):

```cpp
    // Re-reads the exclusions file fresh - called once at the top of every
    // BuildIndex()/IncrementalRescan() pass (never on every
    // ShouldSkipDirectory/IsUserRelevantFile call, and never via a
    // filesystem watcher - see US-019's "re-read once per scan pass" AC).
    void ReloadUserExclusions() {
        userExclusions_ = exclusionsPath_.empty() ? UserExclusions{} : LoadUserExclusions(exclusionsPath_);
    }

    void BuildIndex() {
```
(This replaces the plain `void BuildIndex() {` line - don't duplicate it.)

- [ ] **Step 4: Call `ReloadUserExclusions()` at the top of each pass**

In `src/file_index.h`, change `BuildIndex()`'s first line (currently `phase_ = Phase::FirstWalk;` right after the function signature) to:

```cpp
    void BuildIndex() {
        phase_ = Phase::FirstWalk;
        ReloadUserExclusions();
```

And `IncrementalRescan()`'s first line (currently `phase_ = Phase::IncrementalRescan;`) to:

```cpp
    void IncrementalRescan() {
        phase_ = Phase::IncrementalRescan;
        ReloadUserExclusions();
```

- [ ] **Step 5: Thread `&userExclusions_` through every in-class call site**

In `src/file_index.h`, make these 10 exact edits (each adds `, &userExclusions_` as the call's last argument; all are inside instance methods of `FileIndex`, so `userExclusions_` is directly reachable):

In `IncrementalRescan()`'s inner re-listing loop:
```cpp
                        if (ShouldSkipDirectory(child.path())) continue;
```
→
```cpp
                        if (ShouldSkipDirectory(child.path(), &userExclusions_)) continue;
```

```cpp
                    } else if (!ec && child.is_regular_file(ec) && IsUserRelevantFile(child.path())) {
```
→
```cpp
                    } else if (!ec && child.is_regular_file(ec) && IsUserRelevantFile(child.path(), &userExclusions_)) {
```

In `AddItem()`:
```cpp
            if (!IsUserRelevantFile(p)) return;
```
→
```cpp
            if (!IsUserRelevantFile(p, &userExclusions_)) return;
```

In `ScanPath()`:
```cpp
        if (IsDriveRoot(root) || ShouldSkipDirectory(root)) return;
```
→
```cpp
        if (IsDriveRoot(root) || ShouldSkipDirectory(root, &userExclusions_)) return;
```

```cpp
                    if (depth >= maxDepth || ShouldSkipDirectory(entry.path())) {
```
→
```cpp
                    if (depth >= maxDepth || ShouldSkipDirectory(entry.path(), &userExclusions_)) {
```

```cpp
                if (!ec && entry.is_regular_file(ec) && IsUserRelevantFile(entry.path())) {
```
→
```cpp
                if (!ec && entry.is_regular_file(ec) && IsUserRelevantFile(entry.path(), &userExclusions_)) {
```

In `BuildIndex()` phase 2 (`%USERPROFILE%` scan - identified unambiguously by the `profileIdx`/`profileItems` variables, unique to this phase):
```cpp
                        if (!ShouldSkipDirectory(entry.path()) &&
                            _wcsicmp(name.c_str(), L"Desktop") != 0 &&
                            _wcsicmp(name.c_str(), L"Documents") != 0 &&
                            _wcsicmp(name.c_str(), L"Downloads") != 0 &&
                            _wcsicmp(name.c_str(), L"Pictures") != 0 &&
                            _wcsicmp(name.c_str(), L"Music") != 0 &&
                            _wcsicmp(name.c_str(), L"Videos") != 0) {
                            AddItem(entry.path(), true, profileIdx, profileItems, seen);
                            uint32_t childIdx = InternLocked(pool_, entry.path().wstring(), Normalize(entry.path().wstring()));
                            ScanPath(entry.path(), childIdx, pool_, seen, scannedDirs, 8);
                        }
                    } else if (entry.is_regular_file(ec)) {
                        if (IsUserRelevantFile(entry.path())) {
                            AddItem(entry.path(), false, profileIdx, profileItems, seen);
                        }
                    }
```
→
```cpp
                        if (!ShouldSkipDirectory(entry.path(), &userExclusions_) &&
                            _wcsicmp(name.c_str(), L"Desktop") != 0 &&
                            _wcsicmp(name.c_str(), L"Documents") != 0 &&
                            _wcsicmp(name.c_str(), L"Downloads") != 0 &&
                            _wcsicmp(name.c_str(), L"Pictures") != 0 &&
                            _wcsicmp(name.c_str(), L"Music") != 0 &&
                            _wcsicmp(name.c_str(), L"Videos") != 0) {
                            AddItem(entry.path(), true, profileIdx, profileItems, seen);
                            uint32_t childIdx = InternLocked(pool_, entry.path().wstring(), Normalize(entry.path().wstring()));
                            ScanPath(entry.path(), childIdx, pool_, seen, scannedDirs, 8);
                        }
                    } else if (entry.is_regular_file(ec)) {
                        if (IsUserRelevantFile(entry.path(), &userExclusions_)) {
                            AddItem(entry.path(), false, profileIdx, profileItems, seen);
                        }
                    }
```

In `BuildIndex()` phase 3 (drive-root scan - identified unambiguously by the `driveIdx`/`driveItems` variables, unique to this phase):
```cpp
                        if (!ShouldSkipDirectory(entry.path())) {
                            AddItem(entry.path(), true, driveIdx, driveItems, seen);
                            uint32_t childIdx = InternLocked(pool_, entry.path().wstring(), Normalize(entry.path().wstring()));
                            const int maxDepth = isDriveC ? 4 : 8;
                            ScanPath(entry.path(), childIdx, pool_, seen, scannedDirs, maxDepth);
                        }
                    } else if (entry.is_regular_file(ec)) {
                        if (IsUserRelevantFile(entry.path())) {
                            AddItem(entry.path(), false, driveIdx, driveItems, seen);
                        }
                    }
```
→
```cpp
                        if (!ShouldSkipDirectory(entry.path(), &userExclusions_)) {
                            AddItem(entry.path(), true, driveIdx, driveItems, seen);
                            uint32_t childIdx = InternLocked(pool_, entry.path().wstring(), Normalize(entry.path().wstring()));
                            const int maxDepth = isDriveC ? 4 : 8;
                            ScanPath(entry.path(), childIdx, pool_, seen, scannedDirs, maxDepth);
                        }
                    } else if (entry.is_regular_file(ec)) {
                        if (IsUserRelevantFile(entry.path(), &userExclusions_)) {
                            AddItem(entry.path(), false, driveIdx, driveItems, seen);
                        }
                    }
```

- [ ] **Step 6: Run test to verify it passes**

Run: `"...cmake.exe" --build cmake --config Release` then `"...ctest.exe" --test-dir cmake -C Release --output-on-failure`
Expected: PASS, including every pre-existing `FileIndex` test (proving default-argument call sites and the untouched production `Start()` 3-arg call sites in `main.cpp` still behave identically) and the new end-to-end exclusions test.

- [ ] **Step 7: Commit**

```bash
git add src/file_index.h tests/core_tests.cpp
git commit -m "Reload user exclusions once per scan pass and thread them through FileIndex scanning (US-019)"
```

---

### Task 6: `docs/file-search.md`

**Files:**
- Create: `docs/file-search.md`

**Interfaces:**
- Consumes: nothing (pure documentation).
- Produces: a GitHub-rendered Markdown page at `https://github.com/sdkasper/lean-launcher/blob/master/docs/file-search.md` explaining both exclusion layers and the file's line-shape convention - the AC's required target for the new Settings "Help" row (Task 7).

- [ ] **Step 1: Write the file**

Create `docs/file-search.md`:

```markdown
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
```

- [ ] **Step 2: Commit**

```bash
git add docs/file-search.md
git commit -m "Add docs/file-search.md explaining built-in and user file search exclusions (US-019)"
```

---

### Task 7: Settings UI - "Edit exclusions..." and "Help" rows

**Files:**
- Modify: `src/launcher.h`

**Interfaces:**
- Consumes: `FileIndex::DefaultExclusionsPath()`, `FileIndex::EnsureExclusionsFileWithHeader()` (Task 2); `docs/file-search.md`'s GitHub URL (Task 6).
- Produces: two new Settings rows, no new `Settings` struct field, no new test surface (this codebase has no automated UI-click test harness for Settings rows - verify by building and manually exercising Settings → Search in the running app).

This task inserts 2 new rows into the Search category, which shifts every row constant from `kRowObsidianEnabled` onward by +2, and updates every place in `launcher.h` whose row-layout math depends on the Search category's row count. Apply every step below - a partial renumbering leaves the Settings UI broken (wrong click targets, wrong scroll bounds).

- [ ] **Step 1: Renumber the row constants**

In `src/launcher.h`, replace the row-constants block (currently lines 1194-1239):

```cpp
    // Settings rows 0-3: keyboard shortcuts. 4-6: system. 7-12: search (7
    // File search, 8 Web search, 9 Search engine picker - US-016; 10-12 add
    // the File/Web/App search prefix fields - US-017). 13-37: Obsidian
    // (only row 13 is active when settings_.obsidianEnabled is false - see
    // IsRowInCategory/ObsidianRowCount). Each of the five *Summary rows is
    // always shown when Obsidian is enabled; its detail rows only appear
    // while obsidianExpandedSection_ names that section - see
    // ObsidianVisibleRows(). 38 is the About tab's one row (not part of
    // "All" - see IsRowInCategory). 39 is the Reset button, handled as a
    // sentinel row rather than a real settings row.
    // Rows 9-12 are not part of the Obsidian block below - they live in the
    // Search category alongside rows 7-8 - so every Obsidian row constant
    // shifts relative to the row it would otherwise have under a plain
    // contiguous 0.. numbering.
    static constexpr int kRowWebSearchEngine = 9;
    static constexpr int kRowFileSearchPrefix = 10;
    static constexpr int kRowWebSearchPrefix = 11;
    static constexpr int kRowAppSearchPrefix = 12;
    static constexpr int kRowObsidianEnabled = 13;
    static constexpr int kRowVaultPicker = 14;
    static constexpr int kRowVaultSearchSummary = 15;
    static constexpr int kRowVaultSearchEnabled = 16;
    static constexpr int kRowVaultSearchPrefix = 17;
    static constexpr int kRowVaultSearchPillLabel = 18;
    static constexpr int kRowTaskSummary = 19;
    static constexpr int kRowTaskAddEnabled = 20;
    static constexpr int kRowTaskPrefix = 21;
    static constexpr int kRowTaskPillLabel = 22;
    static constexpr int kRowTaskPreviewPrefix = 23;
    static constexpr int kRowNoteAddSummary = 24;
    static constexpr int kRowNoteAddEnabled = 25;
    static constexpr int kRowNoteAddPrefix = 26;
    static constexpr int kRowNoteAddPillLabel = 27;
    static constexpr int kRowNoteAddPreviewPrefix = 28;
    static constexpr int kRowLogSummary = 29;
    static constexpr int kRowLogEnabled = 30;
    static constexpr int kRowLogPrefix = 31;
    static constexpr int kRowLogPillLabel = 32;
    static constexpr int kRowLogPreviewPrefix = 33;
    static constexpr int kRowLogHeading = 34;
    static constexpr int kRowOverridesSummary = 35;
    static constexpr int kRowDailyNoteFolderOverride = 36;
    static constexpr int kRowDailyNoteFormatOverride = 37;
    static constexpr int kRowAboutGithubLink = 38;
    static constexpr int kSettingsMaxRow = 38;
    static constexpr int kRowResetToDefaults = 39;
```

with:

```cpp
    // Settings rows 0-3: keyboard shortcuts. 4-6: system. 7-14: search (7
    // File search, 8 Web search, 9 Search engine picker - US-016; 10-12 add
    // the File/Web/App search prefix fields - US-017; 13-14 add Edit
    // exclusions.../Help - US-019). 15-39: Obsidian (only row 15 is active
    // when settings_.obsidianEnabled is false - see
    // IsRowInCategory/ObsidianRowCount). Each of the five *Summary rows is
    // always shown when Obsidian is enabled; its detail rows only appear
    // while obsidianExpandedSection_ names that section - see
    // ObsidianVisibleRows(). 40 is the About tab's one row (not part of
    // "All" - see IsRowInCategory). 41 is the Reset button, handled as a
    // sentinel row rather than a real settings row.
    // Rows 9-14 are not part of the Obsidian block below - they live in the
    // Search category alongside rows 7-8 - so every Obsidian row constant
    // shifts relative to the row it would otherwise have under a plain
    // contiguous 0.. numbering.
    static constexpr int kRowWebSearchEngine = 9;
    static constexpr int kRowFileSearchPrefix = 10;
    static constexpr int kRowWebSearchPrefix = 11;
    static constexpr int kRowAppSearchPrefix = 12;
    static constexpr int kRowFileSearchEditExclusions = 13;
    static constexpr int kRowFileSearchHelp = 14;
    static constexpr int kRowObsidianEnabled = 15;
    static constexpr int kRowVaultPicker = 16;
    static constexpr int kRowVaultSearchSummary = 17;
    static constexpr int kRowVaultSearchEnabled = 18;
    static constexpr int kRowVaultSearchPrefix = 19;
    static constexpr int kRowVaultSearchPillLabel = 20;
    static constexpr int kRowTaskSummary = 21;
    static constexpr int kRowTaskAddEnabled = 22;
    static constexpr int kRowTaskPrefix = 23;
    static constexpr int kRowTaskPillLabel = 24;
    static constexpr int kRowTaskPreviewPrefix = 25;
    static constexpr int kRowNoteAddSummary = 26;
    static constexpr int kRowNoteAddEnabled = 27;
    static constexpr int kRowNoteAddPrefix = 28;
    static constexpr int kRowNoteAddPillLabel = 29;
    static constexpr int kRowNoteAddPreviewPrefix = 30;
    static constexpr int kRowLogSummary = 31;
    static constexpr int kRowLogEnabled = 32;
    static constexpr int kRowLogPrefix = 33;
    static constexpr int kRowLogPillLabel = 34;
    static constexpr int kRowLogPreviewPrefix = 35;
    static constexpr int kRowLogHeading = 36;
    static constexpr int kRowOverridesSummary = 37;
    static constexpr int kRowDailyNoteFolderOverride = 38;
    static constexpr int kRowDailyNoteFormatOverride = 39;
    static constexpr int kRowAboutGithubLink = 40;
    static constexpr int kSettingsMaxRow = 40;
    static constexpr int kRowResetToDefaults = 41;
```

- [ ] **Step 2: Extend the Search category's row range**

In `src/launcher.h`, in `IsRowInCategory` (currently line 1335):
```cpp
        if (cat == SettingsCategory::Search) return row >= 7 && row <= kRowAppSearchPrefix;
```
→
```cpp
        if (cat == SettingsCategory::Search) return row >= 7 && row <= kRowFileSearchHelp;
```

In `LastRowInCategory` (currently line 1353):
```cpp
        if (cat == SettingsCategory::Search) return kRowAppSearchPrefix;
```
→
```cpp
        if (cat == SettingsCategory::Search) return kRowFileSearchHelp;
```

(`FirstRowInCategory`'s `if (cat == SettingsCategory::Search) return 7;` is unchanged - the category still starts at row 7.)

- [ ] **Step 3: Update the layout math for the 2 added rows**

In `src/launcher.h`, in `SettingsContentBottom()` (currently lines 1407-1420):
```cpp
    float SettingsContentBottom() const {
        if (settingsCategory_ == SettingsCategory::All) {
            // 761 = fixed header offset for the OBSIDIAN card in the All view
            // (Search card start 441 + 6 rows * 47 + 38 gap, see the Search
            // branch below); +16 bottom padding.
            return 761.0f + ObsidianRowCount() * kSettingsRowHeight + 16.0f;
        } else if (settingsCategory_ == SettingsCategory::Shortcuts) {
            return 240.0f;
        } else if (settingsCategory_ == SettingsCategory::System) {
            return 193.0f;
        } else if (settingsCategory_ == SettingsCategory::Search) {
            // 6 rows (File search, Web search, Search engine - US-016; File/Web/App
            // search prefix - US-017) + 16 bottom padding.
            return 334.0f;
```
→
```cpp
    float SettingsContentBottom() const {
        if (settingsCategory_ == SettingsCategory::All) {
            // 855 = fixed header offset for the OBSIDIAN card in the All view
            // (Search card start 441 + 8 rows * 47 + 38 gap, see the Search
            // branch below); +16 bottom padding.
            return 855.0f + ObsidianRowCount() * kSettingsRowHeight + 16.0f;
        } else if (settingsCategory_ == SettingsCategory::Shortcuts) {
            return 240.0f;
        } else if (settingsCategory_ == SettingsCategory::System) {
            return 193.0f;
        } else if (settingsCategory_ == SettingsCategory::Search) {
            // 8 rows (File search, Web search, Search engine - US-016; File/Web/App
            // search prefix - US-017; Edit exclusions, Help - US-019) + 16 bottom padding.
            return 428.0f;
```

In `SettingsRowTop()` (currently lines 1447-1449):
```cpp
            if (row < kRowObsidianEnabled) return 441.0f + (row - 7) * kSettingsRowHeight;
            // Obsidian section, All-view only: header@741, card@761.
            return 761.0f + ObsidianRowRank(row) * kSettingsRowHeight;
```
→
```cpp
            if (row < kRowObsidianEnabled) return 441.0f + (row - 7) * kSettingsRowHeight;
            // Obsidian section, All-view only: header@835, card@855.
            return 855.0f + ObsidianRowRank(row) * kSettingsRowHeight;
```

In `EnsureSettingsVisible()` (currently line 1503):
```cpp
            else if (row == kRowObsidianEnabled) sectionHeaderTop = 741.0f;
```
→
```cpp
            else if (row == kRowObsidianEnabled) sectionHeaderTop = 835.0f;
```

In the Obsidian card's draw call (currently lines 4804-4806):
```cpp
        if (settingsCategory_ == SettingsCategory::All || settingsCategory_ == SettingsCategory::Obsidian) {
            const float hY = (settingsCategory_ == SettingsCategory::All) ? 741.0f : 16.0f;
            const float cY = (settingsCategory_ == SettingsCategory::All) ? 761.0f : 36.0f;
```
→
```cpp
        if (settingsCategory_ == SettingsCategory::All || settingsCategory_ == SettingsCategory::Obsidian) {
            const float hY = (settingsCategory_ == SettingsCategory::All) ? 835.0f : 16.0f;
            const float cY = (settingsCategory_ == SettingsCategory::All) ? 855.0f : 36.0f;
```

- [ ] **Step 4: Draw the 2 new rows and bump the Search card's row count**

In `src/launcher.h`, in the Search category's draw block (currently lines 4779-4802):
```cpp
        if (settingsCategory_ == SettingsCategory::All || settingsCategory_ == SettingsCategory::Search) {
            const float hY = (settingsCategory_ == SettingsCategory::All) ? 421.0f : 16.0f;
            const float cY = (settingsCategory_ == SettingsCategory::All) ? 441.0f : 36.0f;
            drawCard(L"SEARCH & FEATURES", hY, cY, 6);

            DrawSettingsRow(7, cY + offsetY, L"File search",
                L"Search files and folders on your computer", {}, true, settings_.enableFileSearch);
            DrawSettingsRow(8, cY + kSettingsRowHeight + offsetY, L"Web search",
                L"Open " + settings_.webSearchEngineName + L" when no results match your query",
                {}, true, settings_.enableWebSearch);
            DrawSettingsRow(kRowWebSearchEngine, cY + 2 * kSettingsRowHeight + offsetY, L"Search engine",
                webSearchDropdownOpen_ ? L"Tap to collapse"
                    : L"Search engine used for the \"Web search\" fallback",
                settings_.webSearchEngineName, false, false, false, true);
            DrawSettingsRow(kRowFileSearchPrefix, cY + 3 * kSettingsRowHeight + offsetY, L"File search prefix",
                L"Type this followed by a space to show only files and folders",
                settings_.fileSearchPrefix, false, false, false, true);
            DrawSettingsRow(kRowWebSearchPrefix, cY + 4 * kSettingsRowHeight + offsetY, L"Web search prefix",
                L"Type this followed by a space to force a \"" + settings_.webSearchEngineName + L"\" search",
                settings_.webSearchPrefix, false, false, false, true);
            DrawSettingsRow(kRowAppSearchPrefix, cY + 5 * kSettingsRowHeight + offsetY, L"App search prefix",
                L"Type this followed by a space to show only installed apps",
                settings_.appSearchPrefix, false, false, false, true);
        }
```
→
```cpp
        if (settingsCategory_ == SettingsCategory::All || settingsCategory_ == SettingsCategory::Search) {
            const float hY = (settingsCategory_ == SettingsCategory::All) ? 421.0f : 16.0f;
            const float cY = (settingsCategory_ == SettingsCategory::All) ? 441.0f : 36.0f;
            drawCard(L"SEARCH & FEATURES", hY, cY, 8);

            DrawSettingsRow(7, cY + offsetY, L"File search",
                L"Search files and folders on your computer", {}, true, settings_.enableFileSearch);
            DrawSettingsRow(8, cY + kSettingsRowHeight + offsetY, L"Web search",
                L"Open " + settings_.webSearchEngineName + L" when no results match your query",
                {}, true, settings_.enableWebSearch);
            DrawSettingsRow(kRowWebSearchEngine, cY + 2 * kSettingsRowHeight + offsetY, L"Search engine",
                webSearchDropdownOpen_ ? L"Tap to collapse"
                    : L"Search engine used for the \"Web search\" fallback",
                settings_.webSearchEngineName, false, false, false, true);
            DrawSettingsRow(kRowFileSearchPrefix, cY + 3 * kSettingsRowHeight + offsetY, L"File search prefix",
                L"Type this followed by a space to show only files and folders",
                settings_.fileSearchPrefix, false, false, false, true);
            DrawSettingsRow(kRowWebSearchPrefix, cY + 4 * kSettingsRowHeight + offsetY, L"Web search prefix",
                L"Type this followed by a space to force a \"" + settings_.webSearchEngineName + L"\" search",
                settings_.webSearchPrefix, false, false, false, true);
            DrawSettingsRow(kRowAppSearchPrefix, cY + 5 * kSettingsRowHeight + offsetY, L"App search prefix",
                L"Type this followed by a space to show only installed apps",
                settings_.appSearchPrefix, false, false, false, true);
            DrawSettingsRow(kRowFileSearchEditExclusions, cY + 6 * kSettingsRowHeight + offsetY, L"Edit exclusions...",
                L"Add your own folder and file-type exclusions on top of the built-in list",
                {}, false, false, true);
            DrawSettingsRow(kRowFileSearchHelp, cY + 7 * kSettingsRowHeight + offsetY, L"Help",
                L"Learn how file search exclusions work",
                {}, false, false, true);
        }
```

- [ ] **Step 5: Wire the two rows' click behavior in `ChangeSetting`**

In `src/launcher.h`, in `ChangeSetting()`, insert two new blocks immediately after the `kRowAboutGithubLink` block (currently lines 2176-2179):
```cpp
        if (row == kRowAboutGithubLink) {
            ShellExecuteW(nullptr, L"open", takeoff::kRepoUrl, nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
```
insert directly after it (leaving the `kRowAboutGithubLink` block itself untouched):
```cpp
        if (row == kRowFileSearchEditExclusions) {
            // File I/O + spawning the user's editor is a real side effect
            // that a UI test run must not trigger - mirrors the existing
            // if constexpr (!kUiTest) guard used for Obsidian's own
            // Start()/Stop() side effect a few lines above kRowVaultPicker.
            if constexpr (!kUiTest) {
                const std::wstring exclusionsPath = takeoff::FileIndex::DefaultExclusionsPath();
                if (takeoff::FileIndex::EnsureExclusionsFileWithHeader(exclusionsPath)) {
                    ShellExecuteW(nullptr, L"open", exclusionsPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
            }
            return;
        }
        if (row == kRowFileSearchHelp) {
            ShellExecuteW(nullptr, L"open",
                L"https://github.com/sdkasper/lean-launcher/blob/master/docs/file-search.md",
                nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
```

- [ ] **Step 6: Build and manually verify**

Run: `"...cmake.exe" --build cmake --config Release` then `"...ctest.exe" --test-dir cmake -C Release --output-on-failure`
Expected: build succeeds, `LeanLauncherCoreTests` still PASS (this task touches no test-covered logic, only UI layout/dispatch - a regression here would show up as every subsequent `FileIndex`/`Settings` test still passing, since none of them exercise `launcher.h`'s row math directly).

Then launch the built app (see the project's `run` skill or launch `cmake/Release/LeanLauncher.exe` directly) and manually verify, in Settings → Search:
- The card now shows 8 rows; "Edit exclusions..." and "Help" appear below "App search prefix" with no visual overlap or clipping.
- Clicking "Edit exclusions..." creates `%LOCALAPPDATA%\LeanLauncher\file_search_excludes.txt` (check it exists and contains the header template) and opens it in the default text editor.
- Clicking "Edit exclusions..." a second time does not reset any edits already saved to that file, and still opens it.
- Clicking "Help" opens `docs/file-search.md` on GitHub in the default browser.
- Switching to the "All" Settings category still shows the Obsidian card positioned correctly below the now-taller Search card, with no gap or overlap.
- Scrolling within the Search-only category view reaches both new rows without clipping (`SettingsContentBottom`'s 428.0f is correct).

- [ ] **Step 7: Commit**

```bash
git add src/launcher.h
git commit -m "Add Edit exclusions... and Help rows to Settings > Search (US-019)"
```
