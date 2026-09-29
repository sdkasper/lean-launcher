#pragma once

#include "obsidian_config.h"
#include "search.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <cwctype>
#include <string>
#include <system_error>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Snippets (US-050): a text expander. The pure logic lives here, free of
// window state and hooks, so it can be unit-tested; expander.h owns the
// keyboard hook and the launcher owns the settings and result rows.
namespace leanlauncher {
namespace snippets {

inline constexpr size_t kMinTrigger = 2;
inline constexpr size_t kMaxTrigger = 32;
inline constexpr size_t kMaxFileBytes = 1024 * 1024;
inline constexpr size_t kMaxSnippets = 5000;
inline constexpr size_t kMaxReplaceChars = 65536;
inline constexpr wchar_t kPathPrefix[] = L"leanlauncher:snippet:";

struct Snippet {
    std::wstring trigger;
    std::wstring replace;
    std::wstring label;
    // Only filled by ParseSnippets(yaml, keepComments = true), for the Espanso
    // import: the comment lines above the item ("# SECTION" at column 0, other
    // comments indented by two spaces), joined by '\n'. The running index never
    // carries them.
    std::string comments;
    // Import only: the Espanso file this snippet came from.
    std::wstring source;
};

struct ParseResult {
    std::vector<Snippet> snippets;
    std::vector<std::wstring> warnings;
    std::string trailingComments;  // comment lines after the last item (keepComments only)
};

// Why a trigger cannot be used, or nullptr when it is fine.
inline const wchar_t* TriggerProblem(std::wstring_view trigger) {
    if (trigger.size() < kMinTrigger) return L"trigger must be at least 2 characters";
    if (trigger.size() > kMaxTrigger) return L"trigger must be at most 32 characters";
    for (wchar_t ch : trigger) {
        if (ch <= L' ' || ch == 0x7F || (ch >= 0xD800 && ch <= 0xDFFF) || iswspace(ch)) {
            return L"trigger must not contain spaces, control or non-BMP characters";
        }
    }
    return nullptr;
}

namespace detail {

inline std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// Leading spaces only: a tab is not YAML indentation, so a tab-indented
// file simply yields no items.
inline size_t IndentOf(std::string_view line) {
    size_t n = 0;
    while (n < line.size() && line[n] == ' ') ++n;
    return n;
}

inline void AppendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// After a closing quote only spaces and a comment may follow.
inline bool TrailingIsEmpty(std::string_view rest) {
    rest = Trim(rest);
    return rest.empty() || rest.front() == '#';
}

// One inline value: double-quoted (\n \t \r \\ \" \/ \uXXXX), single-quoted
// ('' is a quote) or plain (a trailing " #comment" is dropped). Returns false
// for a malformed value (unterminated quote, unknown escape).
inline bool ParseScalar(std::string_view raw, std::string& out) {
    raw = Trim(raw);
    out.clear();
    if (raw.empty() || raw.front() == '#') return true;
    if (raw.front() == '"') {
        size_t i = 1;
        for (; i < raw.size(); ++i) {
            const char c = raw[i];
            if (c == '"') break;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (++i >= raw.size()) return false;
            switch (raw[i]) {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case '\\': out.push_back('\\'); break;
            case '"': out.push_back('"'); break;
            case '/': out.push_back('/'); break;
            case 'u': {
                if (i + 4 >= raw.size()) return false;
                unsigned cp = 0;
                for (int k = 1; k <= 4; ++k) {
                    const char h = raw[i + k];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                    else return false;
                }
                if (cp >= 0xD800 && cp <= 0xDFFF) return false;
                i += 4;
                AppendUtf8(out, cp);
                break;
            }
            default: return false;
            }
        }
        if (i >= raw.size()) return false;  // unterminated
        return TrailingIsEmpty(raw.substr(i + 1));
    }
    if (raw.front() == '\'') {
        size_t i = 1;
        for (; i < raw.size(); ++i) {
            if (raw[i] == '\'') {
                if (i + 1 < raw.size() && raw[i + 1] == '\'') {
                    out.push_back('\'');
                    ++i;
                    continue;
                }
                break;
            }
            out.push_back(raw[i]);
        }
        if (i >= raw.size()) return false;
        return TrailingIsEmpty(raw.substr(i + 1));
    }
    const size_t hash = raw.find(" #");
    if (hash != std::string_view::npos) raw = Trim(raw.substr(0, hash));
    out.assign(raw);
    return true;
}

// A block scalar ("|", "|-", ">", ">-") whose body is the following lines
// indented deeper than `parentIndent`. Advances `i` to the last line consumed.
inline bool ReadBlock(const std::vector<std::string_view>& lines, size_t& i, size_t parentIndent,
                      std::string_view header, std::string& out) {
    out.clear();
    const bool folded = header.front() == '>';
    const bool strip = header.size() > 1 && header[1] == '-';
    std::vector<std::string_view> body;
    size_t j = i + 1;
    for (; j < lines.size(); ++j) {
        const bool blank = Trim(lines[j]).empty();
        if (!blank && IndentOf(lines[j]) <= parentIndent) break;
        body.push_back(lines[j]);
    }
    i = j - 1;
    while (!body.empty() && Trim(body.back()).empty()) body.pop_back();
    if (body.empty()) return true;
    size_t blockIndent = 0;
    for (const auto l : body) {
        if (!Trim(l).empty()) {
            blockIndent = IndentOf(l);
            break;
        }
    }
    for (size_t k = 0; k < body.size(); ++k) {
        std::string_view text;
        if (!Trim(body[k]).empty()) {
            if (IndentOf(body[k]) < blockIndent) return false;
            text = body[k].substr(blockIndent);
            while (!text.empty() && text.back() == '\r') text.remove_suffix(1);
        }
        if (folded) {
            if (text.empty()) {
                out.push_back('\n');
            } else {
                if (!out.empty() && out.back() != '\n') out.push_back(' ');
                out.append(text);
            }
        } else {
            if (k > 0) out.push_back('\n');
            out.append(text);
        }
    }
    if (!strip) out.push_back('\n');
    return true;
}

inline void JoinLines(std::string& dst, std::string_view more) {
    if (more.empty()) return;
    if (!dst.empty()) dst.push_back('\n');
    dst.append(more);
}

// A comment line, kept verbatim but re-indented to column 0 or two spaces.
inline void AddCommentLine(std::string& dst, std::string_view line) {
    std::string text = IndentOf(line) == 0 ? "" : "  ";
    text.append(Trim(line));
    JoinLines(dst, text);
}

// Only the column-0 lines: the section headers that outlive a skipped item.
inline std::string SectionLines(std::string_view comments) {
    std::string out;
    for (size_t pos = 0; pos <= comments.size();) {
        size_t end = comments.find('\n', pos);
        if (end == std::string_view::npos) end = comments.size();
        const std::string_view line = comments.substr(pos, end - pos);
        if (!line.empty() && line.front() == '#') JoinLines(out, line);
        pos = end + 1;
    }
    return out;
}

struct Item {
    std::string comments;
    size_t line = 0;
    size_t dashIndent = 0;
    size_t keyIndent = 0;  // 0 until the first key line fixes it
    std::string trigger, replace, label;
    bool hasTrigger = false, hasReplace = false;
    std::wstring unsupported;  // why the whole item is skipped
};

}  // namespace detail

// Reads the Espanso-compatible YAML subset: a top-level "matches:" list whose
// items have "trigger", "replace" and "label". Any other key (vars, word,
// triggers, ...) skips that whole item with a warning, so an item is never
// half-applied. Never throws; malformed input yields warnings.
inline ParseResult ParseSnippets(std::string_view yaml, bool keepComments = false) {
    using namespace detail;
    ParseResult result;
    if (yaml.size() > kMaxFileBytes) {
        result.warnings.push_back(L"the file is larger than 1 MB; nothing was loaded");
        return result;
    }
    if (yaml.size() >= 3 && yaml.substr(0, 3) == "\xEF\xBB\xBF") yaml.remove_prefix(3);

    std::vector<std::string_view> lines;
    for (size_t pos = 0; pos <= yaml.size();) {
        size_t end = yaml.find('\n', pos);
        if (end == std::string_view::npos) end = yaml.size();
        std::string_view line = yaml.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        lines.push_back(line);
        pos = end + 1;
    }

    std::unordered_set<std::wstring> seen;
    std::string pending;  // comment lines waiting for the next item
    std::string orphan;   // section headers of skipped items, passed on to the next item
    bool capWarned = false;
    bool inMatches = false;
    bool haveItem = false;
    Item item;

    const auto finishItem = [&]() {
        if (!haveItem) return;
        haveItem = false;
        const std::wstring where = L"line " + std::to_wstring(item.line) + L": ";
        if (!item.unsupported.empty()) {
            result.warnings.push_back(where + L"skipped - " + item.unsupported);
            return;
        }
        if (!item.hasTrigger) {
            result.warnings.push_back(where + L"skipped - no trigger");
            return;
        }
        if (!item.hasReplace) {
            result.warnings.push_back(where + L"skipped - no replace text");
            return;
        }
        Snippet snippet;
        snippet.trigger = obsidian::Utf8ToWide(item.trigger);
        snippet.replace = obsidian::Utf8ToWide(item.replace);
        snippet.label = obsidian::Utf8ToWide(item.label);
        if (const wchar_t* problem = TriggerProblem(snippet.trigger)) {
            result.warnings.push_back(where + L"skipped - " + problem);
            return;
        }
        if (snippet.replace.size() > kMaxReplaceChars) {
            result.warnings.push_back(where + L"skipped - replacement is longer than 64 KB");
            return;
        }
        if (!seen.insert(snippet.trigger).second) {
            result.warnings.push_back(where + L"skipped - duplicate trigger " + snippet.trigger);
            return;
        }
        if (result.snippets.size() >= kMaxSnippets) {
            if (!capWarned) {
                result.warnings.push_back(L"only the first 5,000 snippets were loaded");
                capWarned = true;
            }
            return;
        }
        result.snippets.push_back(std::move(snippet));
    };
    const auto finish = [&]() {
        if (!haveItem) return;
        const size_t before = result.snippets.size();
        finishItem();
        if (!keepComments) return;
        if (result.snippets.size() > before) result.snippets.back().comments = item.comments;
        else JoinLines(orphan, SectionLines(item.comments));
    };

    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string_view line = lines[i];
        const std::string_view trimmed = Trim(line);
        if (trimmed.empty()) continue;
        if (trimmed.front() == '#') {
            if (keepComments) AddCommentLine(pending, line);
            continue;
        }
        const size_t indent = IndentOf(line);
        const bool isDash = trimmed.front() == '-' && (trimmed.size() == 1 || trimmed[1] == ' ');

        if (indent == 0 && !isDash) {  // a top-level key
            finish();
            inMatches = trimmed.substr(0, 8) == "matches:" && TrailingIsEmpty(trimmed.substr(8));
            if (!inMatches) pending.clear();  // comments about another top-level key
            continue;
        }
        if (!inMatches) continue;

        std::string_view content = trimmed;
        if (isDash) {
            if (haveItem && indent > item.dashIndent) continue;  // a nested list inside the item
            finish();
            item = Item{};
            if (keepComments) {
                item.comments = std::move(orphan);
                JoinLines(item.comments, pending);
                orphan.clear();
                pending.clear();
            }
            item.line = i + 1;
            item.dashIndent = indent;
            haveItem = true;
            size_t after = 1;
            while (after < trimmed.size() && trimmed[after] == ' ') ++after;
            content = trimmed.substr(after);
            if (!content.empty()) item.keyIndent = indent + after;
        } else {
            if (!haveItem) continue;
            if (item.keyIndent == 0) item.keyIndent = indent;
            if (indent != item.keyIndent) continue;  // nested content of another key
        }
        if (content.empty()) continue;

        const size_t colon = content.find(':');
        if (colon == std::string_view::npos) {
            if (item.unsupported.empty()) item.unsupported = L"a line without a key";
            continue;
        }
        const std::string_view key = Trim(content.substr(0, colon));
        const std::string_view value = Trim(content.substr(colon + 1));
        std::string text;
        bool ok;
        if (!value.empty() && (value.front() == '|' || value.front() == '>')) {
            ok = ReadBlock(lines, i, item.keyIndent, value, text);
        } else {
            ok = ParseScalar(value, text);
            // Check for multi-line plain scalar (only for supported keys)
            if (ok && !text.empty() && (key == "trigger" || key == "replace" || key == "label")) {
                // Look ahead for deeper-indented continuation lines
                for (size_t j = i + 1; j < lines.size(); ++j) {
                    const std::string_view nextLine = Trim(lines[j]);
                    if (nextLine.empty() || nextLine.front() == '#') continue;
                    const size_t nextIndent = IndentOf(lines[j]);
                    if (nextIndent > item.keyIndent) {
                        // Found a deeper-indented line after a non-block scalar
                        if (item.unsupported.empty()) {
                            item.unsupported = L"a multi-line value needs a | block";
                        }
                    }
                    break;
                }
            }
        }
        if (!ok) {
            if (item.unsupported.empty()) {
                item.unsupported = L"could not read the value of '" + std::wstring(key.begin(), key.end()) + L"'";
            }
            continue;
        }
        if (key == "trigger") {
            item.trigger = std::move(text);
            item.hasTrigger = true;
        } else if (key == "replace") {
            item.replace = std::move(text);
            item.hasReplace = true;
        } else if (key == "label") {
            item.label = std::move(text);
        } else if (item.unsupported.empty()) {
            item.unsupported = L"uses '" + std::wstring(key.begin(), key.end()) + L"', which is not supported yet";
        }
    }
    finish();
    if (keepComments) {
        result.trailingComments = std::move(orphan);
        JoinLines(result.trailingComments, pending);
    }
    return result;
}

// Suffix lookup for the rolling key buffer. Built once per (re)load; the hook
// thread reads it through a shared_ptr, never mutating it.
class SnippetIndex {
public:
    explicit SnippetIndex(std::vector<Snippet> snippets = {}) : snippets_(std::move(snippets)) {
        for (size_t i = 0; i < snippets_.size(); ++i) {
            const std::wstring& trigger = snippets_[i].trigger;
            byTrigger_.emplace(trigger, i);
            lastChars_.insert(trigger.back());
            maxTrigger_ = (std::max)(maxTrigger_, trigger.size());
        }
    }

