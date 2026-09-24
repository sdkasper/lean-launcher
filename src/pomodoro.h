#pragma once

#include "daily_note.h"

#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// Pomodoro timer (US-049): one focus timer or break at a time, a tray
// balloon at the end, and finished focus timers logged to the daily note.
// The timer only ever appends to a note that already exists: it never creates
// a daily note and never starts Obsidian (background actions must not).
namespace leanlauncher {
namespace pomodoro {

inline constexpr int kMaxMinutes = 180;
inline constexpr wchar_t kTomato[] = L"\U0001F345";

enum class Kind { Focus, Break };

struct Command {
    enum class Kind { None, Menu, StartFocus, StartBreak, Stop, Invalid } kind = Kind::None;
    int minutes = 0;
    std::wstring label;
};

namespace detail {

inline std::wstring Trim(std::wstring_view s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) ++a;
    while (b > a && iswspace(s[b - 1])) --b;
    return std::wstring(s.substr(a, b - a));
}

// Leading whole number, or -1 if the text doesn't start with digits.
inline int LeadingNumber(const std::wstring& s, size_t& used) {
    used = 0;
    long long n = 0;
    while (used < s.size() && iswdigit(s[used]) && used < 6) n = n * 10 + (s[used++] - L'0');
    if (used == 0 || (used < s.size() && !iswspace(s[used]))) {
        used = 0;
        return -1;
    }
    return static_cast<int>(n);
}

} // namespace detail

// "pomo", "pomo 25 write intro", "pomo write intro", "pomo break [N]",
// "pomo stop". Case-insensitive on the prefix, which needs a space after it
// (or to be the whole text).
inline Command ParseCommand(std::wstring_view input, std::wstring_view prefix, int defaultFocus, int defaultBreak) {
    Command command;
    if (prefix.empty() || input.size() < prefix.size()) return command;
    if (_wcsnicmp(input.data(), prefix.data(), prefix.size()) != 0) return command;
    if (input.size() > prefix.size() && input[prefix.size()] != L' ') return command;
    const std::wstring rest = detail::Trim(input.substr(prefix.size()));
    if (rest.empty()) {
        command.kind = Command::Kind::Menu;
        return command;
    }
    std::wstring lowered = rest;
    for (auto& ch : lowered) ch = static_cast<wchar_t>(towlower(ch));
    if (lowered == L"stop") {
        command.kind = Command::Kind::Stop;
        return command;
    }
    const bool isBreak = lowered == L"break" || lowered.rfind(L"break ", 0) == 0;
    const std::wstring body = isBreak ? detail::Trim(std::wstring_view(rest).substr(5)) : rest;
    size_t used = 0;
    int minutes = detail::LeadingNumber(body, used);
    std::wstring label = detail::Trim(std::wstring_view(body).substr(used));
    if (minutes < 0) minutes = isBreak ? defaultBreak : defaultFocus;
    if (minutes < 1 || minutes > kMaxMinutes || (isBreak && !label.empty())) {
        command.kind = Command::Kind::Invalid;
        return command;
    }
    command.kind = isBreak ? Command::Kind::StartBreak : Command::Kind::StartFocus;
    command.minutes = minutes;
    command.label = std::move(label);
    return command;
}

// Whole seconds left, rounded up; 0 once the end has passed.
inline long long RemainingSeconds(unsigned long long endTicks, unsigned long long nowTicks) {
    if (nowTicks >= endTicks) return 0;
    return static_cast<long long>((endTicks - nowTicks + 9999999ULL) / 10000000ULL);
}

inline std::wstring FormatClock(long long seconds) {
    wchar_t buffer[16];
    swprintf(buffer, std::size(buffer), L"%02lld:%02lld", seconds / 60, seconds % 60);
    return buffer;
}

inline long long MinutesLeft(long long seconds) { return (seconds + 59) / 60; }

inline std::wstring FooterLabel(long long seconds) {
    return std::wstring(kTomato) + L" " + std::to_wstring(MinutesLeft(seconds)) + L"m";
}

inline std::wstring TooltipText(Kind kind, long long seconds, const std::wstring& label) {
    const std::wstring minutes = std::to_wstring(MinutesLeft(seconds)) + L" min left";
    if (kind == Kind::Break) return L"Lean Launcher - break, " + minutes;
    return std::wstring(L"Lean Launcher - ") + kTomato + L" " + minutes + (label.empty() ? L"" : L": " + label);
}

struct State {
    Kind kind = Kind::Focus;
    unsigned long long endTicks = 0;  // absolute UTC FILETIME, so sleep/clock changes don't matter
    int minutes = 0;
    std::wstring label;
};

// "focus|<endTicks>|<minutes>|<label>" - the label is last, so it may contain '|'.
inline std::wstring EncodeState(const State& state) {
    return std::wstring(state.kind == Kind::Focus ? L"focus" : L"break") + L"|" + std::to_wstring(state.endTicks) +
        L"|" + std::to_wstring(state.minutes) + L"|" + state.label;
}

inline std::optional<State> DecodeState(std::wstring_view text) {
    const size_t a = text.find(L'|');
    const size_t b = a == std::wstring_view::npos ? a : text.find(L'|', a + 1);
    const size_t c = b == std::wstring_view::npos ? b : text.find(L'|', b + 1);
    if (c == std::wstring_view::npos) return std::nullopt;
    State state;
    const std::wstring_view kind = text.substr(0, a);
    if (kind == L"focus") state.kind = Kind::Focus;
    else if (kind == L"break") state.kind = Kind::Break;
    else return std::nullopt;
    const std::wstring ticks(text.substr(a + 1, b - a - 1));
    const std::wstring minutes(text.substr(b + 1, c - b - 1));
    wchar_t* end = nullptr;
    state.endTicks = std::wcstoull(ticks.c_str(), &end, 10);
    if (ticks.empty() || *end) return std::nullopt;
    state.minutes = static_cast<int>(std::wcstol(minutes.c_str(), &end, 10));
    if (minutes.empty() || *end || state.minutes < 1 || state.minutes > kMaxMinutes) return std::nullopt;
    state.label.assign(text.substr(c + 1));
    return state;
}

enum class EndReason { Finished, Stopped, EndedWhileAsleep, EndedWhileClosed };

inline bool ShouldLog(Kind kind, EndReason reason, bool logEnabled) {
    return logEnabled && kind == Kind::Focus && reason == EndReason::Finished;
}

inline std::wstring LogText(int minutes, const std::wstring& label) {
    return std::wstring(kTomato) + L" " + std::to_wstring(minutes) + L" min" + (label.empty() ? L"" : L" - " + label);
}

// Appends under `heading` only if the note already exists. A missing daily
// note is never created here - its template would be skipped, and a
// background timer must never go through the "create via Obsidian" path.
inline bool AppendToExistingNote(const std::wstring& notePath, std::wstring_view text, const std::wstring& heading) {
    std::error_code ec;
    if (notePath.empty() || !std::filesystem::is_regular_file(notePath, ec)) return false;
    return obsidian::AppendLogEntry(notePath, text, heading);
}

} // namespace pomodoro
} // namespace leanlauncher
