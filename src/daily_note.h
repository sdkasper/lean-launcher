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
#include <vector>

#include "obsidian_config.h"

namespace leanlauncher {
namespace obsidian {

namespace fs = std::filesystem;

// 0 = Sunday ... 6 = Saturday, for a Gregorian date (Sakamoto's method).
inline int DayOfWeek(int year, int month, int day) {
    static constexpr int kOffsets[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (month < 3) year -= 1;
    return (year + year / 4 - year / 100 + year / 400 + kOffsets[(month + 11) % 12] + day) % 7;
}

// Expands a Moment.js-style date format (US-030) into `out`. Supported:
// YYYY YY, MMMM MMM MM M, DD D, dddd ddd, and [literal text]. Names are
// English - Obsidian's default Moment locale. Returns false if the format
// uses any other token; those letters are copied to `out` unchanged.
//
// Tokens are read as whole runs of the same letter, which is how Moment
// matches them: "MMMM" is the month name, never two "MM"s, and "DDDD" (day
// of year) is an unsupported run rather than two "DD"s. A letter that isn't
// part of a supported run - including one right after a token, like the "o"
// in "Do" (ordinal day) - makes the whole format unsupported, so callers can
// fall back instead of writing a garbled path.
inline bool ExpandDateFormat(const std::wstring& format, int year, int month, int day, std::wstring& out) {
    static constexpr const wchar_t* kMonths[] = {L"January", L"February", L"March", L"April", L"May", L"June",
        L"July", L"August", L"September", L"October", L"November", L"December"};
    static constexpr const wchar_t* kWeekdays[] = {L"Sunday", L"Monday", L"Tuesday", L"Wednesday",
        L"Thursday", L"Friday", L"Saturday"};
    out.clear();
    out.reserve(format.size() + 16);
    bool supported = true;
    wchar_t buf[8];
    size_t i = 0;
    while (i < format.size()) {
        const wchar_t ch = format[i];
        if (ch == L'[') {
            const size_t close = format.find(L']', i + 1);
            if (close != std::wstring::npos) {
                out.append(format, i + 1, close - i - 1);
                i = close + 1;
                continue;
            }
            // Unclosed "[": the bracket would end up in the note name, which
            // breaks wikilinks - treat it as unsupported, like a stray "]".
            supported = false;
        } else if (ch == L']') {
            supported = false;
        }
        if (!std::iswalpha(ch)) {
            out.push_back(ch);
            ++i;
            continue;
        }
        size_t run = 1;
        while (i + run < format.size() && format[i + run] == ch) ++run;
        const std::wstring_view month3(kMonths[month - 1], 3);
        const std::wstring_view weekday(kWeekdays[DayOfWeek(year, month, day)]);
        if (ch == L'Y' && run == 4) { swprintf_s(buf, L"%04d", year); out += buf; }
        else if (ch == L'Y' && run == 2) { swprintf_s(buf, L"%02d", year % 100); out += buf; }
        else if (ch == L'M' && run == 4) { out += kMonths[month - 1]; }
        else if (ch == L'M' && run == 3) { out += month3; }
        else if (ch == L'M' && run == 2) { swprintf_s(buf, L"%02d", month); out += buf; }
        else if (ch == L'M' && run == 1) { out += std::to_wstring(month); }
        else if (ch == L'D' && run == 2) { swprintf_s(buf, L"%02d", day); out += buf; }
        else if (ch == L'D' && run == 1) { out += std::to_wstring(day); }
        else if (ch == L'd' && run == 4) { out += weekday; }
        else if (ch == L'd' && run == 3) { out += weekday.substr(0, 3); }
        else {
            supported = false;
            out.append(format, i, run);
        }
        i += run;
    }
    return supported;
}

// Expands a date format for display/paths; unsupported tokens pass through
// unchanged. Callers that build a path check IsDateFormatFullySupported first.
inline std::wstring FormatDateTokens(const std::wstring& format, int year, int month, int day) {
    std::wstring result;
    ExpandDateFormat(format, year, month, day, result);
    return result;
}

// True when ExpandDateFormat understands every token in `format`. The date
// used for the check doesn't matter - support depends only on the tokens.
inline bool IsDateFormatFullySupported(const std::wstring& format) {
    std::wstring ignored;
    return ExpandDateFormat(format, 2000, 1, 1, ignored);
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
//
// A ":" anywhere is rejected as well ("x/C:y" is drive-relative once joined,
// "note:x" writes an NTFS alternate data stream), as is any segment that is a
// reserved device name (CON, NUL, COM1, ... with or without an extension) or
// made only of dots and spaces - Win32 trims trailing dots/spaces, so ".. "
// becomes a ".." traversal after this check (NFR-009).
inline bool IsReservedDeviceName(std::wstring_view segment) {
    std::wstring_view stem = segment.substr(0, segment.find(L'.'));
    while (!stem.empty() && stem.back() == L' ') stem.remove_suffix(1);
    static constexpr std::wstring_view kNames[] = {L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"};
    for (const std::wstring_view name : kNames) {
        if (stem.size() == name.size() && _wcsnicmp(stem.data(), name.data(), name.size()) == 0) return true;
    }
    if (stem.size() == 4 && (_wcsnicmp(stem.data(), L"COM", 3) == 0 || _wcsnicmp(stem.data(), L"LPT", 3) == 0)) {
        const wchar_t n = stem[3];
        return (n >= L'1' && n <= L'9') || n == L'¹' || n == L'²' || n == L'³';
    }
    return false;
}

inline bool IsUnsafeVaultRelativePath(const std::wstring& value) {
    if (value.empty()) return false;
    if (value.find(L':') != std::wstring::npos) return true;
    const fs::path p(value);
    if (p.is_absolute() || p.has_root_name() || p.has_root_directory()) return true;
    for (const auto& part : p) {
        const std::wstring& segment = part.native();
        if (segment.empty() || segment == L".") continue; // "Daily/" ends in an empty element
        if (segment.find_first_not_of(L". ") == std::wstring::npos) return true;
        if (IsReservedDeviceName(segment)) return true;
    }
    return false;
}

// Today's daily-note name (no extension) under config.format. Falls back to
// the safe default format whenever the configured format contains anything
// ExpandDateFormat can't fully account for (e.g. a Moment.js token like
// "ww" or "Do"), or expands to something unsafe, rather than silently
// producing a garbled or traversal-y filename. The *expanded* name is what
// gets checked: a [literal] can spell "..", "C:" or "/" that the raw format
// doesn't contain ("[..]/[..]/x"), so checking the format alone isn't enough.
inline std::wstring FormatTodayNoteName(const DailyNoteConfig& config, int year, int month, int day) {
    std::wstring name;
    if (ExpandDateFormat(config.format, year, month, day, name) && !name.empty() &&
        name.back() != L'/' && name.back() != L'\\' && !IsUnsafeVaultRelativePath(name)) {
        return name;
    }
    return FormatDateTokens(L"YYYY-MM-DD", year, month, day);
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

// Splits capture text into its lines (US-028). The search box is single-line,
// so a break is written as `\n` or ` // ` (spaces around it are dropped);
// real CR/LF/CRLF also count in case one arrives some other way. `\\n` is the
// escape for a literal backslash-n; every other backslash is kept, so
// `C:\temp` survives - but `C:\notes` splits at its `\n`, typed or pasted, by
// design (US-028); `C:\\notes` keeps it. Trailing spaces are trimmed from
// each line (two of them would be a Markdown hard break), then empty lines
// at the start and end are dropped - so a capture that is only breaks comes
// back empty.
inline std::vector<std::wstring> SplitCaptureLines(std::wstring_view text) {
    std::vector<std::wstring> lines(1);
    auto breakLine = [&lines] { lines.emplace_back(); };
    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t ch = text[i];
        if (ch == L'\r') {
            if (i + 1 < text.size() && text[i + 1] == L'\n') ++i;
            breakLine();
        } else if (ch == L'\n') {
            breakLine();
        } else if (ch == L'\\' && i + 2 < text.size() && text[i + 1] == L'\\' && text[i + 2] == L'n') {
            lines.back() += L"\\n";
            i += 2;
        } else if (ch == L'\\' && i + 1 < text.size() && text[i + 1] == L'n') {
            breakLine();
            ++i;
        } else if (ch == L' ' && text.substr(i, 4) == L" // ") {
            breakLine();
            i += 3;
            while (i + 1 < text.size() && text[i + 1] == L' ') ++i;
        } else {
            lines.back().push_back(ch);
        }
    }
    for (std::wstring& line : lines) {
        while (!line.empty() && std::iswspace(line.back())) line.pop_back();
    }
    auto first = std::find_if(lines.begin(), lines.end(), [](const std::wstring& l) { return !l.empty(); });
    auto last = std::find_if(lines.rbegin(), lines.rend(), [](const std::wstring& l) { return !l.empty(); }).base();
    if (first >= last) return {};
    return std::vector<std::wstring>(first, last);
}

// The capture preview row's version of the text: lines joined with " ⏎ ",
// built from the same split as the write so the two can't disagree.
inline std::wstring CapturePreviewText(std::wstring_view text) {
    std::wstring preview;
    for (const std::wstring& line : SplitCaptureLines(text)) {
        if (!preview.empty()) preview += L" \u23CE ";
        preview += line;
    }
    return preview;
}

// The search box drops control characters, which used to glue pasted lines
// together ("line1line2"). Paste runs clipboard text through this first:
// breaks at the ends are dropped (copying one line usually brings its
// newline along) and the rest become `\n`, which captures turn back into
// real line breaks.
inline std::wstring PastedTextForInput(std::wstring_view text) {
    const size_t start = text.find_first_not_of(L"\r\n");
    if (start == std::wstring_view::npos) return {};
    const size_t end = text.find_last_not_of(L"\r\n");
    std::wstring result;
    for (size_t i = start; i <= end; ++i) {
        if (text[i] == L'\r' || text[i] == L'\n') {
            if (text[i] == L'\r' && i + 1 <= end && text[i + 1] == L'\n') ++i;
            result += L"\\n";
        } else {
            result.push_back(text[i]);
        }
    }
    return result;
}

// Writes `lead` + the first line, then each extra line indented 2 spaces (the
// content column after "- ") so Obsidian keeps it inside the same list item.
// Blank extra lines are dropped - they would split the list.
inline std::wstring BuildListItem(const std::wstring& lead, std::wstring_view text) {
    const std::vector<std::wstring> lines = SplitCaptureLines(text);
    std::wstring item = lead + (lines.empty() ? std::wstring() : lines[0]) + L"\n";
    for (size_t i = 1; i < lines.size(); ++i) {
        const size_t start = lines[i].find_first_not_of(L" \t");
        if (start == std::wstring::npos) continue;
        item += L"  " + lines[i].substr(start) + L"\n";
    }
    return item;
}

// Builds a Markdown checklist line for one task.
inline std::wstring BuildTaskLine(std::wstring_view text) {
    return BuildListItem(L"- [ ] ", text);
}

// Builds plain text (no bullet, no checklist) for the "a " prefix: each line
// as written, blank middle lines kept.
inline std::wstring BuildPlainLine(std::wstring_view text) {
    std::wstring plain;
    for (const std::wstring& line : SplitCaptureLines(text)) plain += line + L"\n";
    return plain.empty() ? std::wstring(L"\n") : plain;
}

// Forward declaration: FormatTimeHHMM is defined below (after
// FormatIsoTimestamp, per the log-capture task brief's placement), but
// BuildLogLine - placed here after BuildPlainLine, also per the brief - needs
// to call it first in file order.
inline std::wstring FormatTimeHHMM();

// Builds a timestamped log line: "- HH:MM: <text>\n".
inline std::wstring BuildLogLine(std::wstring_view text) {
    return BuildListItem(L"- " + FormatTimeHHMM() + L": ", text);
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
