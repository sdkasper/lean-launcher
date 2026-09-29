#pragma once

#include "daily_note.h"
#include "typed_input.h"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Preview panel (US-045/US-046): the pure parts - markdown scanning,
// frontmatter, file classification, bounded reading, panel geometry and the
// stale-result gate. The launcher owns the window, threads and drawing.
// Nothing here writes to disk or starts Obsidian.
namespace leanlauncher {
namespace preview {

enum class SpanStyle { Heading1, Heading2, Heading3, Bold, Italic, Link, Tag, Code, Muted };

struct Span {
    size_t start = 0;
    size_t length = 0;
    SpanStyle style = SpanStyle::Muted;
};

struct ScannedText {
    std::wstring text;
    std::vector<Span> spans;
};

namespace detail {

// Inline pass over one line: **bold**, *italic*, `code`, [[link|alias]], #tag.
inline void ScanInline(std::wstring_view line, ScannedText& out) {
    size_t i = 0;
    while (i < line.size()) {
        const auto emit = [&](std::wstring_view text, SpanStyle style) {
            out.spans.push_back({out.text.size(), text.size(), style});
            out.text.append(text);
        };
        if (line.compare(i, 2, L"**") == 0) {
            const size_t end = line.find(L"**", i + 2);
            if (end != std::wstring_view::npos && end > i + 2) { emit(line.substr(i + 2, end - i - 2), SpanStyle::Bold); i = end + 2; continue; }
        }
        if (line[i] == L'`') {
            const size_t end = line.find(L'`', i + 1);
            if (end != std::wstring_view::npos && end > i + 1) { emit(line.substr(i + 1, end - i - 1), SpanStyle::Code); i = end + 1; continue; }
        }
        if (line.compare(i, 2, L"[[") == 0) {
            const size_t end = line.find(L"]]", i + 2);
            if (end != std::wstring_view::npos) {
                std::wstring_view inner = line.substr(i + 2, end - i - 2);
                const size_t bar = inner.find(L'|');
                if (bar != std::wstring_view::npos) inner = inner.substr(bar + 1);
                emit(inner, SpanStyle::Link);
                i = end + 2;
                continue;
            }
        }
        if (line[i] == L'*' && i + 1 < line.size() && line[i + 1] != L' ' && line[i + 1] != L'*') {
            const size_t end = line.find(L'*', i + 1);
            if (end != std::wstring_view::npos && end > i + 1 && line[end - 1] != L' ') {
                emit(line.substr(i + 1, end - i - 1), SpanStyle::Italic);
                i = end + 1;
                continue;
            }
        }
        if (line[i] == L'#' && (i == 0 || line[i - 1] == L' ') && i + 1 < line.size() && iswalnum(line[i + 1])) {
            size_t end = i + 1;
            while (end < line.size() && (iswalnum(line[end]) || line[end] == L'/' || line[end] == L'-' || line[end] == L'_')) ++end;
            emit(line.substr(i, end - i), SpanStyle::Tag);
            i = end;
            continue;
        }
        out.text += line[i++];
    }
}

inline std::wstring_view TrimView(std::wstring_view s) {
    while (!s.empty() && iswspace(s.front())) s.remove_prefix(1);
    while (!s.empty() && iswspace(s.back())) s.remove_suffix(1);
    return s;
}

} // namespace detail

// One pass, line by line. Headings lose their #s; "- [ ]"/"- [x]" become
// ☐/☑; "- " and "* " become "• "; fenced code is kept verbatim as Code.
// Callouts, quotes and tables are left as plain text.
inline ScannedText ScanMarkdown(std::wstring_view body) {
    ScannedText out;
    bool fenced = false;
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t nl = body.find(L'\n', pos);
        if (nl == std::wstring_view::npos) nl = body.size();
        std::wstring_view line = body.substr(pos, nl - pos);
        if (!line.empty() && line.back() == L'\r') line.remove_suffix(1);
        const bool last = nl >= body.size();
        pos = nl + 1;
        if (detail::TrimView(line).substr(0, 3) == L"```") {
            fenced = !fenced;
            continue;
        }
        if (fenced) {
            out.spans.push_back({out.text.size(), line.size(), SpanStyle::Code});
            out.text.append(line);
        } else {
            size_t hashes = 0;
            while (hashes < line.size() && hashes < 6 && line[hashes] == L'#') ++hashes;
            if (hashes > 0 && hashes < line.size() && line[hashes] == L' ') {
                const std::wstring_view title = line.substr(hashes + 1);
                const SpanStyle style = hashes == 1 ? SpanStyle::Heading1 : hashes == 2 ? SpanStyle::Heading2 : SpanStyle::Heading3;
                out.spans.push_back({out.text.size(), title.size(), style});
                out.text.append(title);
            } else {
                size_t indent = 0;
                while (indent < line.size() && line[indent] == L' ') ++indent;
                std::wstring_view rest = line.substr(indent);
                out.text.append(indent, L' ');
                if (rest.substr(0, 6) == L"- [ ] " || rest.substr(0, 6) == L"* [ ] ") { out.text += L"☐ "; rest.remove_prefix(6); }
                else if (rest.substr(0, 6) == L"- [x] " || rest.substr(0, 6) == L"- [X] ") { out.text += L"☑ "; rest.remove_prefix(6); }
                else if (rest.substr(0, 2) == L"- " || rest.substr(0, 2) == L"* ") { out.text += L"• "; rest.remove_prefix(2); }
                detail::ScanInline(rest, out);
            }
        }
        if (!last) out.text += L'\n';
        if (last) break;
    }
    return out;
}

// The panel lays out only about the first 8 KB of a body, so building and
// drawing its text layout stays well under a millisecond.
inline constexpr size_t kLayoutChars = 8192;

// Where to cut `text` for layout: just after the last line break at or before
// `max`, or at `max` itself when there's no break in its second half (one long
// minified line). Never splits a surrogate pair.
inline size_t CutAtLineBreak(std::wstring_view text, size_t max = kLayoutChars) {
    if (text.size() <= max || max == 0) return (std::min)(text.size(), max);
    const size_t nl = text.rfind(L'\n', max - 1);
    if (nl != std::wstring_view::npos && nl >= max / 2) return nl + 1;
    size_t cut = max;
    if (IS_HIGH_SURROGATE(text[cut - 1])) --cut;
    return cut;
}

// The text and spans the panel lays out for a body: markdown-scanned for
// notes, plain otherwise, cut by CutAtLineBreak, and ending in a Muted
// "Preview shows the start of the file" when the read stopped at 64 KB
// (`truncated`) or the layout cut dropped text.
inline ScannedText BodyForLayout(std::wstring_view body, bool markdown, bool truncated, size_t max = kLayoutChars) {
    const size_t cut = CutAtLineBreak(body, max);
    ScannedText out = markdown ? ScanMarkdown(body.substr(0, cut)) : ScannedText{std::wstring(body.substr(0, cut)), {}};
    if (!truncated && cut >= body.size()) return out;
    while (!out.text.empty() && (out.text.back() == L'\n' || out.text.back() == L'\r')) out.text.pop_back();
    for (auto& span : out.spans) {
        span.length = span.start >= out.text.size() ? 0 : (std::min)(span.length, out.text.size() - span.start);
    }
    // One truthful notice for either cut: only this text is ever shown.
    const std::wstring_view notice = L"Preview shows the start of the file";
    if (!out.text.empty()) out.text += L"\n\n";
    out.spans.push_back({out.text.size(), notice.size(), SpanStyle::Muted});
    out.text.append(notice);
    return out;
}

struct Frontmatter {
    std::wstring propertyLine;  // "tag1 · tag2 · created 2026-09-24 09:01", or empty
    std::wstring body;
};

// YAML frontmatter between a first line "---" and the next "---" becomes one
// compact line of tags and the created time; everything else in it is left
// out. An unclosed block is not frontmatter.
inline Frontmatter SplitFrontmatter(std::wstring_view note) {
    Frontmatter out;
    const auto startsWith = [](std::wstring_view s, std::wstring_view p) { return s.substr(0, p.size()) == p; };
    if (!startsWith(note, L"---\n") && !startsWith(note, L"---\r\n")) {
        out.body.assign(note);
        return out;
    }
    const size_t open = note.find(L'\n') + 1;
    size_t close = std::wstring_view::npos;
    for (size_t p = open; p < note.size();) {
        size_t nl = note.find(L'\n', p);
        if (nl == std::wstring_view::npos) nl = note.size();
        if (detail::TrimView(note.substr(p, nl - p)) == L"---") { close = p; break; }
        p = nl + 1;
    }
    if (close == std::wstring_view::npos) {
        out.body.assign(note);
        return out;
    }
    std::vector<std::wstring> tags;
    std::wstring created;
    bool inTags = false;
    for (size_t p = open; p < close;) {
        size_t nl = note.find(L'\n', p);
        if (nl == std::wstring_view::npos || nl > close) nl = close;
        std::wstring_view line = note.substr(p, nl - p);
        if (!line.empty() && line.back() == L'\r') line.remove_suffix(1);
        p = nl + 1;
        const std::wstring_view trimmed = detail::TrimView(line);
        if (inTags && startsWith(trimmed, L"- ")) { tags.emplace_back(detail::TrimView(trimmed.substr(2))); continue; }
        inTags = false;
        if (startsWith(trimmed, L"tags:")) {
            std::wstring_view value = detail::TrimView(trimmed.substr(5));
            if (value.empty()) { inTags = true; continue; }
            if (value.front() == L'[' && value.back() == L']') value = value.substr(1, value.size() - 2);
            size_t s = 0;
            while (s <= value.size()) {
                size_t comma = value.find(L',', s);
                if (comma == std::wstring_view::npos) comma = value.size();
                const std::wstring_view tag = detail::TrimView(value.substr(s, comma - s));
                if (!tag.empty()) tags.emplace_back(tag);
                s = comma + 1;
            }
        } else if (startsWith(trimmed, L"created:")) {
            created.assign(detail::TrimView(trimmed.substr(8)));
            std::replace(created.begin(), created.end(), L'T', L' ');
        }
    }
    for (const auto& tag : tags) {
        if (!out.propertyLine.empty()) out.propertyLine += L" · ";
        out.propertyLine += tag;
    }
    if (!created.empty()) {
        if (!out.propertyLine.empty()) out.propertyLine += L" · ";
        out.propertyLine += L"created " + created;
    }
    size_t bodyStart = note.find(L'\n', close);
    out.body.assign(bodyStart == std::wstring_view::npos ? std::wstring_view{} : note.substr(bodyStart + 1));
    return out;
}

enum class PreviewKind { Note, Text, Folder, Image, App, None };

namespace detail {
inline bool HasExtension(std::wstring_view path, std::initializer_list<const wchar_t*> extensions) {
    const size_t dot = path.rfind(L'.');
    const size_t slash = path.find_last_of(L"\\/");
    if (dot == std::wstring_view::npos || (slash != std::wstring_view::npos && dot < slash)) return false;
    const std::wstring_view ext = path.substr(dot);
    for (const wchar_t* candidate : extensions) {
        if (ext.size() == wcslen(candidate) && _wcsnicmp(ext.data(), candidate, ext.size()) == 0) return true;
    }
    return false;
}
} // namespace detail

inline PreviewKind ClassifyPreview(std::wstring_view path, bool isDirectory, bool isNote, bool isApp) {
    // Any .md file gets the light markdown, not only vault notes.
    if (isNote || detail::HasExtension(path, {L".md"})) return PreviewKind::Note;
    if (isApp) return PreviewKind::App;
    if (isDirectory) return PreviewKind::Folder;
    if (detail::HasExtension(path, {L".png", L".jpg", L".jpeg", L".gif", L".bmp", L".webp"})) return PreviewKind::Image;
    if (detail::HasExtension(path, {L".txt", L".json", L".log", L".csv", L".ini", L".ps1", L".py", L".js", L".ts",
            L".cpp", L".h", L".hpp", L".c", L".cs", L".xml", L".yml", L".yaml", L".toml", L".bat", L".cmd", L".sh",
            L".html", L".css", L".sql", L".go", L".rs", L".java", L".base", L".canvas"})) {
        return PreviewKind::Text;
    }
    return PreviewKind::None;
}

inline bool IsCloudPlaceholder(DWORD attributes) {
    return (attributes & (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_OFFLINE)) != 0;
}

namespace detail {
// ReadPreviewBytes' 64 KB cap can cut a UTF-8 buffer off mid-character. If the
// last 1-3 bytes end in a lead byte whose declared sequence length runs past
// the end of the buffer, drop that incomplete trailing sequence so the valid
// prefix still decodes as UTF-8 instead of the whole buffer falling back to
// cp1252 mojibake.
inline std::string_view TrimIncompleteUtf8Tail(std::string_view text) {
    const size_t window = (std::min)(text.size(), static_cast<size_t>(3));
    for (size_t i = 1; i <= window; ++i) {
        const size_t pos = text.size() - i;
        const unsigned char lead = static_cast<unsigned char>(text[pos]);
        if ((lead & 0xC0) == 0x80) continue;  // continuation byte: keep walking back to find the lead
        size_t declared = 0;
        if ((lead & 0xE0) == 0xC0) declared = 2;
        else if ((lead & 0xF0) == 0xE0) declared = 3;
        else if ((lead & 0xF8) == 0xF0) declared = 4;
        else break;  // ASCII or not a valid lead byte: nothing incomplete to trim here
        const size_t present = text.size() - pos;
        if (present < declared) return text.substr(0, pos);
        break;
    }
    return text;
}
} // namespace detail

// `truncated` should be ReadPreviewBytes' ReadResult::truncated: only a buffer
// actually cut at the 64 KB cap gets its incomplete trailing sequence trimmed
// before the strict UTF-8 attempt; a short, untruncated file that happens to
// end in a lead byte (real cp1252, e.g. "café") must still fall back whole.
inline std::wstring DecodeText(const std::string& bytes, bool truncated = false) {
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE) {
        std::wstring out((bytes.size() - 2) / 2, L'\0');
        memcpy(out.data(), bytes.data() + 2, out.size() * sizeof(wchar_t));
        return out;
    }
    std::string_view utf8(bytes);
    if (utf8.size() >= 3 && utf8.substr(0, 3) == "\xEF\xBB\xBF") utf8.remove_prefix(3);
    if (utf8.empty()) return {};

