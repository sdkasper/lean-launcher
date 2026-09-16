# Log Capture Action Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a fourth configurable Obsidian action ("Log") that inserts a timestamped line (`- HH:MM: <text>`) into today's daily note, at the end of a user-configured heading's section, using the same direct on-disk file I/O the existing Task/Note-Add actions use - no Obsidian CLI, no `eval`, works even when Obsidian isn't running.

**Architecture:** Two new pure functions in `src/obsidian_config.h` (`Utf8ToWide`/`WideToUtf8`, generalizing UTF-8/UTF-16 conversion this file already does inline for JSON parsing) plus three new pure functions in `src/daily_note.h` (`BuildLogLine`, `HeadingLevel`, `FindHeadingSectionEnd`) let `AppendLogEntry` read a note's full text, find where a heading's section ends, splice in the new line, and rewrite the file - the same "read whole file, transform, write it back" shape this codebase already uses for JSON config. `FindPrefixConflict` grows from 3 to 4 prefix arguments. `src/launcher.h`'s existing Settings-driven dispatch (`UpdateResults`/`LaunchSelected`/secondary actions/footer/pill labels), persistence (`LoadSettings`/`SaveSettings`), and Settings-UI accordion (row constants, `ObsidianVisibleRows`, `DrawObsidianRow`, `ChangeSetting`, `SettingsTextFieldForRow`) are extended in place, mirroring the existing Task/Note-Add action exactly, with one added field (`logHeading`).

**Tech Stack:** C++17, Win32, Direct2D/DirectWrite, Windows Registry (`HKEY_CURRENT_USER\Software\LeanLauncher`) for persistence.

**Spec:** `docs/superpowers/specs/2026-09-16-log-capture-design.md`

## Global Constraints

- No em dashes in any user-visible string or code comment (repo-wide convention, `CLAUDE.md`) - use a spaced hyphen `-` instead.
- Prefix matching stays case-insensitive (`_wcsnicmp`, via the existing `TryParsePrefix`), matching `T `/`O `/`a ` behavior.
- Heading matching (`FindHeadingSectionEnd`) is an exact, case-sensitive full-line match - a heading field is a literal line from the user's own note, not a fuzzy prefix.
- Registry persistence follows the existing two patterns exactly: bools go in the `Entry{name, DWORD}` array in `SaveSettings()`; strings are individual `RegSetValueExW`/`RegGetValueW` `REG_SZ` calls via the existing `readStringSetting`/`writeStringSetting` lambdas.
- Every new pure function goes in `src/obsidian_config.h` or `src/daily_note.h` (matching where similar existing helpers already live) and gets unit tests in `tests/core_tests.cpp` - the existing project convention.
- Build verification command (core tests): `"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release --target LeanLauncherCoreTests`
- Build verification command (full app, only needed for tasks touching `src/launcher.h` or `src/search.h`): `"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release --target LeanLauncher` - if `LeanLauncher.exe` is currently running, stop it first (the linker fails with LNK1104 otherwise).
- Test binary: `./build/Release/LeanLauncherCoreTests.exe` (exit code 0, no `FAIL:` lines in output).

---

## Task 1: Settings struct fields

**Files:**
- Modify: `src/settings.h` (the `Settings` struct)

**Interfaces:**
- Produces: `Settings::logEnabled` (bool), `Settings::logPrefix`, `Settings::logPillLabel`, `Settings::logPreviewPrefix`, `Settings::logHeading` (std::wstring) - every later task reads/writes these exact names.

- [ ] **Step 1: Add the new fields to `Settings`**

Find:
```cpp
    bool obsidianEnabled = false;
    bool vaultSearchEnabled = true;
    bool taskAddEnabled = true;
    bool noteAddEnabled = true;

    std::wstring vaultSearchPrefix = L"o";
    std::wstring vaultSearchPillLabel = L"Jump";

    std::wstring taskPrefix = L"t";
    std::wstring taskPillLabel = L"Task";
    std::wstring taskPreviewPrefix = L"Add task: ";

    std::wstring noteAddPrefix = L"a";
    std::wstring noteAddPillLabel = L"Note";
    std::wstring noteAddPreviewPrefix = L"Add to today's note: ";

    // Empty = auto-detect from the vault's own daily-notes/periodic-notes
    // plugin config (existing ReadDailyNoteConfig behavior, unchanged).
    std::wstring dailyNoteFolderOverride;
    std::wstring dailyNoteFormatOverride;
};
```
Replace with:
```cpp
    bool obsidianEnabled = false;
    bool vaultSearchEnabled = true;
    bool taskAddEnabled = true;
    bool noteAddEnabled = true;
    bool logEnabled = true;

    std::wstring vaultSearchPrefix = L"o";
    std::wstring vaultSearchPillLabel = L"Jump";

    std::wstring taskPrefix = L"t";
    std::wstring taskPillLabel = L"Task";
    std::wstring taskPreviewPrefix = L"Add task: ";

    std::wstring noteAddPrefix = L"a";
    std::wstring noteAddPillLabel = L"Note";
    std::wstring noteAddPreviewPrefix = L"Add to today's note: ";

    std::wstring logPrefix = L"l";
    std::wstring logPillLabel = L"Log";
    std::wstring logPreviewPrefix = L"Log: ";
    // The exact heading line (including leading '#'s) to insert log entries
    // after, e.g. "## Log". Not an "empty means auto-detect" field like the
    // overrides below - it always has a concrete default.
    std::wstring logHeading = L"## Log";

    // Empty = auto-detect from the vault's own daily-notes/periodic-notes
    // plugin config (existing ReadDailyNoteConfig behavior, unchanged).
    std::wstring dailyNoteFolderOverride;
    std::wstring dailyNoteFormatOverride;
};
```

- [ ] **Step 2: Compile to confirm no syntax errors**