    size_t Size() const { return snippets_.size(); }
    bool Empty() const { return snippets_.empty(); }
    size_t MaxTriggerLength() const { return maxTrigger_; }
    const std::vector<Snippet>& All() const { return snippets_; }

    // The snippet whose trigger is the longest suffix of `buffer`, or nullptr.
    // A shorter trigger that is a prefix of a longer one fires first, because
    // expansion happens as soon as the last character is typed (documented).
    const Snippet* MatchSuffix(std::wstring_view buffer) const {
        if (buffer.empty() || lastChars_.find(buffer.back()) == lastChars_.end()) return nullptr;
        const size_t longest = (std::min)(buffer.size(), maxTrigger_);
        thread_local std::wstring key;  // reused: this runs inside the keyboard hook, so no per-length allocation
        for (size_t len = longest; len >= kMinTrigger; --len) {
            key.assign(buffer.data() + buffer.size() - len, len);
            const auto it = byTrigger_.find(key);
            if (it != byTrigger_.end()) {
                const Snippet* hit = &snippets_[it->second];
                std::fill(key.begin(), key.end(), L'\0');  // do not leave typed text behind
                return hit;
            }
        }
        std::fill(key.begin(), key.end(), L'\0');
        return nullptr;
    }

private:
    std::vector<Snippet> snippets_;
    std::unordered_map<std::wstring, size_t> byTrigger_;
    std::unordered_set<wchar_t> lastChars_;
    size_t maxTrigger_ = 0;
};

// Indexes into `list`, best match first. An empty query lists everything in
// file order. Label or trigger starting with the query beats containing it.
inline std::vector<size_t> SearchSnippets(const std::vector<Snippet>& list, const std::wstring& normalizedQuery,
                                          size_t limit = 50) {
    std::vector<std::pair<int, size_t>> scored;
    for (size_t i = 0; i < list.size(); ++i) {
        if (normalizedQuery.empty()) {
            scored.emplace_back(0, i);
            continue;
        }
        const std::wstring label = takeoff::Normalize(list[i].label);
        const std::wstring trigger = takeoff::Normalize(list[i].trigger);
        int score = -1;
        if (label.rfind(normalizedQuery, 0) == 0 || trigger.rfind(normalizedQuery, 0) == 0) {
            score = 3;
        } else if (label.find(normalizedQuery) != std::wstring::npos ||
                   trigger.find(normalizedQuery) != std::wstring::npos) {
            score = 2;
        }
        if (score >= 0) scored.emplace_back(score, i);
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<size_t> out;
    for (const auto& s : scored) {
        if (out.size() >= limit) break;
        out.push_back(s.second);
    }
    return out;
}

inline std::wstring PathForIndex(size_t index) { return std::wstring(kPathPrefix) + std::to_wstring(index); }

inline bool IndexFromPath(std::wstring_view path, size_t& index) {
    const std::wstring_view prefix = kPathPrefix;
    if (path.size() <= prefix.size() || path.substr(0, prefix.size()) != prefix) return false;
    const std::wstring_view digits = path.substr(prefix.size());
    if (digits.size() > 9) return false;
    size_t value = 0;
    for (wchar_t ch : digits) {
        if (ch < L'0' || ch > L'9') return false;
        value = value * 10 + static_cast<size_t>(ch - L'0');
    }
    index = value;
    return true;
}

namespace detail {
inline std::string QuoteYaml(const std::string& utf8) {
    std::string out = "\"";
    for (unsigned char ch : utf8) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 0x20) {
                static const char* hex = "0123456789abcdef";
                out += "\\u00";
                out.push_back(hex[ch >> 4]);
                out.push_back(hex[ch & 0xF]);
            } else {
                out.push_back(static_cast<char>(ch));
            }
        }
    }
    out += '"';
    return out;
}
}  // namespace detail