    const std::string_view trimmed = truncated ? detail::TrimIncompleteUtf8Tail(utf8) : utf8;
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, trimmed.data(), static_cast<int>(trimmed.size()), nullptr, 0);
    if (len > 0 || trimmed.empty()) {
        std::wstring out(static_cast<size_t>((std::max)(len, 0)), L'\0');
        if (len > 0) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, trimmed.data(), static_cast<int>(trimmed.size()), out.data(), len);
        return out;
    }
    // Not UTF-8, even after trimming a possible mid-character truncation: read as Windows Western.
    len = MultiByteToWideChar(1252, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>((std::max)(len, 0)), L'\0');
    if (len > 0) MultiByteToWideChar(1252, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), len);
    return out;
}

inline bool LooksBinary(const std::string& bytes) {
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE) return false;
    return bytes.find('\0') != std::string::npos;
}

inline constexpr size_t kMaxPreviewBytes = 64 * 1024;

struct ReadResult {
    std::string bytes;
    bool truncated = false;
    DWORD error = 0;
    bool placeholder = false;
};

// Reads at most `cap` bytes without blocking other apps' writes, renames or
// deletes. Online-only cloud files are never opened (that would download them).
inline ReadResult ReadPreviewBytes(const std::wstring& path, size_t cap = kMaxPreviewBytes) {
    ReadResult result;
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) {
        result.error = GetLastError();
        return result;
    }
    if (IsCloudPlaceholder(info.dwFileAttributes)) {
        result.placeholder = true;
        return result;
    }
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_NO_RECALL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        result.error = GetLastError();
        return result;
    }
    const unsigned long long size = (static_cast<unsigned long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    result.truncated = size > cap;
    result.bytes.resize(static_cast<size_t>((std::min)(size, static_cast<unsigned long long>(cap))));
    DWORD got = 0;
    if (!result.bytes.empty() && !ReadFile(file, result.bytes.data(), static_cast<DWORD>(result.bytes.size()), &got, nullptr)) {
        result.error = GetLastError();
    }
    result.bytes.resize(got);
    CloseHandle(file);
    return result;
}