Run the core-tests build command from Global Constraints.
Expected: builds clean (this struct isn't wired to anything new yet, so nothing else changes behavior).

- [ ] **Step 3: Commit**

```bash
git add src/settings.h
git commit -m "Add Log action fields to Settings"
```

---

## Task 2: UTF-8/UTF-16 helpers, heading-section lookup, and `AppendLogEntry`

**Files:**
- Modify: `src/obsidian_config.h` (add `Utf8ToWide`/`WideToUtf8` after `ReadFileUtf8`)
- Modify: `src/daily_note.h` (add `FormatTimeHHMM` after `FormatIsoTimestamp`; add `BuildLogLine`, `HeadingLevel`, `FindHeadingSectionEnd` after `BuildPlainLine`; add `AppendLogEntry` after `AppendNoteText`)
- Modify: `tests/core_tests.cpp` (new test blocks, inserted right before the `// --- NoteIndex Tests (Task 3) ---` comment, i.e. immediately after the existing note-add integration test block that ends `RemoveDirectoryW(vaultRoot.c_str());\n    }`)

**Interfaces:**
- Consumes: `ReadFileUtf8` (existing).
- Produces: `leanlauncher::obsidian::Utf8ToWide(const std::string&) -> std::wstring`, `leanlauncher::obsidian::WideToUtf8(const std::wstring&) -> std::string`, `leanlauncher::obsidian::BuildLogLine(std::wstring_view) -> std::wstring`, `leanlauncher::obsidian::HeadingLevel(const std::wstring&) -> int`, `leanlauncher::obsidian::FindHeadingSectionEnd(const std::wstring&, const std::wstring&) -> size_t`, `leanlauncher::obsidian::AppendLogEntry(const std::wstring& notePath, std::wstring_view text, const std::wstring& heading) -> bool`. Task 6 calls `AppendLogEntry` by this exact name and signature.

- [ ] **Step 1: Write the failing tests**

In `tests/core_tests.cpp`, find the end of the note-add integration test block:
```cpp
        Check(ss.str().find("write plan\n") != std::string::npos &&
              ss.str().find("- [ ] write plan\n") == std::string::npos,
            "note-add pipeline: end-to-end content matches what was typed, as plain text (no checklist marker)");
        check.close();
        DeleteFileW(notePath.c_str());
        RemoveDirectoryW(vaultRoot.c_str());
    }

    // --- NoteIndex Tests (Task 3) ---
```
Insert the following test blocks between that closing `}` and the `// --- NoteIndex Tests (Task 3) ---` comment:
```cpp
    // --- Log capture: heading-section lookup ---
    {
        Check(FindHeadingSectionEnd(L"# Title\nSome text\n", L"## Log") == std::wstring::npos,
            "FindHeadingSectionEnd returns npos when the heading isn't present");

        const std::wstring toEof = L"## Log\n- 10:00: a\n";
        Check(FindHeadingSectionEnd(toEof, L"## Log") == toEof.size(),
            "FindHeadingSectionEnd returns end-of-content when the heading's section runs to EOF");

        const std::wstring sameLevel = L"## Log\n- 10:00: a\n## Other\nmore\n";
        Check(FindHeadingSectionEnd(sameLevel, L"## Log") == sameLevel.find(L"## Other"),
            "FindHeadingSectionEnd stops at the next heading of the same level");

        const std::wstring shallower = L"## Log\n- 10:00: a\n# Bigger\n";
        Check(FindHeadingSectionEnd(shallower, L"## Log") == shallower.find(L"# Bigger"),
            "FindHeadingSectionEnd stops at a shallower-level heading too");

        const std::wstring withSubsection = L"## Log\n### Sub\ntext\n## Other\n";
        Check(FindHeadingSectionEnd(withSubsection, L"## Log") == withSubsection.find(L"## Other"),
            "FindHeadingSectionEnd treats a deeper sub-heading as still inside the section");

        Check(FindHeadingSectionEnd(L"## Log", L"## Log") == 6,
            "FindHeadingSectionEnd handles a heading with no trailing newline as running to EOF");

        Check(HeadingLevel(L"## Log") == 2, "HeadingLevel counts leading '#' characters");
        Check(HeadingLevel(L"#tag not a heading") == 0,
            "HeadingLevel rejects a '#' run with no following space (not an ATX heading)");
        Check(HeadingLevel(L"plain text") == 0, "HeadingLevel returns 0 for a non-heading line");
    }

    {
        const std::wstring line = BuildLogLine(L"buy milk");
        Check(line.size() >= 11 && line.substr(0, 2) == L"- " && line[4] == L':' && line.substr(7, 2) == L": " &&
              line.back() == L'\n',
            "BuildLogLine formats as '- HH:MM: <text>\\n'");
        Check(line.find(L"buy milk") != std::wstring::npos, "BuildLogLine includes the entry text");
        Check(BuildLogLine(L"line1\r\nline2").find(L"line1line2") != std::wstring::npos,
            "BuildLogLine strips embedded CR/LF so one entry never becomes two lines");
    }

    // --- Log capture: AppendLogEntry integration ---
    {
        wchar_t tempDir[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tempDir);
        const std::wstring notePath = std::wstring(tempDir) + L"LeanLauncherTest_LogNewFile.md";
        DeleteFileW(notePath.c_str());

        Check(AppendLogEntry(notePath, L"first entry", L"## Log"),
            "AppendLogEntry creates a new file and returns true");
        std::ifstream check(notePath, std::ios::binary);
        std::ostringstream ss;
        ss << check.rdbuf();
        const std::string content = ss.str();
        check.close();
        Check(content.find("---\ncreated:") != std::string::npos,
            "AppendLogEntry writes minimal frontmatter for a new file");
        Check(content.find(": first entry\n") != std::string::npos,
            "AppendLogEntry writes the log line for a new file");
        DeleteFileW(notePath.c_str());
    }

    {
        wchar_t tempDir[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tempDir);
        const std::wstring notePath = std::wstring(tempDir) + L"LeanLauncherTest_LogWithHeading.md";
        DeleteFileW(notePath.c_str());
        {
            std::ofstream seed(notePath, std::ios::binary);
            seed << "# Daily\n\n## Log\n- 09:00: woke up\n\n## Other section\nunrelated\n";
        }

        Check(AppendLogEntry(notePath, L"back from a walk", L"## Log"),
            "AppendLogEntry writes to an existing file and returns true");
        std::ifstream check(notePath, std::ios::binary);
        std::ostringstream ss;
        ss << check.rdbuf();
        const std::string content = ss.str();
        check.close();

        const size_t logPos = content.find("- 09:00: woke up");
        const size_t newPos = content.find(": back from a walk");
        const size_t otherPos = content.find("## Other section");
        Check(logPos != std::string::npos && newPos != std::string::npos && otherPos != std::string::npos &&
              logPos < newPos && newPos < otherPos,
            "AppendLogEntry inserts the new line after the existing log entry but before the next heading");
        Check(content.find("unrelated") != std::string::npos,
            "AppendLogEntry leaves content after the section untouched");
        DeleteFileW(notePath.c_str());
    }

    {
        wchar_t tempDir[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tempDir);
        const std::wstring notePath = std::wstring(tempDir) + L"LeanLauncherTest_LogNoHeading.md";
        DeleteFileW(notePath.c_str());
        {
            std::ofstream seed(notePath, std::ios::binary);
            seed << "# Daily\nsome notes\n";
        }

        Check(AppendLogEntry(notePath, L"no heading here", L"## Log"),
            "AppendLogEntry succeeds even when the configured heading is missing");
        std::ifstream check(notePath, std::ios::binary);
        std::ostringstream ss;
        ss << check.rdbuf();
        const std::string content = ss.str();
        check.close();
        Check(content.find("some notes\n- ") != std::string::npos &&
              content.find(": no heading here\n") != std::string::npos,
            "AppendLogEntry falls back to end-of-file when the heading isn't found");
        DeleteFileW(notePath.c_str());
    }

    {
        wchar_t tempDir[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tempDir);
        const std::wstring notePath = std::wstring(tempDir) + L"LeanLauncherTest_LogNoTrailingNewline.md";
        DeleteFileW(notePath.c_str());
        {
            std::ofstream seed(notePath, std::ios::binary);
            seed << "## Log\n- 09:00: woke up";  // no trailing newline
        }

        Check(AppendLogEntry(notePath, L"buy milk", L"## Log"),
            "AppendLogEntry on a no-trailing-newline file returns true");
        std::ifstream check(notePath, std::ios::binary);
        std::ostringstream ss;
        ss << check.rdbuf();
        const std::string content = ss.str();
        check.close();
        Check(content.find("- 09:00: woke up\n- ") != std::string::npos,
            "AppendLogEntry inserts a newline so the previous line and the new entry are both intact");
        Check(content.find("woke up- ") == std::string::npos,
            "AppendLogEntry never merges the new entry into the previous line");
        DeleteFileW(notePath.c_str());
    }

    {
        wchar_t tempDir[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tempDir);
        const std::wstring notePath = std::wstring(tempDir) + L"LeanLauncherTest_LogUtf8Roundtrip.md";
        DeleteFileW(notePath.c_str());
        {
            // UTF-8 bytes for "café 🌱" seeded directly so this test doesn't
            // depend on the compiler's source-file encoding of a literal.
            std::ofstream seed(notePath, std::ios::binary);
            seed << "## Log\n- 09:00: caf\xC3\xA9 \xF0\x9F\x8C\xB1\n";
        }

        Check(AppendLogEntry(notePath, L"second entry", L"## Log"),
            "AppendLogEntry succeeds against a file containing multi-byte UTF-8 content");
        std::ifstream check(notePath, std::ios::binary);
        std::ostringstream ss;
        ss << check.rdbuf();
        const std::string content = ss.str();
        check.close();
        Check(content.find("caf\xC3\xA9 \xF0\x9F\x8C\xB1") != std::string::npos,
            "AppendLogEntry preserves existing multi-byte UTF-8 content byte-for-byte");
        Check(content.find(": second entry\n") != std::string::npos,
            "AppendLogEntry appends the new entry after the UTF-8 content");
        DeleteFileW(notePath.c_str());
    }
```
(`AppendLogEntry`, `FindHeadingSectionEnd`, `HeadingLevel`, and `BuildLogLine` are called unqualified because `using namespace leanlauncher::obsidian;`, declared earlier at line 997 inside `main()`, is still in scope for the rest of the function - the same reason nearby `AppendTask`/`AppendNoteText` calls are unqualified.)

- [ ] **Step 2: Run tests to verify they fail to compile**

Run the core-tests build command from Global Constraints.
Expected: FAIL - `Utf8ToWide`, `BuildLogLine`, `HeadingLevel`, `FindHeadingSectionEnd`, `AppendLogEntry` are not declared yet.

- [ ] **Step 3: Implement `Utf8ToWide`/`WideToUtf8` in `obsidian_config.h`**

Find:
```cpp
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
```
Replace with:
```cpp
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

// Decodes a UTF-8 byte string to UTF-16.
inline std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return L"";
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (wlen <= 0) return L"";
    std::wstring out(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), wlen);
    return out;
}

// Encodes UTF-16 text to a UTF-8 byte string. Inverse of Utf8ToWide.
inline std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), len, nullptr, nullptr);
    return out;
}
```

- [ ] **Step 4: Implement `FormatTimeHHMM`, `BuildLogLine`, `HeadingLevel`, `FindHeadingSectionEnd` in `daily_note.h`**

Find:
```cpp
inline std::wstring FormatIsoTimestamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[32];
    swprintf_s(buf, L"%04d-%02d-%02dT%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}
```
Replace with:
```cpp
inline std::wstring FormatIsoTimestamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[32];
    swprintf_s(buf, L"%04d-%02d-%02dT%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}

// HH:MM local time, matching the vault's existing QuickAdd "{{time}}" token
// format for the "Log Entry" capture choice this action replicates.
inline std::wstring FormatTimeHHMM() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[8];
    swprintf_s(buf, L"%02d:%02d", st.wHour, st.wMinute);
    return buf;
}
```

Find:
```cpp
// Builds a plain text line (no bullet, no checklist) for the "a " prefix.
inline std::wstring BuildPlainLine(std::wstring_view text) {
    return StripLineBreaks(text) + L"\n";
}
```
Replace with:
```cpp
// Builds a plain text line (no bullet, no checklist) for the "a " prefix.
inline std::wstring BuildPlainLine(std::wstring_view text) {
    return StripLineBreaks(text) + L"\n";
}

// Builds a timestamped log line: "- HH:MM: <text>\n".
inline std::wstring BuildLogLine(std::wstring_view text) {
    return L"- " + FormatTimeHHMM() + L": " + StripLineBreaks(text) + L"\n";
}

// Returns the ATX heading level (count of leading '#' characters) of `line`,
// or 0 if `line` isn't a heading. A run of '#' only counts as a heading if
// it's followed by a space or the end of the line - "#tag" at the start of
// a line is not a heading.
inline int HeadingLevel(const std::wstring& line) {
    size_t i = 0;
    while (i < line.size() && line[i] == L'#') ++i;
    if (i == 0) return 0;
    if (i < line.size() && line[i] != L' ') return 0;
    return static_cast<int>(i);
}

// Finds where a new line should be inserted to land at the end of the
// section introduced by `heading` (an exact, case-sensitive line match,
// e.g. "## Log"). The section ends at the first later line that is itself
// a heading of level <= the target's level - a deeper sub-heading (e.g.
// "### Sub" under "## Log") stays inside the section. Returns
// content.size() if the heading's section runs to the end of the file, or
// std::wstring::npos if `heading` doesn't appear in `content` at all.
inline size_t FindHeadingSectionEnd(const std::wstring& content, const std::wstring& heading) {
    bool found = false;
    int targetLevel = 0;
    size_t lineStart = 0;
    while (lineStart <= content.size()) {
        const size_t lineEnd = content.find(L'\n', lineStart);
        const bool atEnd = (lineEnd == std::wstring::npos);
        const size_t rawEnd = atEnd ? content.size() : lineEnd;
        size_t trimEnd = rawEnd;
        if (trimEnd > lineStart && content[trimEnd - 1] == L'\r') --trimEnd;
        const std::wstring lineText = content.substr(lineStart, trimEnd - lineStart);

        if (!found) {
            if (lineText == heading) {
                found = true;
                targetLevel = HeadingLevel(lineText);
            }
        } else {
            const int level = HeadingLevel(lineText);
            if (level > 0 && level <= targetLevel) return lineStart;
        }
        if (atEnd) break;
        lineStart = lineEnd + 1;
    }
    return found ? content.size() : std::wstring::npos;
}
```

- [ ] **Step 5: Implement `AppendLogEntry` in `daily_note.h`**

Find:
```cpp
// Appends one plain text line (the "a " prefix) to the note at notePath.
inline bool AppendNoteText(const std::wstring& notePath, std::wstring_view text) {
    return AppendLine(notePath, BuildPlainLine(text));
}

}  // namespace obsidian
}  // namespace leanlauncher
```
Replace with:
```cpp
// Appends one plain text line (the "a " prefix) to the note at notePath.
inline bool AppendNoteText(const std::wstring& notePath, std::wstring_view text) {
    return AppendLine(notePath, BuildPlainLine(text));
}

// Inserts one timestamped log line at the end of `heading`'s section in the
// note at notePath - see FindHeadingSectionEnd for exactly where that is.
// Creates the file (with the same minimal frontmatter as AppendLine) if it
// doesn't exist yet; falls back to end-of-file if `heading` isn't found in
// an existing file. Unlike AppendLine, this must read and rewrite the whole
// file - the insertion point usually isn't at the end - so it opens the
// file for a full overwrite rather than FILE_APPEND_DATA.
inline bool AppendLogEntry(const std::wstring& notePath, std::wstring_view text, const std::wstring& heading) {
    std::error_code ec;
    const fs::path path(notePath);
    fs::create_directories(path.parent_path(), ec);

    const std::wstring line = BuildLogLine(text);
    std::wstring newContent;

    if (!fs::exists(path, ec)) {
        newContent = L"---\ncreated: " + FormatIsoTimestamp() + L"\n---\n\n" + line;
    } else {
        const std::wstring content = Utf8ToWide(ReadFileUtf8(path));
        const size_t boundary = FindHeadingSectionEnd(content, heading);
        const size_t insertAt = (boundary == std::wstring::npos) ? content.size() : boundary;
        const bool needsLeadingNewline = insertAt > 0 && content[insertAt - 1] != L'\n';

        newContent = content.substr(0, insertAt);
        if (needsLeadingNewline) newContent += L'\n';
        newContent += line;
        newContent += content.substr(insertAt);
    }

    const std::string utf8 = WideToUtf8(newContent);
    HANDLE file = CreateFileW(notePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr) &&
        written == static_cast<DWORD>(utf8.size());
    CloseHandle(file);
    return ok;
}

}  // namespace obsidian
}  // namespace leanlauncher
```

- [ ] **Step 6: Run tests to verify they pass**

Run the core-tests build command, then `./build/Release/LeanLauncherCoreTests.exe`.
Expected: builds clean, exit code 0, no `FAIL:` lines.

- [ ] **Step 7: Commit**

```bash
git add src/obsidian_config.h src/daily_note.h tests/core_tests.cpp
git commit -m "Add heading-based log-line insertion (Utf8ToWide/WideToUtf8, FindHeadingSectionEnd, AppendLogEntry)"
```

---

## Task 3: Extend `FindPrefixConflict` to four prefixes

**Files:**
- Modify: `src/obsidian_config.h` (`FindPrefixConflict`)
- Modify: `tests/core_tests.cpp` (its existing test block)

**Interfaces:**
- Produces: `leanlauncher::obsidian::FindPrefixConflict(const std::wstring& vaultSearchPrefix, const std::wstring& taskPrefix, const std::wstring& noteAddPrefix, const std::wstring& logPrefix) -> const wchar_t*`. Task 5's `CommitEditingRow` calls this by this exact name/signature.

- [ ] **Step 1: Update the failing test**

Find:
```cpp
        Check(FindPrefixConflict(L"O", L"T", L"a") == nullptr,
            "FindPrefixConflict accepts three distinct non-empty prefixes");
        Check(FindPrefixConflict(L"", L"T", L"a") != nullptr,
            "FindPrefixConflict rejects an empty prefix");
        Check(FindPrefixConflict(L"T", L"T", L"a") != nullptr,
            "FindPrefixConflict rejects an exact duplicate");
        Check(FindPrefixConflict(L"t", L"T", L"a") != nullptr,
            "FindPrefixConflict rejects a case-insensitive duplicate");
    }
```
Replace with:
```cpp
        Check(FindPrefixConflict(L"O", L"T", L"a", L"l") == nullptr,
            "FindPrefixConflict accepts four distinct non-empty prefixes");
        Check(FindPrefixConflict(L"", L"T", L"a", L"l") != nullptr,
            "FindPrefixConflict rejects an empty prefix");
        Check(FindPrefixConflict(L"T", L"T", L"a", L"l") != nullptr,
            "FindPrefixConflict rejects an exact duplicate");
        Check(FindPrefixConflict(L"t", L"T", L"a", L"l") != nullptr,
            "FindPrefixConflict rejects a case-insensitive duplicate");
        Check(FindPrefixConflict(L"O", L"T", L"a", L"a") != nullptr,
            "FindPrefixConflict rejects a duplicate against the fourth (log) prefix");
    }
```

- [ ] **Step 2: Run test to verify it fails to compile**

Run the core-tests build command from Global Constraints.
Expected: FAIL - `FindPrefixConflict` only takes 3 arguments.

- [ ] **Step 3: Update `FindPrefixConflict`**

Find:
```cpp
// Returns a user-facing error message if the three configured action
// prefixes aren't all non-empty and mutually distinct (case-insensitive),
// or nullptr if they're valid. Used to reject an in-progress Settings edit
// before it's saved - two prefixes colliding would make one action
// permanently unreachable, and an empty prefix would match every input.
inline const wchar_t* FindPrefixConflict(
    const std::wstring& vaultSearchPrefix, const std::wstring& taskPrefix, const std::wstring& noteAddPrefix) {
    if (vaultSearchPrefix.empty() || taskPrefix.empty() || noteAddPrefix.empty()) {
        return L"Prefix cannot be empty.";
    }
    auto ciEqual = [](const std::wstring& a, const std::wstring& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (towlower(a[i]) != towlower(b[i])) return false;
        }
        return true;
    };
    if (ciEqual(vaultSearchPrefix, taskPrefix) || ciEqual(vaultSearchPrefix, noteAddPrefix) ||
        ciEqual(taskPrefix, noteAddPrefix)) {
        return L"Prefixes must be unique.";
    }
    return nullptr;
}
```
Replace with:
```cpp
// Returns a user-facing error message if the four configured action
// prefixes aren't all non-empty and mutually distinct (case-insensitive),
// or nullptr if they're valid. Used to reject an in-progress Settings edit
// before it's saved - two prefixes colliding would make one action
// permanently unreachable, and an empty prefix would match every input.
inline const wchar_t* FindPrefixConflict(
    const std::wstring& vaultSearchPrefix, const std::wstring& taskPrefix,
    const std::wstring& noteAddPrefix, const std::wstring& logPrefix) {
    if (vaultSearchPrefix.empty() || taskPrefix.empty() || noteAddPrefix.empty() || logPrefix.empty()) {
        return L"Prefix cannot be empty.";
    }
    auto ciEqual = [](const std::wstring& a, const std::wstring& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (towlower(a[i]) != towlower(b[i])) return false;
        }
        return true;
    };
    const std::wstring* prefixes[] = {&vaultSearchPrefix, &taskPrefix, &noteAddPrefix, &logPrefix};
    for (size_t i = 0; i < 4; ++i) {
        for (size_t j = i + 1; j < 4; ++j) {
            if (ciEqual(*prefixes[i], *prefixes[j])) return L"Prefixes must be unique.";
        }
    }
    return nullptr;
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run the core-tests build command, then `./build/Release/LeanLauncherCoreTests.exe`.
Expected: builds clean, exit code 0. (This will keep failing to link/compile until Task 5's Step 3 updates the one call site in `launcher.h` - if building the core-tests target alone succeeds without touching `launcher.h`, that's expected, since `launcher.h` is only pulled in by the full `LeanLauncher` app target, not `LeanLauncherCoreTests`. Confirm by also running the full-app build command from Global Constraints and expecting a compile error there naming `CommitEditingRow`'s `FindPrefixConflict` call - that error is resolved in Task 5.)

- [ ] **Step 5: Commit**

```bash
git add src/obsidian_config.h tests/core_tests.cpp
git commit -m "Extend FindPrefixConflict to four action prefixes"
```

---

## Task 4: Settings persistence (Load/Save)

**Files:**
- Modify: `src/launcher.h` (`LoadSettings()`, `SaveSettings()`)

**Interfaces:**
- Consumes: `Settings::logEnabled/logPrefix/logPillLabel/logPreviewPrefix/logHeading` (Task 1).
- Produces: no new symbols - persistence plumbing only.

- [ ] **Step 1: Read the new fields in `LoadSettings()`**

Find:
```cpp
                settings_.vaultSearchEnabled = ReadDword(key, L"VaultSearchEnabled", 1) != 0;
                settings_.taskAddEnabled = ReadDword(key, L"TaskAddEnabled", 1) != 0;
                settings_.noteAddEnabled = ReadDword(key, L"NoteAddEnabled", 1) != 0;
```
Replace:
```cpp
                settings_.vaultSearchEnabled = ReadDword(key, L"VaultSearchEnabled", 1) != 0;
                settings_.taskAddEnabled = ReadDword(key, L"TaskAddEnabled", 1) != 0;
                settings_.noteAddEnabled = ReadDword(key, L"NoteAddEnabled", 1) != 0;
                settings_.logEnabled = ReadDword(key, L"LogEnabled", 1) != 0;
```

Find:
```cpp
                readStringSetting(L"NoteAddPreviewPrefix", settings_.noteAddPreviewPrefix);
                readStringSetting(L"DailyNoteFolderOverride", settings_.dailyNoteFolderOverride);
                readStringSetting(L"DailyNoteFormatOverride", settings_.dailyNoteFormatOverride);
```
Replace:
```cpp
                readStringSetting(L"NoteAddPreviewPrefix", settings_.noteAddPreviewPrefix);
                readStringSetting(L"LogPrefix", settings_.logPrefix);
                readStringSetting(L"LogPillLabel", settings_.logPillLabel);
                readStringSetting(L"LogPreviewPrefix", settings_.logPreviewPrefix);
                readStringSetting(L"LogHeading", settings_.logHeading);
                readStringSetting(L"DailyNoteFolderOverride", settings_.dailyNoteFolderOverride);
                readStringSetting(L"DailyNoteFormatOverride", settings_.dailyNoteFormatOverride);
```

- [ ] **Step 2: Write the new fields in `SaveSettings()`**

Find:
```cpp
                {L"ObsidianEnabled", settings_.obsidianEnabled ? 1u : 0u},
                {L"VaultSearchEnabled", settings_.vaultSearchEnabled ? 1u : 0u},
                {L"TaskAddEnabled", settings_.taskAddEnabled ? 1u : 0u},
                {L"NoteAddEnabled", settings_.noteAddEnabled ? 1u : 0u},
            };
