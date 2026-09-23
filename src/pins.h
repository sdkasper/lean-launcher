#pragma once

#include <cstddef>
#include <cwchar>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Pinned search results (US-024): the pure list logic, kept free of any
// window/registry state so it can be unit-tested. The launcher owns
// persistence (HKCU\Software\LeanLauncher\Pinned) and maps pins onto its
// result rows.
namespace leanlauncher {
namespace pins {

// At most 5 pins, so at least 3 of the launcher's 8 visible rows are always
// left for query matches or the recency view.
constexpr size_t kMaxPins = 5;

struct Pin {
    std::wstring path;  // identity: full launch path (apps) or file/folder path
    bool isApp = false;
};

enum class PinResult { Pinned, Unpinned, AtCap };

// Pin identity: case-insensitive, like Windows paths.
inline bool SamePath(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() && _wcsnicmp(a.data(), b.data(), a.size()) == 0;
}

// Index of `path` in `pins`, or -1.
inline int FindPin(const std::vector<Pin>& pins, std::wstring_view path) {
    for (size_t i = 0; i < pins.size(); ++i) {
        if (SamePath(pins[i].path, path)) return static_cast<int>(i);
    }
    return -1;
}

// Unpins `path` if it's pinned; otherwise pins it as the newest (first)
// entry - unless the list is full, in which case nothing changes. Refusing
// beats silently evicting the oldest pin.
inline PinResult TogglePin(std::vector<Pin>& pins, const std::wstring& path, bool isApp) {
    const int existing = FindPin(pins, path);
    if (existing >= 0) {
        pins.erase(pins.begin() + existing);
        return PinResult::Unpinned;
    }
    if (pins.size() >= kMaxPins) return PinResult::AtCap;
    pins.insert(pins.begin(), Pin{path, isApp});
    return PinResult::Pinned;
}

// Registry value form: "app|<path>" or "file|<path>". '|' is illegal in
// Windows paths, so the first one is always the separator.
inline std::wstring EncodePin(const Pin& pin) {
    return (pin.isApp ? L"app|" : L"file|") + pin.path;
}

inline bool DecodePin(std::wstring_view value, Pin& out) {
    const size_t bar = value.find(L'|');
    if (bar == std::wstring_view::npos || bar + 1 >= value.size()) return false;
    const std::wstring_view kind = value.substr(0, bar);
    if (kind != L"app" && kind != L"file") return false;
    out.isApp = (kind == L"app");
    out.path.assign(value.substr(bar + 1));
    return true;
}

// Moves every entry of `results` whose pinRank(entry) is >= 0 to the front,
// ordered by rank (0 first), keeping all other entries in their original
// relative order. In place and allocation-free (NFR-015): with at most
// kMaxPins pins, the pinned entries fit in a fixed-size scratch array, so
// this is one linear pass plus a block move on every keystroke.
template <typename PinRankFn>
void MovePinnedToFront(std::vector<size_t>& results, PinRankFn pinRank) {
    std::pair<int, size_t> found[kMaxPins];
    size_t foundCount = 0;
    size_t write = 0;
    for (size_t read = 0; read < results.size(); ++read) {
        const size_t entry = results[read];
        const int rank = pinRank(entry);
        if (rank >= 0 && foundCount < kMaxPins) {
            found[foundCount++] = {rank, entry};
        } else {
            results[write++] = entry;
        }
    }
    if (foundCount == 0) return;
    // Insertion sort by rank - at most kMaxPins elements.
    for (size_t i = 1; i < foundCount; ++i) {
        const auto item = found[i];
        size_t j = i;
        while (j > 0 && found[j - 1].first > item.first) {
            found[j] = found[j - 1];
            --j;
        }
        found[j] = item;
    }
    // Shift the unpinned block right by foundCount, then fill the front.
    for (size_t i = write; i > 0; --i) {
        results[i - 1 + foundCount] = results[i - 1];
    }
    for (size_t i = 0; i < foundCount; ++i) {
        results[i] = found[i].second;
    }
}

}  // namespace pins
}  // namespace leanlauncher