struct TextPreview {
    std::wstring propertyLine;
    std::wstring body;
    std::wstring status;  // non-empty: shown instead of the body
    bool truncated = false;
};

// What a note or text file previews as, built from ReadPreviewBytes' result
// on the preview thread. Never a blank panel: a note that's only frontmatter
// keeps its property line, and an empty file says so. `todayNote`: a capture
// row's target is today's daily note, which the capture creates on first use
// (the preview never does), so a missing one isn't an error.
inline TextPreview BuildTextPreview(const ReadResult& read, bool isNote, bool todayNote = false) {
    TextPreview out;
    if (read.placeholder) {
        out.status = L"Not downloaded - open to download";
        return out;
    }
    if (read.error != 0 && read.bytes.empty()) {
        const bool missing = read.error == ERROR_FILE_NOT_FOUND || read.error == ERROR_PATH_NOT_FOUND;
        out.status = missing && todayNote ? L"Today's note doesn't exist yet"
                   : missing ? L"File not found"
                   : read.error == ERROR_ACCESS_DENIED ? L"Access denied"
                   : L"Can't read this file";
        return out;
    }
    if (LooksBinary(read.bytes)) {
        out.status = L"Binary file - no preview";
        return out;
    }
    out.truncated = read.truncated;
    std::wstring text = DecodeText(read.bytes, read.truncated);
    if (isNote) {
        Frontmatter split = SplitFrontmatter(text);
        out.propertyLine = std::move(split.propertyLine);
        out.body = std::move(split.body);
    } else {
        out.body = std::move(text);
    }
    if (out.body.empty() && out.propertyLine.empty()) out.status = L"Empty file";
    return out;
}