// The file format ParseSnippets reads, used by the Espanso import: one blank
// line before each item, and the item's comment lines (if any) right above it.
inline std::string SerializeSnippets(const std::vector<Snippet>& list, std::string_view trailingComments = {}) {
    std::string out = "matches:\n";
    for (const auto& s : list) {
        out += "\n";
        if (!s.comments.empty()) out += s.comments + "\n";
        out += "  - trigger: " + detail::QuoteYaml(obsidian::WideToUtf8(s.trigger)) + "\n";
        if (!s.label.empty()) out += "    label: " + detail::QuoteYaml(obsidian::WideToUtf8(s.label)) + "\n";
        out += "    replace: " + detail::QuoteYaml(obsidian::WideToUtf8(s.replace)) + "\n";
    }
    if (!trailingComments.empty()) {
        out += "\n";
        out.append(trailingComments);
        out += "\n";
    }
    return out;
}

struct MergeResult {
    std::vector<Snippet> merged;
    size_t added = 0;
    size_t duplicates = 0;
    size_t overflow = 0;  // new snippets that did not fit under kMaxSnippets (not added, not duplicates)
};

// "1 snippet" / "3 snippets" (also used for "entry" / "entries").
inline std::wstring CountNoun(size_t count, const wchar_t* one, const wchar_t* many) {
    return std::to_wstring(count) + L" " + (count == 1 ? one : many);
}