```
Replace:
```cpp
                {L"ObsidianEnabled", settings_.obsidianEnabled ? 1u : 0u},
                {L"VaultSearchEnabled", settings_.vaultSearchEnabled ? 1u : 0u},
                {L"TaskAddEnabled", settings_.taskAddEnabled ? 1u : 0u},
                {L"NoteAddEnabled", settings_.noteAddEnabled ? 1u : 0u},
                {L"LogEnabled", settings_.logEnabled ? 1u : 0u},
            };
```

Find:
```cpp
            writeStringSetting(L"NoteAddPreviewPrefix", settings_.noteAddPreviewPrefix);
            writeStringSetting(L"DailyNoteFolderOverride", settings_.dailyNoteFolderOverride);
            writeStringSetting(L"DailyNoteFormatOverride", settings_.dailyNoteFormatOverride);
```
Replace:
```cpp
            writeStringSetting(L"NoteAddPreviewPrefix", settings_.noteAddPreviewPrefix);
            writeStringSetting(L"LogPrefix", settings_.logPrefix);
            writeStringSetting(L"LogPillLabel", settings_.logPillLabel);
            writeStringSetting(L"LogPreviewPrefix", settings_.logPreviewPrefix);
            writeStringSetting(L"LogHeading", settings_.logHeading);
            writeStringSetting(L"DailyNoteFolderOverride", settings_.dailyNoteFolderOverride);
            writeStringSetting(L"DailyNoteFormatOverride", settings_.dailyNoteFormatOverride);
