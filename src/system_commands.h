#pragma once

#include "search.h"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// System commands (US-041): lock, sleep, hibernate, restart, shut down, sign
// out, empty Recycle Bin. The pure logic lives here, free of window state, so
// it can be unit-tested; the launcher owns the Win32 calls that run them.
// The table is compile-time only, so no registry value can add a command
// (the same principle as NFR-009's compile-time update endpoints).
namespace leanlauncher {
namespace syscmd {

enum class Command : uint8_t { Lock, Sleep, Hibernate, Restart, ShutDown, SignOut, EmptyRecycleBin };

struct CommandDef {
    Command id;
    const wchar_t* name;     // result row title
    const wchar_t* slug;     // identity in the result's path
    const wchar_t* verb;     // for "Press Enter again to <verb>"
    const wchar_t* subtitle; // result row description
    const wchar_t* aliases[3];
};

inline constexpr CommandDef kCommands[] = {
    {Command::Lock, L"Lock", L"lock", L"lock", L"Lock this PC", {L"lock", L"lock pc", nullptr}},
    {Command::Sleep, L"Sleep", L"sleep", L"sleep", L"Put this PC to sleep", {L"sleep", L"suspend", nullptr}},
    {Command::Hibernate, L"Hibernate", L"hibernate", L"hibernate", L"Hibernate this PC", {L"hibernate", nullptr, nullptr}},
    {Command::Restart, L"Restart", L"restart", L"restart", L"Restart this PC", {L"restart", L"reboot", nullptr}},
    {Command::ShutDown, L"Shut Down", L"shutdown", L"shut down", L"Shut down this PC", {L"shut down", L"shutdown", L"power off"}},
    {Command::SignOut, L"Sign Out", L"signout", L"sign out", L"Sign out of Windows", {L"sign out", L"log off", L"logout"}},
    {Command::EmptyRecycleBin, L"Empty Recycle Bin", L"emptybin", L"empty the Recycle Bin", L"",
        {L"empty recycle bin", L"empty trash", L"bin"}},
};

// Beats any app or settings page, including one boosted by recency
// (an exact name match scores 10000, plus at most 800 for recency).
inline constexpr int kExactCommandScore = 12000;
inline constexpr unsigned long long kConfirmTimeoutMs = 5000;
inline constexpr wchar_t kPathPrefix[] = L"leanlauncher:command:";

inline const CommandDef* FindCommand(Command id) {
    for (const auto& def : kCommands) {
        if (def.id == id) return &def;
    }
    return nullptr;
}

// Only the commands that can lose unsaved work or data ask for a second Enter.
inline bool NeedsConfirmation(Command id) {
    return id == Command::Restart || id == Command::ShutDown ||
           id == Command::SignOut || id == Command::EmptyRecycleBin;
}

inline std::wstring ConfirmPrompt(Command id) {
    const CommandDef* def = FindCommand(id);
    return std::wstring(L"Press Enter again to ") + (def ? def->verb : L"continue");
}

enum class PressResult { Execute, AskAgain };

// First Enter on a destructive command arms it; only a second Enter on the
// same command within kConfirmTimeoutMs runs it. The launcher calls Cancel()
// when the query changes or the window hides.
class ConfirmGate {
public:
    PressResult Press(Command id, unsigned long long nowMs) {
        if (!NeedsConfirmation(id)) return PressResult::Execute;
        if (IsPending(id, nowMs)) {
            pending_.reset();
            return PressResult::Execute;
        }
        pending_ = id;
        armedAtMs_ = nowMs;
        return PressResult::AskAgain;
    }

    bool IsPending(Command id, unsigned long long nowMs) const {
        return pending_ == id && nowMs - armedAtMs_ <= kConfirmTimeoutMs;
    }

    std::optional<Command> Pending() const { return pending_; }

    void Cancel() { pending_.reset(); }

private:
    std::optional<Command> pending_;
    unsigned long long armedAtMs_ = 0;
};

inline int ScoreCommand(std::wstring_view normalizedName, const std::vector<std::wstring>& aliases,
                        std::wstring_view query) {
    if (query.empty()) return -1;
    if (normalizedName == query) return kExactCommandScore;
    for (const auto& alias : aliases) {
        if (alias == query) return kExactCommandScore;
    }
    return takeoff::ScoreApp(normalizedName, aliases, query, -1);
}

inline std::wstring FormatRecycleBinSummary(long long items, long long bytes) {
    if (items <= 0) return L"Recycle Bin is empty";
    std::wstring text = std::to_wstring(items) + (items == 1 ? L" item, " : L" items, ");
    if (bytes < 1024) return text + std::to_wstring(bytes) + L" bytes";
    static constexpr const wchar_t* kUnits[] = {L"KB", L"MB", L"GB", L"TB"};
    double value = static_cast<double>(bytes) / 1024.0;
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(kUnits)) {
        value /= 1024.0;
        ++unit;
    }
    wchar_t buffer[32];
    swprintf(buffer, std::size(buffer), L"%.1f %s", value, kUnits[unit]);
    return text + buffer;
}

// shutdown.exe arguments. Never /f, so apps can still ask to save work.
inline std::wstring ShutdownArguments(Command id) {
    return id == Command::Restart ? L"/r /t 0" : L"/s /t 0";
}

struct PowerCaps {
    bool canSleep = true;
    bool canHibernate = true;
};

inline bool IsAvailable(Command id, const PowerCaps& caps) {
    if (id == Command::Sleep) return caps.canSleep;
    if (id == Command::Hibernate) return caps.canHibernate;
    return true;
}

// "<prefix> <filter>" narrows results to system commands. Unlike the other
// prefixes, the filter may be empty: "s " alone lists every command.
// Case-insensitive on the prefix; spaces after it are trimmed.
inline bool TryParseCommandPrefix(std::wstring_view input, std::wstring_view prefix, std::wstring& filter) {
    if (prefix.empty() || input.size() <= prefix.size() || input[prefix.size()] != L' ') return false;
    if (_wcsnicmp(input.data(), prefix.data(), prefix.size()) != 0) return false;
    std::wstring_view rest = input.substr(prefix.size());
    const size_t start = rest.find_first_not_of(L' ');
    filter.assign(start == std::wstring_view::npos ? std::wstring_view{} : rest.substr(start));
    return true;
}

inline std::wstring CommandPath(Command id) {
    const CommandDef* def = FindCommand(id);
    return std::wstring(kPathPrefix) + (def ? def->slug : L"");
}

inline std::optional<Command> CommandFromPath(std::wstring_view path) {
    const std::wstring_view prefix(kPathPrefix);
    if (path.substr(0, prefix.size()) != prefix) return std::nullopt;
    const std::wstring_view slug = path.substr(prefix.size());
    for (const auto& def : kCommands) {
        if (slug == def.slug) return def.id;
    }
    return std::nullopt;
}

} // namespace syscmd
} // namespace leanlauncher