// Espanso items that need variables or a cursor hint. The parser keeps them as
// literal text, so the import filters them out (the user's own files may
// legitimately contain braces, so ParseSnippets itself must not do this).
inline bool LooksLikeEspansoVariable(std::wstring_view replace) {
    if (replace.find(L"$|$") != std::wstring_view::npos) return true;
    const size_t open = replace.find(L"{{");
    return open != std::wstring_view::npos && replace.find(L"}}", open + 2) != std::wstring_view::npos;
}

// snippets-before-import-YYYYMMDD-HHMMSS.yml, with -2, -3, ... before the extension for attempt >= 2.
inline std::wstring BackupName(int year, int month, int day, int hour, int minute, int second, int attempt = 1) {
    auto two = [](int v) { return (v < 10 ? L"0" : L"") + std::to_wstring(v); };
    std::wstring name = L"snippets-before-import-" + std::to_wstring(year) + two(month) + two(day) + L"-" +
                        two(hour) + two(minute) + two(second);
    if (attempt >= 2) name += L"-" + std::to_wstring(attempt);
    return name + L".yml";
}

// The written file must stay loadable: within the size and count limits of ParseSnippets.
inline bool ImportWithinLimits(size_t yamlBytes, size_t count) {
    return yamlBytes <= kMaxFileBytes && count <= kMaxSnippets;
}