```

- [ ] **Step 3: Build to confirm no syntax errors**

Run the full-app build command from Global Constraints (stop `LeanLauncher.exe` first if running).
Expected: builds clean (nothing reads these rows in the UI yet - that's Task 5 - so this is inert but functional persistence).

- [ ] **Step 4: Commit**

```bash
git add src/launcher.h
git commit -m "Persist Log action settings (LogEnabled/LogPrefix/LogPillLabel/LogPreviewPrefix/LogHeading)"
```

---

## Task 5: Settings UI - row scheme, accordion section, editing

**Files:**
- Modify: `src/launcher.h` (row-index constants, `ObsidianVisibleRows`, `DrawObsidianRow`, `ChangeSetting`, `SettingsTextFieldForRow`, `IsPrefixRow`, `CommitEditingRow`)

**Interfaces:**
- Consumes: `Settings::logEnabled/logPrefix/logPillLabel/logPreviewPrefix/logHeading` (Task 1), `FindPrefixConflict` 4-arg form (Task 3).
- Produces: row constants `kRowLogSummary` through `kRowLogHeading`, `kSectionLog` - Task 6 does not need these (dispatch/execution reads `settings_.log*` directly, not row IDs), but keeping the names exact matters for anyone extending this section further.

- [ ] **Step 1: Renumber row constants and add the Log section id**

Find:
```cpp
    // Settings rows 0-3: keyboard shortcuts. 4-6: system. 7-8: search.
    // 9-27: Obsidian (only row 9 is active when settings_.obsidianEnabled is
    // false - see IsRowInCategory/ObsidianRowCount). Each of the four
    // *Summary rows is always shown when Obsidian is enabled; its detail
    // rows only appear while obsidianExpandedSection_ names that section -
    // see ObsidianVisibleRows(). 29 is the About tab's one row (not part of
    // "All" - see IsRowInCategory). 30 is the Reset button, handled as a
    // sentinel row rather than a real settings row.
    static constexpr int kRowObsidianEnabled = 9;
    static constexpr int kRowVaultPicker = 10;
    static constexpr int kRowVaultSearchSummary = 11;
    static constexpr int kRowVaultSearchEnabled = 12;
    static constexpr int kRowVaultSearchPrefix = 13;
    static constexpr int kRowVaultSearchPillLabel = 14;
    static constexpr int kRowTaskSummary = 15;
    static constexpr int kRowTaskAddEnabled = 16;
    static constexpr int kRowTaskPrefix = 17;
    static constexpr int kRowTaskPillLabel = 18;
    static constexpr int kRowTaskPreviewPrefix = 19;
    static constexpr int kRowNoteAddSummary = 20;
    static constexpr int kRowNoteAddEnabled = 21;
    static constexpr int kRowNoteAddPrefix = 22;
    static constexpr int kRowNoteAddPillLabel = 23;
    static constexpr int kRowNoteAddPreviewPrefix = 24;
    static constexpr int kRowOverridesSummary = 25;
    static constexpr int kRowDailyNoteFolderOverride = 26;
    static constexpr int kRowDailyNoteFormatOverride = 27;
    static constexpr int kRowAboutGithubLink = 29;
    static constexpr int kSettingsMaxRow = 29;
    static constexpr int kRowResetToDefaults = 30;

    // Which of the four Obsidian action blocks is currently expanded, or
    // -1 if all are collapsed. A single int gives accordion behavior for
    // free: setting it to a new section implicitly collapses whichever one
    // was open before.
    static constexpr int kSectionVaultSearch = 0;
    static constexpr int kSectionTask = 1;
    static constexpr int kSectionNoteAdd = 2;
    static constexpr int kSectionOverrides = 3;