inline constexpr float kPanelWidth = 420.0f;
inline constexpr float kScreenMargin = 16.0f;

struct PanelPlacement {
    bool overlay = false;
    float windowWidthDip = 0;
    float windowLeftDip = 0;
};

// The panel is 420 DIP wide. The window grows from `launcherWidthDip` to
// launcher+panel and moves left only as far as needed to stay 16 DIP inside
// the work area. If the work area is too narrow for launcher+panel+2*margin,
// the panel is shown as an overlay over the results instead, at the
// launcher's normal (centered) width and position.
inline PanelPlacement PanelGeometry(float workLeftDip, float workWidthDip, float launcherWidthDip, float centeredLeftDip) {
    PanelPlacement place;
    const float wide = launcherWidthDip + kPanelWidth;
    if (workWidthDip < wide + 2 * kScreenMargin) {
        place.overlay = true;
        place.windowWidthDip = launcherWidthDip;
        place.windowLeftDip = centeredLeftDip;
        return place;
    }
    place.windowWidthDip = wide;
    const float maxLeft = workLeftDip + workWidthDip - kScreenMargin - wide;
    place.windowLeftDip = (std::min)(centeredLeftDip, maxLeft);
    place.windowLeftDip = (std::max)(place.windowLeftDip, workLeftDip + kScreenMargin);
    return place;
}