// What std::filesystem::status told us about a path. Only a genuine not-found
// counts as missing; any other failure must never be mistaken for "no file".
enum class FileState { Missing, Present, Error };
inline FileState ClassifyFileStatus(const std::error_code& ec, std::filesystem::file_type type) {
    if (type == std::filesystem::file_type::not_found) return FileState::Missing;
    if (ec || type == std::filesystem::file_type::none) return FileState::Error;
    return FileState::Present;
}
inline FileState ClassifyPath(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::file_status status = std::filesystem::status(path, ec);
    return ClassifyFileStatus(ec, status.type());
}

// The confirmation text for the Espanso import. backupName is empty when there is
// no current file (nothing to back up); unreadable counts entries of the current
// file that the parser cannot keep and a rewrite would remove.
inline std::wstring EspansoImportPrompt(size_t added, size_t duplicates, size_t skipped, size_t unreadable,
                                        const std::wstring& backupName, bool limitReached) {
    std::wstring text = L"Add " + CountNoun(added, L"snippet", L"snippets") + L" from Espanso (" +
                        std::to_wstring(duplicates) + L" already exist, " + CountNoun(skipped, L"entry", L"entries") +
                        L" skipped)?";
    if (limitReached) text += L"\n\nImport limit reached: the rest of the Espanso folder was not read.";
    if (unreadable > 0) {
        text += L"\n\n" + CountNoun(unreadable, L"entry", L"entries") +
                L" in your file that Lean Launcher can't read (for example ones using variables or word) " +
                L"will be removed.";
    }
    if (!backupName.empty()) {
        text += L"\n\nYour current snippets file is saved first as " + backupName +
                L" in the same folder. The new snippets are added at the end, grouped by Espanso file, and the" +
                L" comments and layout of your file are kept.";
    }
    return text;
}

