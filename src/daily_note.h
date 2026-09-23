#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
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
// anything else in the format string passes through unchanged. The DD branch
// mirrors IsDateFormatFullySupported's lookahead guard (below) so this
// function can't garble a token like "DDDD" if a future caller ever reaches
// it without validating the format first - today ResolveTodayPath is the
// only caller and always validates, but the two functions must agree on
// what counts as a real DD token.
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
        } else if (format.compare(i, 2, L"DD") == 0 &&
                   (i + 2 >= format.size() || format[i + 2] != L'D')) {
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
// match not be immediately followed by another 'M'. "DD" needs the same
// guard because Moment.js's "DDDD" (day of year) would misread the same way.
inline bool IsDateFormatFullySupported(const std::wstring& format) {
    size_t i = 0;
    while (i < format.size()) {
        if (format.compare(i, 4, L"YYYY") == 0) { i += 4; }
        else if (format.compare(i, 2, L"MM") == 0 &&
                 (i + 2 >= format.size() || format[i + 2] != L'M')) { i += 2; }
        else if (format.compare(i, 2, L"DD") == 0 &&
                 (i + 2 >= format.size() || format[i + 2] != L'D')) { i += 2; }
        else if (std::iswalpha(format[i])) { return false; }
        else { ++i; }
    }
    return true;
}

// Rejects vault-relative config strings that could steer a write outside the
// vault: absolute paths (fs::path::is_absolute() also catches UNC paths like
// "\\attacker.example.com\share", which would trigger an outbound SMB auth
// handshake) and ".." traversal segments. `folder`/`format` come straight out
// of the vault's own .obsidian/daily-notes.json (or Periodic Notes/Journals
// plugin config) with no validation upstream - a synced/shared vault could
// have a tampered config, so this codebase must not trust it. Rooted paths
// without a drive ("\Windows") and drive-relative ones ("C:Daily") aren't
// is_absolute() on Windows, but joining either onto the vault path discards
// the vault's own root - so any root name/directory counts as unsafe too.
inline bool IsUnsafeVaultRelativePath(const std::wstring& value) {
    if (value.empty()) return false;
    const fs::path p(value);
    if (p.is_absolute() || p.has_root_name() || p.has_root_directory()) return true;
    for (const auto& part : p) {
        if (part == L"..") return true;
    }
    return false;
}

// Today's daily-note name (no extension) under config.format. Falls back to
// the safe default format whenever the configured format contains anything
// FormatDateTokens can't fully account for (e.g. a Moment.js token like
// MMMM), or is itself unsafe (e.g. "../../secret"), rather than silently
// producing a garbled or traversal-y filename. Expanding token support is
// out of scope here.
inline std::wstring FormatTodayNoteName(const DailyNoteConfig& config, int year, int month, int day) {
    const bool formatIsSafe = !IsUnsafeVaultRelativePath(config.format);
    const std::wstring& formatToUse =
        (formatIsSafe && IsDateFormatFullySupported(config.format)) ? config.format
                                                                     : std::wstring(L"YYYY-MM-DD");
    return FormatDateTokens(formatToUse, year, month, day);
}

inline std::wstring ResolveTodayPath(const DailyNoteConfig& config, const std::wstring& vaultPath,
    int year, int month, int day) {
    fs::path base(vaultPath);
    if (!config.folder.empty() && !IsUnsafeVaultRelativePath(config.folder)) {
        base /= config.folder;
    }
    const std::wstring filename = FormatTodayNoteName(config, year, month, day) + L".md";
    return (base / filename).wstring();
}

// Today's daily note as a vault-relative ref (forward slashes, no ".md") -
// the same shape NoteIndex produces and OpenNoteInObsidian expects. Follows
// ResolveTodayPath's folder/format rules exactly (US-026).
inline std::wstring TodayNoteRef(const DailyNoteConfig& config, int year, int month, int day) {
    std::wstring ref;
    if (!config.folder.empty() && !IsUnsafeVaultRelativePath(config.folder)) {
        ref = config.folder;
        if (ref.back() != L'/' && ref.back() != L'\\') ref += L'/';
    }
    ref += FormatTodayNoteName(config, year, month, day);
    std::replace(ref.begin(), ref.end(), L'\\', L'/');
    return ref;
}

// Normalizes a user-entered capture target note ("Inbox/Tasks",
// "Inbox\Tasks.md", " Scratch.MD ") into a vault-relative ref: trimmed,
// forward slashes, no ".md". Returns false for anything that isn't a note
// inside the vault - empty, absolute/UNC/rooted, ".." traversal, or no note
// name at all ("Inbox/", ".md") - so callers can reject it at save time
// instead of silently writing somewhere else (US-025).
inline bool NormalizeTargetNoteRef(const std::wstring& target, std::wstring& outRef) {
    const size_t first = target.find_first_not_of(L" \t");
    if (first == std::wstring::npos) return false;
    const size_t last = target.find_last_not_of(L" \t");
    std::wstring ref = target.substr(first, last - first + 1);
    if (IsUnsafeVaultRelativePath(ref)) return false;
    std::replace(ref.begin(), ref.end(), L'\\', L'/');
    if (ref.size() >= 3 && _wcsicmp(ref.c_str() + ref.size() - 3, L".md") == 0) {
        ref.resize(ref.size() - 3);
    }
    if (ref.empty() || ref.back() == L'/') return false;
    if (fs::path(ref).filename() == L".") return false;
    outRef = std::move(ref);
    return true;
}

// Which note `o .` opens (US-026): QuickOpenTarget 1/2/3 pick the task,
// append, or log target; 0 - or a target that isn't set - means today's
// daily note, signalled by an empty return.
inline std::wstring SelectQuickOpenTarget(int choice, const std::wstring& taskTarget,
    const std::wstring& noteAddTarget, const std::wstring& logTarget) {
    switch (choice) {
    case 1: return taskTarget;
    case 2: return noteAddTarget;
    case 3: return logTarget;
    default: return {};
    }
}

inline void GetTodayYmd(int& year, int& month, int& day) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    year = st.wYear;
    month = st.wMonth;
    day = st.wDay;
}

