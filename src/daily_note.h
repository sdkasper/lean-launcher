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
#include <fstream>
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

// Returns true if every letter in `format` is consumed by a recognized
// YYYY/MM/DD token (in any combination/order, with any non-letter
// separators around them). Used to detect formats FormatDateTokens can't
// fully honor, so callers can fall back instead of silently producing a
// garbled path.
//
// Note: a naive "MM" check would greedily match a longer run like "MMMM"
// (Moment.js's full-month-name token) as two consecutive MM tokens and
// incorrectly call it fully supported - exactly the silent-garbling bug
// this function exists to catch. Guard against that by requiring the "MM"
// match not be immediately followed by another 'M'.
inline bool IsDateFormatFullySupported(const std::wstring& format) {
    size_t i = 0;
    while (i < format.size()) {
        if (format.compare(i, 4, L"YYYY") == 0) { i += 4; }
        else if (format.compare(i, 2, L"MM") == 0 &&
                 (i + 2 >= format.size() || format[i + 2] != L'M')) { i += 2; }
        else if (format.compare(i, 2, L"DD") == 0) { i += 2; }
        else if (std::iswalpha(format[i])) { return false; }
        else { ++i; }
    }
    return true;
}

inline std::wstring ResolveTodayPath(const DailyNoteConfig& config, const std::wstring& vaultPath,
    int year, int month, int day) {
    fs::path base(vaultPath);
    if (!config.folder.empty()) {
        base /= config.folder;
    }
    // Fall back to the safe default format whenever the configured format
    // contains anything FormatDateTokens can't fully account for (e.g. a
    // Moment.js token like MMMM), rather than silently producing a garbled
    // filename. Expanding token support is out of scope here.
    const std::wstring& formatToUse =
        IsDateFormatFullySupported(config.format) ? config.format : std::wstring(L"YYYY-MM-DD");
    const std::wstring filename = FormatDateTokens(formatToUse, year, month, day) + L".md";
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

    // Obsidian doesn't guarantee a trailing newline on saved notes. Appending
    // straight onto a file whose last byte isn't '\n' would merge the new
    // task into the previous line, corrupting both. Detect that case with a
    // separate read handle (FILE_APPEND_DATA doesn't reliably support reads)
    // and prepend a newline to compensate.
    bool needsLeadingNewline = false;
    if (!isNewFile) {
        std::ifstream check(path, std::ios::binary | std::ios::ate);
        if (check) {
            const std::streamoff size = check.tellg();
            if (size > 0) {
                check.seekg(-1, std::ios::end);
                char lastChar = 0;
                check.read(&lastChar, 1);
                needsLeadingNewline = (lastChar != '\n');
            }
        }
    }

    HANDLE file = CreateFileW(notePath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    std::wstring content;
    if (isNewFile) {
        content = L"---\ncreated: " + FormatIsoTimestamp() + L"\n---\n\n";
    } else if (needsLeadingNewline) {
        content = L"\n";
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