// Every selection change takes a new number; a result is shown only if its
// number is still the latest. Invalidate() drops everything in flight (e.g.
// hiding the launcher while a preview load is still running).
class PreviewGate {
public:
    unsigned Next() { return ++current_; }
    bool IsCurrent(unsigned generation) const { return generation == current_ && generation != 0; }
    void Invalidate() { ++current_; }
private:
    unsigned current_ = 0;
};

// Folders first, then files, alphabetically (case-insensitive); hidden/system
// entries are left out. At most `shown` lines are listed, with the rest
// counted as "and N more" ("and N+ more" when the listing stopped at the
// entry cap, so the real count is higher).
inline std::wstring FolderSummary(const std::vector<typed::PathEntry>& entries, size_t shown = 20, bool truncated = false) {
    std::vector<const typed::PathEntry*> visible;
    for (const auto& e : entries) if (!e.hidden) visible.push_back(&e);
    std::stable_sort(visible.begin(), visible.end(), [](const typed::PathEntry* a, const typed::PathEntry* b) {
        if (a->isDirectory != b->isDirectory) return a->isDirectory;
        return _wcsicmp(a->name.c_str(), b->name.c_str()) < 0;
    });
    std::wstring out;
    for (size_t i = 0; i < visible.size() && i < shown; ++i) {
        out += (visible[i]->isDirectory ? L"\U0001F4C1 " : L"    ") + visible[i]->name + L"\n";
    }
    if (visible.size() > shown || truncated) {
        out += L"and " + std::to_wstring(visible.size() > shown ? visible.size() - shown : 0) +
               (truncated ? L"+ more" : L" more");
    } else if (visible.empty()) {
        out = L"Empty folder";
    }
    return out;
}