// Strips embedded CR/LF from `text` so a pasted multi-line entry can never
// split into more than one line/list item.
inline std::wstring StripLineBreaks(std::wstring_view text) {
    std::wstring clean;
    clean.reserve(text.size());
    for (wchar_t ch : text) {
        if (ch == L'\r' || ch == L'\n') continue;
        clean.push_back(ch);
    }
    return clean;
}

// Builds a Markdown checklist line for one task.
inline std::wstring BuildTaskLine(std::wstring_view text) {
    return L"- [ ] " + StripLineBreaks(text) + L"\n";
}

// Builds a plain text line (no bullet, no checklist) for the "a " prefix.
inline std::wstring BuildPlainLine(std::wstring_view text) {
    return StripLineBreaks(text) + L"\n";
}

// Forward declaration: FormatTimeHHMM is defined below (after
// FormatIsoTimestamp, per the log-capture task brief's placement), but
// BuildLogLine - placed here after BuildPlainLine, also per the brief - needs
// to call it first in file order.
inline std::wstring FormatTimeHHMM();

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

// Minimal frontmatter written when a daily note is created for the first
// time by any of the append actions below - just a created timestamp.
inline std::wstring BuildFrontmatter() {
    return L"---\ncreated: " + FormatIsoTimestamp() + L"\n---\n\n";
}

// Appends one already-formatted line to the note at notePath, creating the
// file (with parent directories and minimal frontmatter) if it doesn't exist
// yet. Returns false on any I/O failure - callers must surface this to the
// user rather than silently dropping the entry (spec: "task never silently
// dropped", and the same guarantee extends to plain daily-note text).
inline bool AppendLine(const std::wstring& notePath, const std::wstring& line) {
    std::error_code ec;
    const fs::path path(notePath);
    fs::create_directories(path.parent_path(), ec);

    const bool isNewFile = !fs::exists(path, ec);

    // Obsidian doesn't guarantee a trailing newline on saved notes. Appending
    // straight onto a file whose last byte isn't '\n' would merge the new
    // line into the previous one, corrupting both. Detect that case with a
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
        content = BuildFrontmatter();
    } else if (needsLeadingNewline) {
        content = L"\n";
    }
    content += line;

    bool ok = false;
    const std::string utf8 = WideToUtf8(content);
    if (!utf8.empty()) {
        DWORD written = 0;
        ok = WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr) &&
             written == utf8.size();
    }
    CloseHandle(file);
    return ok;
}

