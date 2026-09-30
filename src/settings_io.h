#pragma once

#include "settings.h"
#include "search.h"
#include "obsidian_config.h"
#include "daily_note.h"
#include "snippets.h"

#include <cstdlib>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Settings export/import (US-044): the file format and every check an import
// goes through, kept free of window, registry and file-dialog state so it can
// be unit-tested. An import is untrusted input, so it uses exactly the same
// validators as the Settings screen (prefix conflicts, the web template check,
// target notes inside the vault - NFR-009, hotkey conflicts) and never touches
// run-at-startup (NFR-016). Machine state and history are never exported.
namespace leanlauncher {
namespace settings_io {

inline constexpr int kSchemaVersion = 1;
inline constexpr size_t kMaxImportBytes = 64 * 1024;
inline constexpr size_t kMaxTextLength = 256;
inline constexpr int kQuickOpenTargetCount = 4;

struct ExportOptions {
    bool includeVaultPath = false;
    bool includePins = false;
    bool includeExclusions = false;
};

// Machine-specific extras the export dialog offers as checkboxes.
struct PortableExtras {
    std::wstring vaultPath;
    std::vector<std::wstring> pins;        // EncodePin() strings, e.g. "app|C:\..."
    std::vector<std::wstring> exclusions;  // lines of file_search_excludes.txt
};

struct ImportResult {
    bool ok = false;
    std::wstring error;              // why the whole file was rejected
    takeoff::Settings settings;      // current settings with the file's valid values applied
    int changed = 0;                 // settings that differ from the current ones
    std::vector<std::wstring> skipped;  // "<setting>: <reason>"
    bool newerSchema = false;
    std::optional<std::wstring> vaultPath;
    bool hasPins = false;
    std::vector<std::wstring> pins;
    bool hasExclusions = false;
    std::vector<std::wstring> exclusions;
};

namespace detail {

using takeoff::HotkeyBinding;
using takeoff::Settings;

enum class TextKind { Plain, Prefix, TargetNote, WebTemplate, Minutes, SnippetsFile };

struct BoolField { const char* key; bool Settings::*member; };
struct TextField { const char* key; std::wstring Settings::*member; TextKind kind; };
struct HotkeyField { const char* name; HotkeyBinding Settings::*member; bool hasKey; int row; };

inline constexpr HotkeyField kHotkeys[] = {
    {"Launcher", &Settings::launcherHotkey, true, 0},
    {"Actions", &Settings::actionsHotkey, true, 1},
    {"Admin", &Settings::administratorHotkey, false, 2},
    {"Quick", &Settings::quickLaunchHotkey, false, 3},
    {"Preview", &Settings::previewHotkey, true, 4},
};

inline constexpr BoolField kBools[] = {
    {"ShowTrayIcon", &Settings::showTrayIcon},
    {"CheckForUpdates", &Settings::checkForUpdates},
    {"KeepOpenOnFocusLoss", &Settings::keepOpenOnFocusLoss},
    {"RestoreLastQuery", &Settings::restoreLastQuery},
    {"FileSearchEnabled", &Settings::enableFileSearch},
    {"WebSearchEnabled", &Settings::enableWebSearch},
    {"SystemCommandsEnabled", &Settings::enableSystemCommands},
    {"TypedUrlsEnabled", &Settings::enableTypedUrls},
    {"PathCompletionEnabled", &Settings::enablePathCompletion},
    {"UnitConverterEnabled", &Settings::enableUnitConverter},
    {"TimeZonesEnabled", &Settings::enableTimeZones},
    {"PomodoroEnabled", &Settings::enablePomodoro},
    {"PomodoroLog", &Settings::pomodoroLog},
    {"PreviewEnabled", &Settings::enablePreview},
    // PreviewOpen is window state (whether the panel is currently expanded),
    // not a preference - like RunAtStartup, it's never exported or imported.
    // Its registry load/save stays in launcher.h's LoadSettings/SaveSettings.
    // SnippetsEnabled (US-050) is likewise never exported or imported: a settings file must not switch on a keyboard hook. Its registry load/save stays in launcher.h.
    {"ObsidianEnabled", &Settings::obsidianEnabled},
    {"VaultSearchEnabled", &Settings::vaultSearchEnabled},
    {"TaskAddEnabled", &Settings::taskAddEnabled},
    {"NoteAddEnabled", &Settings::noteAddEnabled},
    {"LogEnabled", &Settings::logEnabled},
};

inline constexpr TextField kTexts[] = {
    {"WebSearchUrlTemplate", &Settings::webSearchUrlTemplate, TextKind::WebTemplate},
    {"WebSearchEngineName", &Settings::webSearchEngineName, TextKind::Plain},
    {"WebSearchPrefix", &Settings::webSearchPrefix, TextKind::Prefix},
    {"WebSearchPillLabel", &Settings::webSearchPillLabel, TextKind::Plain},
    {"FileSearchPrefix", &Settings::fileSearchPrefix, TextKind::Prefix},
    {"AppSearchPrefix", &Settings::appSearchPrefix, TextKind::Prefix},
    {"SystemCommandsPrefix", &Settings::systemCommandsPrefix, TextKind::Prefix},
    {"VaultSearchPrefix", &Settings::vaultSearchPrefix, TextKind::Prefix},
    {"VaultSearchPillLabel", &Settings::vaultSearchPillLabel, TextKind::Plain},
    {"TaskPrefix", &Settings::taskPrefix, TextKind::Prefix},
    {"TaskPillLabel", &Settings::taskPillLabel, TextKind::Plain},
    {"TaskPreviewPrefix", &Settings::taskPreviewPrefix, TextKind::Plain},
    {"NoteAddPrefix", &Settings::noteAddPrefix, TextKind::Prefix},
    {"NoteAddPillLabel", &Settings::noteAddPillLabel, TextKind::Plain},
    {"NoteAddPreviewPrefix", &Settings::noteAddPreviewPrefix, TextKind::Plain},
    {"LogPrefix", &Settings::logPrefix, TextKind::Prefix},
    {"LogPillLabel", &Settings::logPillLabel, TextKind::Plain},
    {"LogPreviewPrefix", &Settings::logPreviewPrefix, TextKind::Plain},
    {"LogHeading", &Settings::logHeading, TextKind::Plain},
    {"DailyNoteFolderOverride", &Settings::dailyNoteFolderOverride, TextKind::Plain},
    {"DailyNoteFormatOverride", &Settings::dailyNoteFormatOverride, TextKind::Plain},
    {"TaskTargetNote", &Settings::taskTargetNote, TextKind::TargetNote},
    {"NoteAddTargetNote", &Settings::noteAddTargetNote, TextKind::TargetNote},
    {"LogTargetNote", &Settings::logTargetNote, TextKind::TargetNote},
    {"PomodoroPrefix", &Settings::pomodoroPrefix, TextKind::Prefix},
    {"PomodoroFocusMinutes", &Settings::pomodoroFocusMinutes, TextKind::Minutes},
    {"PomodoroBreakMinutes", &Settings::pomodoroBreakMinutes, TextKind::Minutes},
    {"SnippetsPrefix", &Settings::snippetsPrefix, TextKind::Prefix},
    {"SnippetsPath", &Settings::snippetsPath, TextKind::SnippetsFile},
};

// ---- JSON writing -----------------------------------------------------------

inline void AppendJsonString(std::string& out, const std::wstring& value) {
    const std::string utf8 = obsidian::WideToUtf8(value);
    out += '"';
    for (unsigned char ch : utf8) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 0x20) {
                static const char kHex[] = "0123456789abcdef";
                out += "\\u00";
                out += kHex[ch >> 4];
                out += kHex[ch & 0xF];
            } else {
                out += static_cast<char>(ch);
            }
        }
    }
    out += '"';
}