// What identifies a preview, so a results rebuild for the same row keeps what's
// shown (and its scroll). Capture rows (keyedByName false) preview their target
// note: their name carries the typed text, so it's left out, or every keystroke
// would reload the note. The vault is in: the same ref is another file there.
inline std::wstring PreviewKey(int category, std::wstring_view path, std::wstring_view name,
    std::wstring_view vault, bool keyedByName) {
    std::wstring key = std::to_wstring(category) + L"|";
    key.append(path);
    key += L"|";
    if (keyedByName) key.append(name);
    key += L"|";
    key.append(vault);
    return key;
}

// An absolute note path (a capture target, already resolved) is inside the
// vault: under its folder, case-insensitively, with a remainder that passes the
// vault-relative safety check (no "..", no ':' streams, no device names).
inline bool IsNoteInsideVault(std::wstring path, std::wstring vault) {
    if (path.empty() || vault.empty()) return false;
    std::replace(path.begin(), path.end(), L'/', L'\\');
    std::replace(vault.begin(), vault.end(), L'/', L'\\');
    while (vault.size() > 1 && vault.back() == L'\\') vault.pop_back();
    if (path.size() <= vault.size() + 1 || path[vault.size()] != L'\\' ||
        _wcsnicmp(path.c_str(), vault.c_str(), vault.size()) != 0) {
        return false;
    }
    const std::wstring rest = path.substr(vault.size() + 1);
    return rest.back() != L'\\' && !obsidian::IsUnsafeVaultRelativePath(rest);
}

// US-046: the size an image of `width` x `height` gets inside a
// `boxWidth` x `boxHeight` box - aspect ratio kept, never enlarged.
struct FitSize {
    float width = 0, height = 0;
};

inline FitSize FitImage(float width, float height, float boxWidth, float boxHeight) {
    if (width <= 0 || height <= 0 || boxWidth <= 0 || boxHeight <= 0) return {};
    const float scale = (std::min)({1.0f, boxWidth / width, boxHeight / height});
    return {width * scale, height * scale};
}