// Existing snippets win: an incoming snippet with a trigger already present
// is counted as a duplicate and dropped. The first snippet added from each
// Espanso file gets a "# From Espanso: <file>" header above its own comments,
// so imported snippets stay grouped by file.
inline MergeResult MergeSnippets(std::vector<Snippet> existing, const std::vector<Snippet>& incoming) {
    MergeResult result;
    result.merged = std::move(existing);
    std::unordered_set<std::wstring> triggers;
    std::unordered_set<std::wstring> headed;
    for (const auto& s : result.merged) triggers.insert(s.trigger);
    for (const auto& s : incoming) {
        if (!triggers.insert(s.trigger).second) {
            ++result.duplicates;
        } else if (result.merged.size() < kMaxSnippets) {
            Snippet added = s;
            if (!added.source.empty() && headed.insert(added.source).second) {
                std::string header = "# From Espanso: " + obsidian::WideToUtf8(added.source);
                detail::JoinLines(header, added.comments);
                added.comments = std::move(header);
            }
            added.source.clear();
            result.merged.push_back(std::move(added));
            ++result.added;
        } else {
            ++result.overflow;
        }
    }
    return result;
}

// The characters typed since the last reset, capped at the longest trigger.
// It lives only in memory and is wiped on Clear() and on destruction.
class KeyBuffer {
public:
    KeyBuffer() { text_.reserve(kMaxTrigger + 8); }  // never reallocates: no residue in freed blocks
    ~KeyBuffer() { Clear(); }
    void SetCapacity(size_t capacity) {
        capacity_ = capacity;
        Trim();
    }
    void Append(std::wstring_view text) {
        text_.append(text);
        Trim();
    }
    void Backspace() {
        if (!text_.empty()) text_.back() = L'\0', text_.pop_back();
    }
    void Clear() {
        text_.assign(text_.capacity(), L'\0');  // wipe all storage (never reallocates), not just [0,size())
        text_.clear();
    }
    std::wstring_view View() const { return text_; }
    // Test seams: prove the storage past size() is wiped. Not used by product code.
    size_t StorageCapacityForTest() const { return text_.capacity(); }
    const wchar_t* RawForTest() const { return text_.data(); }

private:
    void Trim() {
        if (text_.size() > capacity_) text_.erase(0, text_.size() - capacity_);  // stale tail is wiped by Clear()
    }
    std::wstring text_;
    size_t capacity_ = kMaxTrigger;
};