```
Replace:
```cpp
    // Settings rows 0-3: keyboard shortcuts. 4-6: system. 7-8: search.
    // 9-33: Obsidian (only row 9 is active when settings_.obsidianEnabled is
    // false - see IsRowInCategory/ObsidianRowCount). Each of the five
    // *Summary rows is always shown when Obsidian is enabled; its detail
    // rows only appear while obsidianExpandedSection_ names that section -
    // see ObsidianVisibleRows(). 34 is the About tab's one row (not part of
    // "All" - see IsRowInCategory). 35 is the Reset button, handled as a
    // sentinel row rather than a real settings row.
    static constexpr int kRowObsidianEnabled = 9;
    static constexpr int kRowVaultPicker = 10;
    static constexpr int kRowVaultSearchSummary = 11;
    static constexpr int kRowVaultSearchEnabled = 12;
    static constexpr int kRowVaultSearchPrefix = 13;
    static constexpr int kRowVaultSearchPillLabel = 14;
    static constexpr int kRowTaskSummary = 15;
    static constexpr int kRowTaskAddEnabled = 16;
    static constexpr int kRowTaskPrefix = 17;
    static constexpr int kRowTaskPillLabel = 18;
    static constexpr int kRowTaskPreviewPrefix = 19;
    static constexpr int kRowNoteAddSummary = 20;
    static constexpr int kRowNoteAddEnabled = 21;
    static constexpr int kRowNoteAddPrefix = 22;
    static constexpr int kRowNoteAddPillLabel = 23;
    static constexpr int kRowNoteAddPreviewPrefix = 24;
    static constexpr int kRowLogSummary = 25;
    static constexpr int kRowLogEnabled = 26;
    static constexpr int kRowLogPrefix = 27;
    static constexpr int kRowLogPillLabel = 28;
    static constexpr int kRowLogPreviewPrefix = 29;
    static constexpr int kRowLogHeading = 30;
    static constexpr int kRowOverridesSummary = 31;
    static constexpr int kRowDailyNoteFolderOverride = 32;
    static constexpr int kRowDailyNoteFormatOverride = 33;
    static constexpr int kRowAboutGithubLink = 34;
    static constexpr int kSettingsMaxRow = 34;
    static constexpr int kRowResetToDefaults = 35;

    // Which of the five Obsidian action blocks is currently expanded, or
    // -1 if all are collapsed. A single int gives accordion behavior for
    // free: setting it to a new section implicitly collapses whichever one
    // was open before.
    static constexpr int kSectionVaultSearch = 0;
    static constexpr int kSectionTask = 1;
    static constexpr int kSectionNoteAdd = 2;
    static constexpr int kSectionLog = 3;
    static constexpr int kSectionOverrides = 4;
```

`FirstRowInCategory`/`LastRowInCategory`/`NextSettingsRow`/`SettingsRowAtPoint`/`SettingsContentBottom`/`SettingsRowTop` already read `kSettingsMaxRow`/`ObsidianRowRank`/`ObsidianVisibleRows` symbolically rather than hardcoding row numbers - no changes needed there.

- [ ] **Step 2: Add the Log block to `ObsidianVisibleRows()`**

Find:
```cpp
        rows.push_back(kRowNoteAddSummary);
        if (obsidianExpandedSection_ == kSectionNoteAdd) {
            rows.push_back(kRowNoteAddEnabled);
            rows.push_back(kRowNoteAddPrefix);
            rows.push_back(kRowNoteAddPillLabel);
            rows.push_back(kRowNoteAddPreviewPrefix);
        }

        rows.push_back(kRowOverridesSummary);
```
Replace:
```cpp
        rows.push_back(kRowNoteAddSummary);
        if (obsidianExpandedSection_ == kSectionNoteAdd) {
            rows.push_back(kRowNoteAddEnabled);
            rows.push_back(kRowNoteAddPrefix);
            rows.push_back(kRowNoteAddPillLabel);
            rows.push_back(kRowNoteAddPreviewPrefix);
        }

        rows.push_back(kRowLogSummary);
        if (obsidianExpandedSection_ == kSectionLog) {
            rows.push_back(kRowLogEnabled);
            rows.push_back(kRowLogPrefix);
            rows.push_back(kRowLogPillLabel);
            rows.push_back(kRowLogPreviewPrefix);
            rows.push_back(kRowLogHeading);
        }

        rows.push_back(kRowOverridesSummary);