// An image's width and height from the start of its file (at most the
// 64 KB ReadPreviewBytes reads): PNG IHDR, GIF screen size, BMP info
// header, JPEG SOFn, WebP VP8/VP8L/VP8X. Every read is bounds-checked;
// anything unknown, truncated or zero-sized gives nullopt. No codec runs.
inline std::optional<std::pair<uint32_t, uint32_t>> ImageDimensions(std::string_view b) {
    using Dims = std::optional<std::pair<uint32_t, uint32_t>>;
    const auto at = [&](size_t i) -> uint32_t { return static_cast<unsigned char>(b[i]); };
    const auto be16 = [&](size_t i) { return at(i) << 8 | at(i + 1); };
    const auto le16 = [&](size_t i) { return at(i) | at(i + 1) << 8; };
    const auto le24 = [&](size_t i) { return le16(i) | at(i + 2) << 16; };
    const auto le32 = [&](size_t i) { return le16(i) | le16(i + 2) << 16; };
    const auto dims = [](uint32_t w, uint32_t h) -> Dims {
        if (!w || !h) return std::nullopt;
        return std::pair{w, h};
    };
    const auto starts = [&](size_t i, std::string_view tag) { return b.size() >= i + tag.size() && b.substr(i, tag.size()) == tag; };
    if (starts(0, std::string_view("\x89PNG\r\n\x1a\n", 8))) {
        if (b.size() < 24 || !starts(12, "IHDR")) return std::nullopt;
        return dims(be16(16) << 16 | be16(18), be16(20) << 16 | be16(22));
    }
    if (starts(0, "GIF87a") || starts(0, "GIF89a")) {
        if (b.size() < 10) return std::nullopt;
        return dims(le16(6), le16(8));
    }
    if (starts(0, "BM")) {
        if (b.size() < 26) return std::nullopt;
        if (le32(14) == 12) return dims(le16(18), le16(20));  // BITMAPCOREHEADER
        const auto height = static_cast<int32_t>(le32(22));   // negative: top-down
        if (static_cast<int32_t>(le32(18)) < 0) return std::nullopt;
        return dims(le32(18), height < 0 ? 0u - static_cast<uint32_t>(height) : static_cast<uint32_t>(height));
    }
    if (starts(0, "RIFF") && starts(8, "WEBP")) {
        if (starts(12, "VP8 ") && b.size() >= 30 && starts(23, "\x9d\x01\x2a")) return dims(le16(26) & 0x3FFF, le16(28) & 0x3FFF);
        if (starts(12, "VP8L") && b.size() >= 25 && at(20) == 0x2F) {
            const uint32_t bits = le32(21);
            return dims((bits & 0x3FFF) + 1, (bits >> 14 & 0x3FFF) + 1);
        }
        if (starts(12, "VP8X") && b.size() >= 30) return dims(le24(24) + 1, le24(27) + 1);
        return std::nullopt;
    }
    if (starts(0, "\xFF\xD8")) {
        size_t i = 2;
        while (i < b.size()) {
            if (at(i) != 0xFF) return std::nullopt;
            while (i < b.size() && at(i) == 0xFF) ++i;  // fill bytes
            if (i >= b.size()) break;
            const uint32_t marker = at(i++);
            if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) continue;  // no length
            if (marker == 0xD9 || marker == 0xDA || i + 2 > b.size()) return std::nullopt;
            const size_t length = be16(i);
            if (length < 2) return std::nullopt;
            if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
                if (length < 7 || i + 7 > b.size()) return std::nullopt;
                return dims(be16(i + 5), be16(i + 3));  // precision, height, width
            }
            i += length;
        }
    }
    return std::nullopt;
}

// "532 bytes", "12.3 KB", "4.5 MB", "1.2 GB" (1 KB = 1024 bytes).
inline std::wstring FormatFileSize(unsigned long long bytes) {
    if (bytes < 1024) return std::to_wstring(bytes) + (bytes == 1 ? L" byte" : L" bytes");
    const wchar_t* units[] = {L"KB", L"MB", L"GB", L"TB"};
    double value = static_cast<double>(bytes) / 1024;
    size_t unit = 0;
    while (value >= 1024 && unit + 1 < std::size(units)) { value /= 1024; ++unit; }
    wchar_t text[32];
    swprintf_s(text, value < 10 ? L"%.1f %s" : L"%.0f %s", value, units[unit]);
    return text;
}

// The line under a thumbnail: "name · W×H · size". The dimensions are left
// out when they aren't known (no codec for the format).
inline std::wstring ImageCaption(std::wstring_view path, unsigned width, unsigned height, unsigned long long bytes) {
    const size_t slash = path.find_last_of(L"\\/");
    std::wstring caption(slash == std::wstring_view::npos ? path : path.substr(slash + 1));
    if (width && height) caption += L" · " + std::to_wstring(width) + L"×" + std::to_wstring(height);
    return caption + L" · " + FormatFileSize(bytes);
}

} // namespace preview
} // namespace leanlauncher