enum class ModifierAction { Type, Reset };

// Ctrl+Alt is AltGr on many layouts (German @ { [ \), so it types. Ctrl alone,
// Alt alone and any Windows-key combination are shortcuts and reset the buffer.
inline ModifierAction ClassifyModifiers(bool ctrl, bool alt, bool win) {
    if (win) return ModifierAction::Reset;
    if (ctrl != alt) return ModifierAction::Reset;
    return ModifierAction::Type;
}

// Raw Win32 virtual-key numbers, so this header stays free of windows.h.
inline bool IsResetKey(unsigned vk) {
    return vk == 0x0D /*Enter*/ || vk == 0x09 /*Tab*/ || vk == 0x1B /*Escape*/ ||
           (vk >= 0x21 && vk <= 0x28) /*PgUp PgDn End Home Left Up Right Down*/ ||
           vk == 0x2D /*Insert*/ || vk == 0x2E /*Delete*/;
}

inline bool IsModifierVk(unsigned vk) {
    return (vk >= 0x10 && vk <= 0x12) /*Shift Ctrl Alt*/ || vk == 0x14 /*CapsLock*/ ||
           vk == 0x5B || vk == 0x5C /*Windows*/ || (vk >= 0xA0 && vk <= 0xA5) /*L/R Shift Ctrl Alt*/ ||
           vk == 0x90 /*NumLock*/ || vk == 0x91 /*ScrollLock*/;
}

enum class InsertMode { Keystrokes, Clipboard };
inline constexpr size_t kMaxKeystrokeChars = 100;

// Short single-line text is typed (the clipboard stays untouched); anything
// longer or multi-line is pasted, because typed newlines trigger auto-indent.
inline InsertMode PlanInsert(std::wstring_view replace) {
    if (replace.size() > kMaxKeystrokeChars) return InsertMode::Clipboard;
    if (replace.find_first_of(L"\r\n") != std::wstring_view::npos) return InsertMode::Clipboard;
    return InsertMode::Keystrokes;
}

// CF_UNICODETEXT convention: line breaks are CRLF.
inline std::wstring ToClipboardText(std::wstring_view text) {
    std::wstring out;
    out.reserve(text.size() + 8);
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r')) out.push_back(L'\r');
        out.push_back(text[i]);
    }
    return out;
}

// The Settings screen's check for the snippets file path. Empty means the
// default (%APPDATA%\LeanLauncher\snippets.yml).
inline const wchar_t* SnippetsPathProblem(std::wstring_view path) {
    if (path.empty()) return nullptr;
    if (path.size() < 4 || !iswalpha(path[0]) || path[1] != L':' || (path[2] != L'\\' && path[2] != L'/')) {
        return L"Use a full path like C:\\Users\\you\\snippets.yml";
    }
    if (path.find(L"..") != std::wstring_view::npos) return L"The path must not contain '..'";
    std::wstring lower(path);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    const auto ends = [&](std::wstring_view suffix) {
        return lower.size() >= suffix.size() && lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    if (!ends(L".yml") && !ends(L".yaml")) return L"The file must end in .yml or .yaml";
    return nullptr;
}

}  // namespace snippets
}  // namespace leanlauncher