// ---- JSON reading (strict, small, depth-limited) ----------------------------

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::wstring text;
    std::vector<JsonValue> items;       // array items, or object values
    std::vector<std::string> keys;      // object keys (parallel to items)

    const JsonValue* Find(std::string_view key) const {
        if (type != Type::Object) return nullptr;
        for (size_t i = 0; i < keys.size(); ++i) {
            if (keys[i] == key) return &items[i];
        }
        return nullptr;
    }
};

class JsonReader {
public:
    explicit JsonReader(std::string_view text) : s_(text) {}

    bool ParseDocument(JsonValue& out) {
        if (!ParseValue(out, 0)) return false;
        SkipSpace();
        return pos_ == s_.size();
    }

private:
    static constexpr int kMaxDepth = 16;
    std::string_view s_;
    size_t pos_ = 0;

    void SkipSpace() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\r' || s_[pos_] == '\n')) ++pos_;
    }

    bool Literal(std::string_view word) {
        if (s_.substr(pos_, word.size()) != word) return false;
        pos_ += word.size();
        return true;
    }

    static void AppendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool Hex4(uint32_t& out) {
        if (pos_ + 4 > s_.size()) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<uint32_t>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool ParseString(std::string& utf8) {
        if (pos_ >= s_.size() || s_[pos_] != '"') return false;
        ++pos_;
        while (pos_ < s_.size()) {
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') {
                utf8 += c;
                continue;
            }
            if (pos_ >= s_.size()) return false;
            const char e = s_[pos_++];
            switch (e) {
            case '"': utf8 += '"'; break;
            case '\\': utf8 += '\\'; break;
            case '/': utf8 += '/'; break;
            case 'b': utf8 += '\b'; break;
            case 'f': utf8 += '\f'; break;
            case 'n': utf8 += '\n'; break;
            case 'r': utf8 += '\r'; break;
            case 't': utf8 += '\t'; break;
            case 'u': {
                uint32_t cp = 0;
                if (!Hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    uint32_t low = 0;
                    if (!Literal("\\u") || !Hex4(low) || low < 0xDC00 || low > 0xDFFF) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return false;
                }
                AppendUtf8(utf8, cp);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    bool ParseNumber(double& out) {
        const size_t start = pos_;
        if (pos_ < s_.size() && s_[pos_] == '-') ++pos_;
        size_t digits = 0;
        while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') { ++pos_; ++digits; }
        if (digits == 0) return false;
        if (pos_ < s_.size() && s_[pos_] == '.') {
            ++pos_;
            size_t frac = 0;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') { ++pos_; ++frac; }
            if (frac == 0) return false;
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            size_t exp = 0;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') { ++pos_; ++exp; }
            if (exp == 0) return false;
        }
        const std::string number(s_.substr(start, pos_ - start));
        out = std::strtod(number.c_str(), nullptr);
        return true;
    }

    bool ParseValue(JsonValue& out, int depth) {
        if (depth > kMaxDepth) return false;
        SkipSpace();
        if (pos_ >= s_.size()) return false;
        const char c = s_[pos_];
        if (c == '{') {
            out.type = JsonValue::Type::Object;
            ++pos_;
            SkipSpace();
            if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
            while (true) {
                SkipSpace();
                std::string key;
                if (!ParseString(key)) return false;
                SkipSpace();
                if (pos_ >= s_.size() || s_[pos_] != ':') return false;
                ++pos_;
                JsonValue value;
                if (!ParseValue(value, depth + 1)) return false;
                out.keys.push_back(std::move(key));
                out.items.push_back(std::move(value));
                SkipSpace();
                if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
                if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
                return false;
            }
        }
        if (c == '[') {
            out.type = JsonValue::Type::Array;
            ++pos_;
            SkipSpace();
            if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
            while (true) {
                JsonValue value;
                if (!ParseValue(value, depth + 1)) return false;
                out.items.push_back(std::move(value));
                SkipSpace();
                if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
                if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
                return false;
            }
        }
        if (c == '"') {
            std::string utf8;
            if (!ParseString(utf8)) return false;
            out.type = JsonValue::Type::String;
            out.text = obsidian::Utf8ToWide(utf8);
            return true;
        }
        if (Literal("true")) { out.type = JsonValue::Type::Bool; out.boolean = true; return true; }
        if (Literal("false")) { out.type = JsonValue::Type::Bool; out.boolean = false; return true; }
        if (Literal("null")) { out.type = JsonValue::Type::Null; return true; }
        out.type = JsonValue::Type::Number;
        return ParseNumber(out.number);
    }
};

inline std::wstring Wide(const char* ascii) {
    return std::wstring(ascii, ascii + std::char_traits<char>::length(ascii));
}

inline bool IsPlainText(const std::wstring& value) {
    if (value.size() > kMaxTextLength) return false;
    for (wchar_t ch : value) {
        if (ch < 0x20) return false;
    }
    return true;
}

inline bool ReadInteger(const JsonValue& value, int minValue, int maxValue, int& out) {
    if (value.type != JsonValue::Type::Number) return false;
    const double n = value.number;
    if (n < minValue || n > maxValue || n != static_cast<double>(static_cast<int>(n))) return false;
    out = static_cast<int>(n);
    return true;
}

inline std::vector<std::wstring> ReadStringArray(const JsonValue& value) {
    std::vector<std::wstring> out;
    if (value.type != JsonValue::Type::Array) return out;
    for (const auto& item : value.items) {
        if (item.type == JsonValue::Type::String && IsPlainText(item.text) && !item.text.empty()) {
            out.push_back(item.text);
        }
    }
    return out;
}

// 1-180 whole minutes, as the Pomodoro length rows accept.
inline bool IsMinutes(const std::wstring& text) {
    if (text.empty() || text.size() > 3) return false;
    int value = 0;
    for (wchar_t ch : text) {
        if (ch < L'0' || ch > L'9') return false;
        value = value * 10 + (ch - L'0');
    }
    return value >= 1 && value <= 180;
}

// The snippets prefix only counts while the feature is on, so its default cannot block another prefix saved with the same value.
inline std::vector<std::wstring> Prefixes(const Settings& s) {
    std::vector<std::wstring> out;
    for (const auto& f : kTexts) {
        if (f.kind != TextKind::Prefix) continue;
        if (f.member == &Settings::snippetsPrefix && !s.enableSnippets) continue;
        out.push_back(s.*(f.member));
    }
    return out;
}

// The Settings screen's own prefix check (empty or colliding prefixes).
inline const wchar_t* PrefixConflict(const Settings& s) {
    return obsidian::FindPrefixConflict(Prefixes(s));
}

inline const wchar_t* HotkeyProblem(const HotkeyField& field, const HotkeyBinding& proposed, const Settings& candidate) {
    if (proposed.disabled) return nullptr;
    if (field.hasKey && takeoff::IsSystemReserved(proposed.modifiers, proposed.key)) return L"reserved by Windows";
    // Row 4 (Preview) is matched the same way as row 1 (Actions): a local
    // WM_KEYDOWN check while the search box has focus, not a global
    // RegisterHotKey. So it can steal the same text-editing combos
    // (Ctrl+A/C/V/X/Z/L) out of the search box the same way Actions can.
    if ((field.row == 1 || field.row == 4) && takeoff::IsReservedInApp(proposed)) return L"used by text editing";
    return takeoff::HasInternalConflict(field.row, proposed, candidate);
}

inline const TextField* FindTextField(std::wstring Settings::*member) {
    for (const auto& f : kTexts) {
        if (f.member == member) return &f;
    }
    return nullptr;
}

} // namespace detail

// Result of checking one proposed text-setting value.
struct TextCheck {
    const wchar_t* error = nullptr;  // the Settings screen's message, or nullptr when accepted
    std::wstring value;              // what to store (target notes normalised, blanks cleared)
};

// The one check a proposed text value goes through, used by both the Settings
// screen and the import so the two can't drift apart. kTexts decides which
// validator runs for which field; a field not listed there is plain text.
inline TextCheck CheckTextSetting(const takeoff::Settings& current, std::wstring takeoff::Settings::*member,
                                  const std::wstring& proposed) {
    using namespace detail;
    TextCheck result;
    result.value = proposed;
    const TextField* field = FindTextField(member);
    switch (field ? field->kind : TextKind::Plain) {
    case TextKind::Prefix: {
        Settings candidate = current;
        candidate.*member = proposed;
        result.error = PrefixConflict(candidate);
        break;
    }
    case TextKind::TargetNote:
        if (proposed.find_first_not_of(L" \t") == std::wstring::npos) {
            result.value.clear();
        } else if (!obsidian::NormalizeTargetNoteRef(proposed, result.value)) {
            result.error = L"Use a note path inside the vault, e.g. Inbox/Tasks";
        }
        break;
    case TextKind::Minutes:
        if (!IsMinutes(proposed)) result.error = L"Enter a whole number of minutes from 1 to 180.";
        break;
    case TextKind::WebTemplate:
        result.error = takeoff::FindWebSearchUrlError(proposed);
        break;
    case TextKind::SnippetsFile:
        result.error = leanlauncher::snippets::SnippetsPathProblem(proposed);
        break;
    case TextKind::Plain:
        break;
    }
    return result;
}

// Maps a pointer to a text field of `settings` back to its member, or nullptr.
inline std::wstring takeoff::Settings::* TextSettingMember(const takeoff::Settings& settings, const std::wstring* field) {
    for (const auto& f : detail::kTexts) {
        if (&(settings.*(f.member)) == field) return f.member;
    }
    return nullptr;
}

inline std::string ExportJson(const takeoff::Settings& settings, const PortableExtras& extras,
                              const ExportOptions& options, std::wstring_view appVersion) {
    using namespace detail;
    std::string out;
    out += "{\n  \"format\": \"lean-launcher-settings\",\n  \"schemaVersion\": ";
    out += std::to_string(kSchemaVersion);
    out += ",\n  \"appVersion\": ";
    AppendJsonString(out, std::wstring(appVersion));
    out += ",\n  \"settings\": {\n";
    bool first = true;
    const auto key = [&](const std::string& name) {
        out += first ? "    \"" : ",\n    \"";
        first = false;
        out += name;
        out += "\": ";
    };
    for (const auto& h : kHotkeys) {
        const HotkeyBinding& b = settings.*(h.member);
        key(std::string(h.name) + "Mod"); out += std::to_string(b.modifiers);
        if (h.hasKey) { key(std::string(h.name) + "Key"); out += std::to_string(b.key); }
        key(std::string(h.name) + "Off"); out += b.disabled ? "true" : "false";
    }
    for (const auto& f : kBools) {
        key(f.key); out += (settings.*(f.member)) ? "true" : "false";
    }
    for (const auto& f : kTexts) {
        key(f.key); AppendJsonString(out, settings.*(f.member));
    }
    key("QuickOpenTarget"); out += std::to_string(settings.quickOpenTarget);
    out += "\n  }";
    const auto array = [&](const char* name, const std::vector<std::wstring>& values) {
        out += ",\n  \"";
        out += name;
        out += "\": [";
        for (size_t i = 0; i < values.size(); ++i) {
            out += i == 0 ? "\n    " : ",\n    ";
            AppendJsonString(out, values[i]);
        }
        out += values.empty() ? "]" : "\n  ]";
    };
    if (options.includeVaultPath) {
        out += ",\n  \"vaultPath\": ";
        AppendJsonString(out, extras.vaultPath);
    }
    if (options.includePins) array("pins", extras.pins);
    if (options.includeExclusions) array("exclusions", extras.exclusions);
    out += "\n}\n";
    return out;
}

inline ImportResult ParseImport(std::string_view json, const takeoff::Settings& current) {
    using namespace detail;
    ImportResult result;
    result.settings = current;
    if (json.size() > kMaxImportBytes) {
        result.error = L"This file is too large to be a Lean Launcher settings file.";
        return result;
    }
    JsonValue root;
    if (!JsonReader(json).ParseDocument(root) || root.type != JsonValue::Type::Object) {
        result.error = L"This isn't a valid settings file.";
        return result;
    }
    const JsonValue* format = root.Find("format");
    if (!format || format->type != JsonValue::Type::String || format->text != L"lean-launcher-settings") {
        result.error = L"This isn't a Lean Launcher settings file.";
        return result;
    }
    int schema = 0;
    const JsonValue* version = root.Find("schemaVersion");
    if (!version || !ReadInteger(*version, 1, 1000, schema)) {
        result.error = L"This settings file has no valid schema version.";
        return result;
    }
    result.newerSchema = schema > kSchemaVersion;
    const JsonValue* values = root.Find("settings");
    if (!values || values->type != JsonValue::Type::Object) {
        result.error = L"This settings file has no settings.";
        return result;
    }

    Settings& candidate = result.settings;
    const auto skip = [&](const std::string& name, const wchar_t* reason) {
        result.skipped.push_back(Wide(name.c_str()) + L": " + reason);
    };

    for (const auto& f : kBools) {
        if (const JsonValue* v = values->Find(f.key)) {
            if (v->type == JsonValue::Type::Bool) candidate.*(f.member) = v->boolean;
            else skip(f.key, L"must be true or false");
        }
    }
    bool templateChanged = false;
    for (const auto& f : kTexts) {
        const JsonValue* v = values->Find(f.key);
        if (!v) continue;
        if (v->type != JsonValue::Type::String || !IsPlainText(v->text)) {
            skip(f.key, L"must be text of up to 256 characters");
            continue;
        }
        if (f.kind == TextKind::Prefix) {
            candidate.*(f.member) = v->text;  // checked together below, as one import may swap prefixes
            continue;
        }
        const TextCheck checked = CheckTextSetting(candidate, f.member, v->text);
        if (checked.error) {
            skip(f.key, f.kind == TextKind::TargetNote ? L"must be a note path inside the vault"
                      : f.kind == TextKind::Minutes    ? L"must be a whole number of minutes from 1 to 180"
                                                       : checked.error);
            continue;
        }
        if (f.kind == TextKind::WebTemplate) templateChanged = checked.value != candidate.*(f.member);
        candidate.*(f.member) = checked.value;
    }
    if (templateChanged && !values->Find("WebSearchEngineName")) {
        candidate.webSearchEngineName = takeoff::DeriveSearchEngineName(candidate.webSearchUrlTemplate);
    }
    // Prefixes: any imported prefix that empties or collides is kept as it was.
    if (PrefixConflict(candidate)) {
        for (const auto& f : kTexts) {
            if (f.kind != TextKind::Prefix || candidate.*(f.member) == current.*(f.member)) continue;
            const std::wstring imported = candidate.*(f.member);
            candidate.*(f.member) = current.*(f.member);
            skip(f.key, imported.empty() ? L"prefix cannot be empty" : L"collides with another prefix");
        }
        if (PrefixConflict(candidate)) {
            for (const auto& f : kTexts) {
                if (f.kind == TextKind::Prefix) candidate.*(f.member) = current.*(f.member);
            }
        }
    }
    for (const auto& h : kHotkeys) {
        HotkeyBinding proposed = candidate.*(h.member);
        bool present = false, valid = true;
        int number = 0;
        if (const JsonValue* v = values->Find(std::string(h.name) + "Mod")) {
            present = true;
            if (ReadInteger(*v, 0, 0xFFFF, number)) proposed.modifiers = static_cast<uint16_t>(number); else valid = false;
        }
        if (h.hasKey) {
            if (const JsonValue* v = values->Find(std::string(h.name) + "Key")) {
                present = true;
                if (ReadInteger(*v, 0, 0xFFFF, number)) proposed.key = static_cast<uint16_t>(number); else valid = false;
            }
        }
        if (const JsonValue* v = values->Find(std::string(h.name) + "Off")) {
            present = true;
            if (v->type == JsonValue::Type::Bool) proposed.disabled = v->boolean; else valid = false;
        }
        if (!present) continue;
        const std::string label = std::string(h.name) + " shortcut";
        if (!valid) { skip(label, L"has an invalid value"); continue; }
        if (proposed == candidate.*(h.member)) continue;
        if (const wchar_t* problem = HotkeyProblem(h, proposed, candidate)) { skip(label, problem); continue; }
        candidate.*(h.member) = proposed;
    }
    if (const JsonValue* v = values->Find("QuickOpenTarget")) {
        int target = 0;
        if (ReadInteger(*v, 0, kQuickOpenTargetCount - 1, target)) candidate.quickOpenTarget = target;
        else skip("QuickOpenTarget", L"is out of range");
    }
    candidate.runAtStartup = current.runAtStartup;  // NFR-016: never from a file

    // Count changed settings (a shortcut counts once, whatever parts changed).
    for (const auto& h : kHotkeys) {
        if (!(candidate.*(h.member) == current.*(h.member)) ||
            (candidate.*(h.member)).modifiers != (current.*(h.member)).modifiers ||
            (candidate.*(h.member)).key != (current.*(h.member)).key) {
            ++result.changed;
        }
    }
    for (const auto& f : kBools) {
        if (candidate.*(f.member) != current.*(f.member)) ++result.changed;
    }
    for (const auto& f : kTexts) {
        if (candidate.*(f.member) != current.*(f.member)) ++result.changed;
    }
    if (candidate.quickOpenTarget != current.quickOpenTarget) ++result.changed;

    if (const JsonValue* v = root.Find("vaultPath")) {
        if (v->type == JsonValue::Type::String && IsPlainText(v->text) && !v->text.empty()) result.vaultPath = v->text;
    }
    if (const JsonValue* v = root.Find("pins")) {
        result.hasPins = true;
        result.pins = ReadStringArray(*v);
    }
    if (const JsonValue* v = root.Find("exclusions")) {
        result.hasExclusions = true;
        result.exclusions = ReadStringArray(*v);
    }
    result.ok = true;
    return result;
}

// Imported exclusions are added to the current ones, never replacing them.
inline std::vector<std::wstring> MergeExclusions(const std::vector<std::wstring>& existing,
                                                 const std::vector<std::wstring>& incoming) {
    std::vector<std::wstring> merged = existing;
    for (const auto& line : incoming) {
        bool present = false;
        for (const auto& have : merged) {
            if (_wcsicmp(have.c_str(), line.c_str()) == 0) { present = true; break; }
        }
        if (!present) merged.push_back(line);
    }
    return merged;
}

} // namespace settings_io
} // namespace leanlauncher
