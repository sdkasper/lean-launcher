#pragma once

#include <algorithm>
#include <cstddef>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

// Typed URLs (US-042): recognising what the user typed, kept free of window
// state so it can be unit-tested. Only http and https are ever opened - a
// typed "javascript:", "file:" or "ms-settings:" never gets a URL row, so
// typed text can't launch an arbitrary protocol handler.
namespace leanlauncher {
namespace typed {

enum class UrlKind {
    None,
    Explicit,   // http://, https:// or www. - the top row
    BareDomain  // host.tld[/path] - a row below the top match (readme.md is also a "domain")
};

namespace detail {

inline bool StartsWithNoCase(std::wstring_view text, std::wstring_view prefix) {
    if (text.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (towlower(text[i]) != prefix[i]) return false;
    }
    return true;
}

// The host part: everything up to the first '/', '?' or '#'.
inline std::wstring_view HostOf(std::wstring_view rest) {
    const size_t end = rest.find_first_of(L"/?#");
    return end == std::wstring_view::npos ? rest : rest.substr(0, end);
}

// "example.com", "sub.example.co.uk": 2+ labels of letters, digits and inner
// hyphens, ending in a letters-only label of 2+ characters.
inline bool IsDomainName(std::wstring_view host) {
    if (host.empty() || host.front() == L'.' || host.back() == L'.') return false;
    size_t labels = 0;
    size_t start = 0;
    while (start <= host.size()) {
        size_t dot = host.find(L'.', start);
        if (dot == std::wstring_view::npos) dot = host.size();
        const std::wstring_view label = host.substr(start, dot - start);
        if (label.empty() || label.front() == L'-' || label.back() == L'-') return false;
        for (wchar_t ch : label) {
            if (!iswalnum(ch) && ch != L'-') return false;
        }
        ++labels;
        if (dot == host.size()) {
            if (label.size() < 2) return false;
            for (wchar_t ch : label) {
                if (!iswalpha(ch)) return false;
            }
            break;
        }
        start = dot + 1;
    }
    return labels >= 2;
}

} // namespace detail

inline UrlKind ClassifyUrl(std::wstring_view text) {
    if (text.empty()) return UrlKind::None;
    for (wchar_t ch : text) {
        if (iswspace(ch) || ch == L'\\') return UrlKind::None;
    }
    for (std::wstring_view scheme : {std::wstring_view(L"https://"), std::wstring_view(L"http://")}) {
        if (detail::StartsWithNoCase(text, scheme)) {
            std::wstring_view host = detail::HostOf(text.substr(scheme.size()));
            const size_t at = host.rfind(L'@');
            if (at != std::wstring_view::npos) host = host.substr(at + 1);
            return host.empty() ? UrlKind::None : UrlKind::Explicit;
        }
    }
    const std::wstring_view host = detail::HostOf(text);
    if (host.find(L':') != std::wstring_view::npos || host.find(L'@') != std::wstring_view::npos) {
        return UrlKind::None;  // another scheme, a drive letter, a port, or an email address
    }
    if (!detail::IsDomainName(host)) return UrlKind::None;
    return detail::StartsWithNoCase(text, L"www.") ? UrlKind::Explicit : UrlKind::BareDomain;
}

// What actually opens: bare domains and www. addresses get https://.
inline std::wstring NormalizeUrl(std::wstring_view text) {
    if (detail::StartsWithNoCase(text, L"https://") || detail::StartsWithNoCase(text, L"http://")) {
        return std::wstring(text);
    }
    return L"https://" + std::wstring(text);
}

// Result position for the "Open URL" row: first for an explicit URL; for a
// bare domain, below the top match so e.g. the file readme.md stays first.
inline size_t UrlRowPosition(UrlKind kind, size_t resultCount) {
    if (kind == UrlKind::Explicit) return 0;
    if (kind == UrlKind::BareDomain) return resultCount > 0 ? 1 : 0;
    return resultCount;
}

// ---------------------------------------------------------------------------
// Path completion (US-043). The launcher does the Win32 parts (%VAR%
// expansion, folder listing on a background thread); these helpers decide
// what counts as a path, what to list, and how Tab completes.
// ---------------------------------------------------------------------------

inline constexpr size_t kMaxPathEntries = 2000;

// Path mode starts only on these shapes, with no disk access:
// C:\ or C:/, \\server..., %VAR%\ or %VAR%/, ~\ or ~/.
inline bool LooksLikePath(std::wstring_view text) {
    const auto isSep = [](wchar_t ch) { return ch == L'\\' || ch == L'/'; };
    if (text.size() >= 3 && iswalpha(text[0]) && text[1] == L':' && isSep(text[2])) return true;
    if (text.size() > 2 && text[0] == L'\\' && text[1] == L'\\') return true;
    if (text.size() >= 2 && text[0] == L'~' && isSep(text[1])) return true;
    if (!text.empty() && text[0] == L'%') {
        const size_t close = text.find(L'%', 1);
        return close != std::wstring_view::npos && close >= 2 && close + 1 < text.size() && isSep(text[close + 1]);
    }
    return false;
}

// "~\x" or "~/x" -> "<profile>\x"; forward slashes become backslashes.
// (%VAR% expansion is done by the launcher with ExpandEnvironmentStringsW.)
inline std::wstring ExpandHome(std::wstring_view text, std::wstring_view profile) {
    std::wstring out;
    if (text.size() >= 2 && text[0] == L'~' && (text[1] == L'\\' || text[1] == L'/')) {
        out.assign(profile);
        out += L'\\';
        out.append(text.substr(2));
    } else {
        out.assign(text);
    }
    for (auto& ch : out) {
        if (ch == L'/') ch = L'\\';
    }
    return out;
}

enum class PathKind {
    None,
    Local,      // a drive path (mapped network drives are detected by the launcher)
    Network,    // \\server\share\...
    NeedsShare  // \\server or \\server\sha - listing a server's shares is out of scope
};

struct PathQuery {
    PathKind kind = PathKind::None;
    std::wstring folder;   // with trailing backslash
    std::wstring partial;  // the name being typed
};

// Splits an expanded, backslash-only path into the folder to list and the
// partial name after its last backslash.
inline PathQuery SplitPathQuery(std::wstring_view path) {
    PathQuery query;
    const size_t sep = path.rfind(L'\\');
    if (sep == std::wstring_view::npos) return query;
    if (path.size() > 2 && path[0] == L'\\' && path[1] == L'\\') {
        const size_t serverEnd = path.find(L'\\', 2);
        const size_t shareEnd = serverEnd == std::wstring_view::npos ? std::wstring_view::npos
                                                                    : path.find(L'\\', serverEnd + 1);
        if (serverEnd == std::wstring_view::npos || serverEnd == 2 || shareEnd == std::wstring_view::npos ||
            shareEnd == serverEnd + 1) {
            query.kind = PathKind::NeedsShare;
            return query;
        }
        query.kind = PathKind::Network;
    } else {
        query.kind = PathKind::Local;
    }
    query.folder.assign(path.substr(0, sep + 1));
    query.partial.assign(path.substr(sep + 1));
    return query;
}

struct PathEntry {
    std::wstring name;
    bool isDirectory = false;
    bool hidden = false;  // hidden or system attribute
};

// Entries matching `partial`, case-insensitive: prefix matches first, then
// (from 2 typed characters) "contains" matches; folders before files; then
// by name. Hidden and system items only when `partial` starts with ".".
inline std::vector<PathEntry> FilterPathEntries(const std::vector<PathEntry>& entries, std::wstring_view partial) {
    const auto lower = [](std::wstring_view s) {
        std::wstring out(s);
        for (auto& ch : out) ch = static_cast<wchar_t>(towlower(ch));
        return out;
    };
    const std::wstring needle = lower(partial);
    const bool showHidden = !needle.empty() && needle[0] == L'.';
    struct Ranked { int group; const PathEntry* entry; std::wstring key; };
    std::vector<Ranked> ranked;
    for (const auto& entry : entries) {
        if (entry.hidden && !showHidden) continue;
        std::wstring key = lower(entry.name);
        int group = -1;
        if (needle.empty() || key.compare(0, needle.size(), needle) == 0) group = 0;
        else if (needle.size() >= 2 && key.find(needle) != std::wstring::npos) group = 1;
        if (group < 0) continue;
        ranked.push_back({group, &entry, std::move(key)});
    }
    std::sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
        if (a.group != b.group) return a.group < b.group;
        if (a.entry->isDirectory != b.entry->isDirectory) return a.entry->isDirectory;
        return a.key < b.key;
    });
    std::vector<PathEntry> out;
    out.reserve(ranked.size());
    for (const auto& r : ranked) out.push_back(*r.entry);
    return out;
}

// Tab: replace the partial name with the chosen entry, keeping whatever was
// typed before it (%APPDATA%, ~/, forward slashes). Folders get a "\".
inline std::wstring CompleteTypedPath(std::wstring_view typed, std::wstring_view name, bool isDirectory) {
    const size_t sep = typed.find_last_of(L"\\/");
    std::wstring out(sep == std::wstring_view::npos ? std::wstring_view{} : typed.substr(0, sep + 1));
    out.append(name);
    if (isDirectory) out += L'\\';
    return out;
}

} // namespace typed
} // namespace leanlauncher