// Appends one task checklist line to the note at notePath.
inline bool AppendTask(const std::wstring& notePath, std::wstring_view taskText) {
    return AppendLine(notePath, BuildTaskLine(taskText));
}

// Appends one plain text line (the "a " prefix) to the note at notePath.
inline bool AppendNoteText(const std::wstring& notePath, std::wstring_view text) {
    return AppendLine(notePath, BuildPlainLine(text));
}

// Walks `pos` (which must be the start of a line) backward over any run of
// blank or whitespace-only lines immediately preceding it, stopping right
// after the end of the last non-blank line. Used by AppendLogEntry so that
// a blank separator line between a section's content and the next heading
// stays between the new entry and that heading, instead of being pushed
// above the newly inserted line.
inline size_t SkipBlankLinesBackward(const std::wstring& content, size_t pos) {
    size_t result = pos;
    while (result > 0 && content[result - 1] == L'\n') {
        const size_t newlinePos = result - 1;
        const size_t prevNewline = (newlinePos == 0) ? std::wstring::npos
                                                       : content.rfind(L'\n', newlinePos - 1);
        const size_t lineStart = (prevNewline == std::wstring::npos) ? 0 : prevNewline + 1;

        bool blank = true;
        for (size_t i = lineStart; i < newlinePos; ++i) {
            if (!std::iswspace(content[i])) { blank = false; break; }
        }
        if (!blank) break;
        result = lineStart;
    }
    return result;
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
        newContent = BuildFrontmatter() + line;
    } else {
        const std::string raw = ReadFileUtf8(path);
        // ReadFileUtf8 returns "" both for a legitimately empty file and for
        // a read that failed outright (a transient sharing violation from a
        // sync client, an unhydrated cloud-placeholder file, an AV scanner
        // lock, etc). Treating a failed read as "empty note" would make the
        // CREATE_ALWAYS write below silently truncate the user's whole note
        // down to just the new line. Tell the two apart using the file's
        // actual on-disk size, and bail out - never rewrite - if the read
        // came back empty for a file that isn't.
        if (raw.empty() && fs::file_size(path, ec) > 0) return false;

        const std::wstring content = Utf8ToWide(raw);
        // A non-empty read that fails to decode as UTF-8 is just as
        // dangerous to proceed on - never rewrite the file in that case
        // either.
        if (!raw.empty() && content.empty()) return false;

        const size_t boundary = FindHeadingSectionEnd(content, heading);
        // A boundary of npos (heading not found) or content.size() (the
        // heading's section runs off the end of the file, i.e. there is no
        // next heading) both mean "no heading to preserve spacing before" -
        // only a boundary that actually lands on a later heading line calls
        // for the backward walk over any blank separator line.
        const bool boundaryIsHeadingMatch = boundary != std::wstring::npos && boundary < content.size();
        size_t insertAt = (boundary == std::wstring::npos) ? content.size() : boundary;
        if (boundaryIsHeadingMatch) {
            insertAt = SkipBlankLinesBackward(content, insertAt);
        }
        const bool needsLeadingNewline = insertAt > 0 && content[insertAt - 1] != L'\n';

        newContent = content.substr(0, insertAt);
        if (needsLeadingNewline) newContent += L'\n';
        newContent += line;
        newContent += content.substr(insertAt);
    }

    const std::string utf8 = WideToUtf8(newContent);

    // Write to a temp file alongside notePath first, then atomically replace
    // it - this function rewrites the whole file (unlike AppendLine's pure
    // FILE_APPEND_DATA), so a CREATE_ALWAYS write straight to notePath that
    // fails or is cut short partway through (full disk, a sync client
    // holding a lock, etc) would truncate the user's note to just the new
    // content, or to zero bytes, while reporting a generic error.
    const fs::path tempPath = path.parent_path() / (path.filename().wstring() + L".tmp");
    HANDLE tempFile = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (tempFile == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool writeOk = WriteFile(tempFile, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr) &&
        written == static_cast<DWORD>(utf8.size());
    FlushFileBuffers(tempFile);
    CloseHandle(tempFile);
    if (!writeOk) {
        DeleteFileW(tempPath.c_str());
        return false;
    }
    if (!MoveFileExW(tempPath.c_str(), notePath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tempPath.c_str());
        return false;
    }
    return true;
}

}  // namespace obsidian
}  // namespace leanlauncher