```

- [ ] **Step 3: Add Log cases to `DrawObsidianRow`**

Find:
```cpp
        case kRowNoteAddPreviewPrefix:
            DrawSettingsRow(row, top, L"Add to note preview",
                L"Text shown before what you typed, e.g. \"Add to today's note: back from the gym\"",
                settings_.noteAddPreviewPrefix, false, false, false, true);
            return;
        case kRowOverridesSummary: {
```
Replace:
```cpp
        case kRowNoteAddPreviewPrefix:
            DrawSettingsRow(row, top, L"Add to note preview",
                L"Text shown before what you typed, e.g. \"Add to today's note: back from the gym\"",
                settings_.noteAddPreviewPrefix, false, false, false, true);
            return;
        case kRowLogSummary:
            DrawObsidianSectionSummary(row, top, kSectionLog, L"Log",
                settings_.logEnabled, settings_.logPrefix, settings_.logPillLabel);
            return;
        case kRowLogEnabled:
            DrawSettingsRow(row, top, L"Log enabled",
                L"Insert a timestamped line after a heading in today's daily note",
                {}, true, settings_.logEnabled);
            return;
        case kRowLogPrefix:
            DrawSettingsRow(row, top, L"Log prefix", L"Type this followed by a space, then the log text",
                settings_.logPrefix, false, false, false, true);
            return;
        case kRowLogPillLabel:
            DrawSettingsRow(row, top, L"Log label", L"Result-row tag shown next to a pending log entry",
                settings_.logPillLabel, false, false, false, true);
            return;
        case kRowLogPreviewPrefix:
            DrawSettingsRow(row, top, L"Log preview",
                L"Text shown before what you typed, e.g. \"Log: back from a walk\"",
                settings_.logPreviewPrefix, false, false, false, true);
            return;
        case kRowLogHeading:
            DrawSettingsRow(row, top, L"Log heading", L"Exact heading line to insert after, e.g. \"## Log\"",
                settings_.logHeading, false, false, false, true);
            return;
        case kRowOverridesSummary: {
```

- [ ] **Step 4: Wire toggling and the enabled-toggle in `ChangeSetting`**

Find:
```cpp
        if (row == kRowVaultSearchSummary) { ToggleObsidianSection(kSectionVaultSearch); return; }
        if (row == kRowTaskSummary) { ToggleObsidianSection(kSectionTask); return; }
        if (row == kRowNoteAddSummary) { ToggleObsidianSection(kSectionNoteAdd); return; }
        if (row == kRowOverridesSummary) { ToggleObsidianSection(kSectionOverrides); return; }
        if (row == kRowVaultSearchEnabled) {
            settings_.vaultSearchEnabled = !settings_.vaultSearchEnabled;
            SaveSettings();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (row == kRowTaskAddEnabled) {
            settings_.taskAddEnabled = !settings_.taskAddEnabled;
            SaveSettings();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (row == kRowNoteAddEnabled) {
            settings_.noteAddEnabled = !settings_.noteAddEnabled;
            SaveSettings();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
```
Replace:
```cpp
        if (row == kRowVaultSearchSummary) { ToggleObsidianSection(kSectionVaultSearch); return; }
        if (row == kRowTaskSummary) { ToggleObsidianSection(kSectionTask); return; }
        if (row == kRowNoteAddSummary) { ToggleObsidianSection(kSectionNoteAdd); return; }
        if (row == kRowLogSummary) { ToggleObsidianSection(kSectionLog); return; }
        if (row == kRowOverridesSummary) { ToggleObsidianSection(kSectionOverrides); return; }
        if (row == kRowVaultSearchEnabled) {
            settings_.vaultSearchEnabled = !settings_.vaultSearchEnabled;
            SaveSettings();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (row == kRowTaskAddEnabled) {
            settings_.taskAddEnabled = !settings_.taskAddEnabled;
            SaveSettings();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (row == kRowNoteAddEnabled) {
            settings_.noteAddEnabled = !settings_.noteAddEnabled;
            SaveSettings();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (row == kRowLogEnabled) {
            settings_.logEnabled = !settings_.logEnabled;
            SaveSettings();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
```

- [ ] **Step 5: Wire the editable text rows in `SettingsTextFieldForRow` and `IsPrefixRow`**

Find:
```cpp
    std::wstring* SettingsTextFieldForRow(int row) {
        switch (row) {
        case kRowVaultSearchPrefix: return &settings_.vaultSearchPrefix;
        case kRowVaultSearchPillLabel: return &settings_.vaultSearchPillLabel;
        case kRowTaskPrefix: return &settings_.taskPrefix;
        case kRowTaskPillLabel: return &settings_.taskPillLabel;
        case kRowTaskPreviewPrefix: return &settings_.taskPreviewPrefix;
        case kRowNoteAddPrefix: return &settings_.noteAddPrefix;
        case kRowNoteAddPillLabel: return &settings_.noteAddPillLabel;
        case kRowNoteAddPreviewPrefix: return &settings_.noteAddPreviewPrefix;
        case kRowDailyNoteFolderOverride: return &settings_.dailyNoteFolderOverride;
        case kRowDailyNoteFormatOverride: return &settings_.dailyNoteFormatOverride;
        default: return nullptr;
        }
    }

    bool IsPrefixRow(int row) const {
        return row == kRowVaultSearchPrefix || row == kRowTaskPrefix || row == kRowNoteAddPrefix;
    }
```
Replace:
```cpp
    std::wstring* SettingsTextFieldForRow(int row) {
        switch (row) {
        case kRowVaultSearchPrefix: return &settings_.vaultSearchPrefix;
        case kRowVaultSearchPillLabel: return &settings_.vaultSearchPillLabel;
        case kRowTaskPrefix: return &settings_.taskPrefix;
        case kRowTaskPillLabel: return &settings_.taskPillLabel;
        case kRowTaskPreviewPrefix: return &settings_.taskPreviewPrefix;
        case kRowNoteAddPrefix: return &settings_.noteAddPrefix;
        case kRowNoteAddPillLabel: return &settings_.noteAddPillLabel;
        case kRowNoteAddPreviewPrefix: return &settings_.noteAddPreviewPrefix;
        case kRowLogPrefix: return &settings_.logPrefix;
        case kRowLogPillLabel: return &settings_.logPillLabel;
        case kRowLogPreviewPrefix: return &settings_.logPreviewPrefix;
        case kRowLogHeading: return &settings_.logHeading;
        case kRowDailyNoteFolderOverride: return &settings_.dailyNoteFolderOverride;
        case kRowDailyNoteFormatOverride: return &settings_.dailyNoteFormatOverride;
        default: return nullptr;
        }
    }

    bool IsPrefixRow(int row) const {
        return row == kRowVaultSearchPrefix || row == kRowTaskPrefix || row == kRowNoteAddPrefix ||
            row == kRowLogPrefix;
    }
```

- [ ] **Step 6: Add the fourth candidate to `CommitEditingRow`'s conflict check**

Find:
```cpp
        if (IsPrefixRow(editingRow_)) {
            std::wstring vaultSearchCandidate = settings_.vaultSearchPrefix;
            std::wstring taskCandidate = settings_.taskPrefix;
            std::wstring noteAddCandidate = settings_.noteAddPrefix;
            if (editingRow_ == kRowVaultSearchPrefix) vaultSearchCandidate = settingsEdit_.text;
            else if (editingRow_ == kRowTaskPrefix) taskCandidate = settingsEdit_.text;
            else if (editingRow_ == kRowNoteAddPrefix) noteAddCandidate = settingsEdit_.text;
            if (const wchar_t* error = leanlauncher::obsidian::FindPrefixConflict(
                    vaultSearchCandidate, taskCandidate, noteAddCandidate)) {
                settingsStatus_ = error;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;  // stay in edit mode so the user can fix it
            }
        }
```
Replace:
```cpp
        if (IsPrefixRow(editingRow_)) {
            std::wstring vaultSearchCandidate = settings_.vaultSearchPrefix;
            std::wstring taskCandidate = settings_.taskPrefix;
            std::wstring noteAddCandidate = settings_.noteAddPrefix;
            std::wstring logCandidate = settings_.logPrefix;
            if (editingRow_ == kRowVaultSearchPrefix) vaultSearchCandidate = settingsEdit_.text;
            else if (editingRow_ == kRowTaskPrefix) taskCandidate = settingsEdit_.text;
            else if (editingRow_ == kRowNoteAddPrefix) noteAddCandidate = settingsEdit_.text;
            else if (editingRow_ == kRowLogPrefix) logCandidate = settingsEdit_.text;
            if (const wchar_t* error = leanlauncher::obsidian::FindPrefixConflict(
                    vaultSearchCandidate, taskCandidate, noteAddCandidate, logCandidate)) {
                settingsStatus_ = error;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;  // stay in edit mode so the user can fix it
            }
        }
```

`ResetToDefaults()` resets `settings_ = quicklaunch::Settings{}`, which already picks up Task 1's new field defaults with no code change needed there.

- [ ] **Step 7: Build to confirm no syntax errors**

Run the full-app build command from Global Constraints (stop `LeanLauncher.exe` first if running), then the core-tests build and test-run commands.
Expected: both build clean; `LeanLauncherCoreTests.exe` exits 0. (This also resolves Task 3's cross-file build gap - `CommitEditingRow` now calls the 4-arg `FindPrefixConflict`.)

- [ ] **Step 8: Commit**

```bash
git add src/launcher.h
git commit -m "Add Log action to the Settings UI accordion"
```

---

## Task 6: Result dispatch and execution

**Files:**
- Modify: `src/search.h` (`AppCategory` enum)
- Modify: `src/launcher.h` (`UpdateResults`, `LaunchSelected`, secondary-actions handler, middle-click handler, `PointInAdminAction`, `DrawFooter`, `DrawActions`, pill-label rendering)

**Interfaces:**
- Consumes: `AppendLogEntry` (Task 2), `Settings::logEnabled/logPrefix/logPillLabel/logPreviewPrefix/logHeading` (Task 1).
- Produces: `takeoff::AppCategory::LogAdd`.

- [ ] **Step 1: Add `LogAdd` to `AppCategory`**

In `src/search.h`, find:
```cpp
enum class AppCategory : uint8_t {
    Application,
    System,
    File,
    Folder,
    Calculator,
    TaskAdd,
    NoteJump,
    NoteAdd
};
```
Replace:
```cpp
enum class AppCategory : uint8_t {
    Application,
    System,
    File,
    Folder,
    Calculator,
    TaskAdd,
    NoteJump,
    NoteAdd,
    LogAdd
};
```

- [ ] **Step 2: Add the prefix-detection block to `UpdateResults()`**

In `src/launcher.h`, find:
```cpp
            entry.normalizedName = Normalize(entry.name);
            const size_t noteAddIdx = apps_.size();
            apps_.push_back(std::move(entry));
            // Same strong-app-match guard as TaskAdd above.
            constexpr int kStrongMatchThreshold = 9000;
            const size_t insertPos = (topAppScore >= kStrongMatchThreshold) ? 1 : 0;
            results_.insert(results_.begin() + std::min(insertPos, results_.size()), noteAddIdx);
        }
        std::wstring noteQuery;
```
Replace:
```cpp
            entry.normalizedName = Normalize(entry.name);
            const size_t noteAddIdx = apps_.size();
            apps_.push_back(std::move(entry));
            // Same strong-app-match guard as TaskAdd above.
            constexpr int kStrongMatchThreshold = 9000;
            const size_t insertPos = (topAppScore >= kStrongMatchThreshold) ? 1 : 0;
            results_.insert(results_.begin() + std::min(insertPos, results_.size()), noteAddIdx);
        }
        std::wstring logText;
        if (settings_.obsidianEnabled && settings_.logEnabled &&
            leanlauncher::obsidian::TryParsePrefix(input_.text, settings_.logPrefix, logText)) {
            AppEntry entry;
            entry.category = AppCategory::LogAdd;
            entry.parameters = logText;
            if (obsidianVaultPath_.empty()) {
                entry.name = L"Set up your vault in Settings";
                entry.iconPath = L"notepad.exe";
            } else {
                int year = 0, month = 0, day = 0;
                leanlauncher::obsidian::GetTodayYmd(year, month, day);
                entry.path = leanlauncher::obsidian::ResolveTodayPath(
                    dailyNoteConfig_, obsidianVaultPath_, year, month, day);
                entry.name = settings_.logPreviewPrefix + logText;
                entry.iconPath = L"notepad.exe";
            }
            entry.normalizedName = Normalize(entry.name);
            const size_t logIdx = apps_.size();
            apps_.push_back(std::move(entry));
            // Same strong-app-match guard as TaskAdd above.
            constexpr int kStrongMatchThreshold = 9000;
            const size_t insertPos = (topAppScore >= kStrongMatchThreshold) ? 1 : 0;
            results_.insert(results_.begin() + std::min(insertPos, results_.size()), logIdx);
        }
        std::wstring noteQuery;
```

- [ ] **Step 3: Add the execution block to `LaunchSelected()`**

Find:
```cpp
        if (app.category == takeoff::AppCategory::NoteAdd) {
            if (app.path.empty()) {
                OpenSettings(SettingsCategory::Obsidian);
                return;
            }
            Hide();
            const bool ok = leanlauncher::obsidian::AppendNoteText(app.path, app.parameters);
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
        if (app.category == takeoff::AppCategory::NoteJump) {
```
Replace:
```cpp
        if (app.category == takeoff::AppCategory::NoteAdd) {
            if (app.path.empty()) {
                OpenSettings(SettingsCategory::Obsidian);
                return;
            }
            Hide();
            const bool ok = leanlauncher::obsidian::AppendNoteText(app.path, app.parameters);
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
        if (app.category == takeoff::AppCategory::LogAdd) {
            if (app.path.empty()) {
                OpenSettings(SettingsCategory::Obsidian);
                return;
            }
            Hide();
            const bool ok = leanlauncher::obsidian::AppendLogEntry(app.path, app.parameters, settings_.logHeading);
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
        if (app.category == takeoff::AppCategory::NoteJump) {
```

- [ ] **Step 4: Add the secondary-actions (Actions menu) block**

Find:
```cpp
        if (app.category == takeoff::AppCategory::NoteAdd) {
            if (action == 0) {
                LaunchSelected(false);
                return;
            } else if (action == 1) {
                const bool copied = CopyText(app.parameters);
                status_ = copied ? L"Text copied" : L"Clipboard is busy. Try again.";
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
        if (app.category == takeoff::AppCategory::NoteJump) {
            if (action == 0) {
```
Replace:
```cpp
        if (app.category == takeoff::AppCategory::NoteAdd) {
            if (action == 0) {
                LaunchSelected(false);
                return;
            } else if (action == 1) {
                const bool copied = CopyText(app.parameters);
                status_ = copied ? L"Text copied" : L"Clipboard is busy. Try again.";
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
        if (app.category == takeoff::AppCategory::LogAdd) {
            if (action == 0) {
                LaunchSelected(false);
                return;
            } else if (action == 1) {
                const bool copied = CopyText(app.parameters);
                status_ = copied ? L"Text copied" : L"Clipboard is busy. Try again.";
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
        if (app.category == takeoff::AppCategory::NoteJump) {
            if (action == 0) {
```

- [ ] **Step 5: Include `LogAdd` in the footer middle-click launch condition**

Find:
```cpp
                } else if (app.category == takeoff::AppCategory::TaskAdd ||
                           app.category == takeoff::AppCategory::NoteAdd) {
                    LaunchSelected(false);
```
Replace:
```cpp
                } else if (app.category == takeoff::AppCategory::TaskAdd ||
                           app.category == takeoff::AppCategory::NoteAdd ||
                           app.category == takeoff::AppCategory::LogAdd) {
                    LaunchSelected(false);
```

- [ ] **Step 6: Exclude `LogAdd` from the admin-action hint**

Find:
```cpp
        if (app.category == takeoff::AppCategory::Calculator ||
            app.category == takeoff::AppCategory::TaskAdd ||
            app.category == takeoff::AppCategory::NoteAdd ||
            app.category == takeoff::AppCategory::NoteJump) return false;
```
Replace:
```cpp
        if (app.category == takeoff::AppCategory::Calculator ||
            app.category == takeoff::AppCategory::TaskAdd ||
            app.category == takeoff::AppCategory::NoteAdd ||
            app.category == takeoff::AppCategory::NoteJump ||
            app.category == takeoff::AppCategory::LogAdd) return false;
```

- [ ] **Step 7: Add the pill-label case**

Find:
```cpp
                const wchar_t* categoryLabel = recent ? L"Recent"
                    : (app.category == takeoff::AppCategory::System ? L"System"
                    : (app.category == takeoff::AppCategory::Folder ? L"Folder"
                    : (app.category == takeoff::AppCategory::File ? L"File"
                    : (app.category == takeoff::AppCategory::TaskAdd ? settings_.taskPillLabel.c_str()
                    : (app.category == takeoff::AppCategory::NoteAdd ? settings_.noteAddPillLabel.c_str()
                    : (app.category == takeoff::AppCategory::NoteJump ? settings_.vaultSearchPillLabel.c_str() : L"Application"))))));
```
Replace:
```cpp
                const wchar_t* categoryLabel = recent ? L"Recent"
                    : (app.category == takeoff::AppCategory::System ? L"System"
                    : (app.category == takeoff::AppCategory::Folder ? L"Folder"
                    : (app.category == takeoff::AppCategory::File ? L"File"
                    : (app.category == takeoff::AppCategory::TaskAdd ? settings_.taskPillLabel.c_str()
                    : (app.category == takeoff::AppCategory::NoteAdd ? settings_.noteAddPillLabel.c_str()
                    : (app.category == takeoff::AppCategory::LogAdd ? settings_.logPillLabel.c_str()
                    : (app.category == takeoff::AppCategory::NoteJump ? settings_.vaultSearchPillLabel.c_str() : L"Application")))))));
```
(Adding one nested ternary adds one open paren and one close paren - the replacement's trailing `)))))));` has one more `)` than the original's `))))));`. If the build reports a mismatched-parenthesis error here, count again: there must be exactly one `(app.category == ...` per line from `System` through `NoteJump` inclusive - 7 lines, 7 opens, 7 closes before the final `;`.)

- [ ] **Step 8: Add the footer hint**

Find:
```cpp
            } else if (app.category == takeoff::AppCategory::NoteAdd) {
                Text(L"Add to note", D2D1::RectF(24, top, 156, height_),
                    hintFormat_.Get(), actionsOpen_ ? Foreground() : Muted());
                Key(L"\u21B5", 96, top + (kFooterHeight - 22) / 2, 24);
            } else if (app.category == takeoff::AppCategory::NoteJump) {
```
Replace:
```cpp
            } else if (app.category == takeoff::AppCategory::NoteAdd) {
                Text(L"Add to note", D2D1::RectF(24, top, 156, height_),
                    hintFormat_.Get(), actionsOpen_ ? Foreground() : Muted());
                Key(L"\u21B5", 96, top + (kFooterHeight - 22) / 2, 24);
            } else if (app.category == takeoff::AppCategory::LogAdd) {
                Text(L"Add log entry", D2D1::RectF(24, top, 156, height_),
                    hintFormat_.Get(), actionsOpen_ ? Foreground() : Muted());
                Key(L"\u21B5", 96, top + (kFooterHeight - 22) / 2, 24);
            } else if (app.category == takeoff::AppCategory::NoteJump) {
```

- [ ] **Step 9: Add the Actions-panel label set in `DrawActions`**

Find:
```cpp
        const bool isCalc = (app.category == takeoff::AppCategory::Calculator);
        const bool isTaskAdd = (app.category == takeoff::AppCategory::TaskAdd);
        const bool isNoteAdd = (app.category == takeoff::AppCategory::NoteAdd);
        const bool isNoteJump = (app.category == takeoff::AppCategory::NoteJump);
        const bool isFileOrFolder = (app.category == takeoff::AppCategory::File ||
                                     app.category == takeoff::AppCategory::Folder);
        const wchar_t* appLabels[] = {L"Open as Administrator", L"Copy app name", L"Copy launch path"};
        const wchar_t* fileLabels[] = {L"Open", L"Open containing folder", L"Copy file path"};
        const wchar_t* calcLabels[] = {L"Copy result", L"Copy calculation", L"Open Windows Calculator"};
        const wchar_t* taskLabels[] = {L"Add task", L"Copy task text", L"Open today's note"};
        const wchar_t* noteAddLabels[] = {L"Add to note", L"Copy text", L"Open today's note"};
        const wchar_t* noteLabels[] = {L"Open in Obsidian", L"Copy note title", L"Reveal in Explorer"};
        const wchar_t** labels = isCalc ? calcLabels
            : (isTaskAdd ? taskLabels
            : (isNoteAdd ? noteAddLabels
            : (isNoteJump ? noteLabels
            : (isFileOrFolder ? fileLabels : appLabels))));
```
Replace:
```cpp
        const bool isCalc = (app.category == takeoff::AppCategory::Calculator);
        const bool isTaskAdd = (app.category == takeoff::AppCategory::TaskAdd);
        const bool isNoteAdd = (app.category == takeoff::AppCategory::NoteAdd);
        const bool isLogAdd = (app.category == takeoff::AppCategory::LogAdd);
        const bool isNoteJump = (app.category == takeoff::AppCategory::NoteJump);
        const bool isFileOrFolder = (app.category == takeoff::AppCategory::File ||
                                     app.category == takeoff::AppCategory::Folder);
        const wchar_t* appLabels[] = {L"Open as Administrator", L"Copy app name", L"Copy launch path"};
        const wchar_t* fileLabels[] = {L"Open", L"Open containing folder", L"Copy file path"};
        const wchar_t* calcLabels[] = {L"Copy result", L"Copy calculation", L"Open Windows Calculator"};
        const wchar_t* taskLabels[] = {L"Add task", L"Copy task text", L"Open today's note"};
        const wchar_t* noteAddLabels[] = {L"Add to note", L"Copy text", L"Open today's note"};
        const wchar_t* logAddLabels[] = {L"Add log entry", L"Copy text", L"Open today's note"};
        const wchar_t* noteLabels[] = {L"Open in Obsidian", L"Copy note title", L"Reveal in Explorer"};
        const wchar_t** labels = isCalc ? calcLabels
            : (isTaskAdd ? taskLabels
            : (isNoteAdd ? noteAddLabels
            : (isLogAdd ? logAddLabels
            : (isNoteJump ? noteLabels
            : (isFileOrFolder ? fileLabels : appLabels)))));
```

- [ ] **Step 10: Build and run tests**

Run the full-app build command from Global Constraints (stop `LeanLauncher.exe` first if running).
Expected: builds clean.

Run the core-tests build command, then `./build/Release/LeanLauncherCoreTests.exe`.
Expected: builds clean, exit code 0, no `FAIL:` lines (nothing in this task changes core-tests behavior, but this confirms no regression).

- [ ] **Step 11: Manually verify against the real vault**

Launch `build/Release/LeanLauncher.exe`, open Settings -> Obsidian, expand "Log" (should default to enabled, prefix `l`, label `Log`, heading `## Log`), then close Settings and type `l testing the new log action` in the launcher. Confirm:
- The result row reads "Log: testing the new log action" with a "Log" pill.
- Pressing Enter inserts `- HH:MM: testing the new log action` at the end of the `## Log` section in today's daily note (`06 BJ/10 Daily/YYYY/MM/YYYY-MM-DD.md` in the configured vault), matching where the existing QuickAdd "🌱 Log Entry" choice would have placed it.
- Repeating with Obsidian closed produces the same result (confirms no CLI/Obsidian dependency).

- [ ] **Step 12: Commit**

```bash
git add src/search.h src/launcher.h
git commit -m "Wire the Log action into result dispatch, execution, and the Actions menu"
```
