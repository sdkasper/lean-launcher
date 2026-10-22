#pragma once

#include <windows.h>
#include <dwmapi.h>

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "search.h"

// US-065: switch to an open window by typing ("d <text>"). The pure
// decisions (which windows count, ranking, the row path) are unit-tested; the
// enumeration and the raise are Win32 and are checked by hand (TC-023).
// Nothing here is kept: the list is built per call and dropped by the caller
// (NFR-018).
namespace leanlauncher {
namespace winswitch {

// What the Alt+Tab list asks of a top-level window.
struct WindowTraits {
    bool visible = false;
    bool hasTitle = false;
    bool hasOwner = false;
    bool toolWindow = false;
    bool appWindow = false;
    bool deletedFromTaskList = false;  // the ITaskList_Deleted property is set
    bool cloaked = false;              // hidden by DWM: preloaded Store apps, other virtual desktops
    bool coreWindow = false;
    bool isSelf = false;
};

inline bool IsSwitchable(const WindowTraits& t) {
    return t.visible && t.hasTitle && !t.hasOwner && (!t.toolWindow || t.appWindow) &&
           !t.deletedFromTaskList && !t.cloaked && !t.coreWindow && !t.isSelf;
}

inline bool IsCoreWindowClass(std::wstring_view className) {
    return className == L"Windows.UI.Core.CoreWindow";
}

struct WindowInfo {
    HWND hwnd = nullptr;
    std::wstring title;
    std::wstring processName;  // "notepad", empty when the process cannot be queried
    std::wstring exePath;      // for the row icon, empty when unknown
};

inline std::wstring_view FileNameOf(std::wstring_view path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring_view::npos ? path : path.substr(slash + 1);
}

// "C:\Windows\System32\notepad.exe" -> "notepad".
inline std::wstring ProcessDisplayName(std::wstring_view imagePath) {
    std::wstring_view name = FileNameOf(imagePath);
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring_view::npos && dot > 0) name = name.substr(0, dot);
    return std::wstring(name);
}

// Store apps are drawn inside this host; the real app lives in a child window.
inline bool IsApplicationFrameHost(std::wstring_view imagePath) {
    return _wcsicmp(std::wstring(FileNameOf(imagePath)).c_str(), L"ApplicationFrameHost.exe") == 0;
}

// Indices into `windows`, best match first. An empty filter keeps every window
// in the order given (z-order, the way Alt+Tab shows them).
inline std::vector<size_t> Rank(const std::vector<WindowInfo>& windows, const std::wstring& normalizedFilter) {
    std::vector<size_t> order;
    if (normalizedFilter.empty()) {
        order.resize(windows.size());
        std::iota(order.begin(), order.end(), size_t{0});
        return order;
    }
    std::vector<std::pair<int, size_t>> scored;
    for (size_t i = 0; i < windows.size(); ++i) {
        const int score = (std::max)(takeoff::MatchScore(takeoff::Normalize(windows[i].title), normalizedFilter),
                                     takeoff::MatchScore(takeoff::Normalize(windows[i].processName), normalizedFilter));
        if (score >= 0) scored.emplace_back(score, i);
    }
    std::stable_sort(scored.begin(), scored.end(),
        [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& item : scored) order.push_back(item.second);
    return order;
}

// A result row names its window by handle, so nothing else has to be stored.
inline constexpr wchar_t kPathPrefix[] = L"hwnd:";

inline std::wstring PathForWindow(HWND hwnd) {
    return kPathPrefix + std::to_wstring(static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hwnd)));
}

inline bool WindowFromPath(const std::wstring& path, HWND& hwnd) {
    constexpr size_t prefixLength = std::size(kPathPrefix) - 1;
    if (path.size() <= prefixLength || path.compare(0, prefixLength, kPathPrefix) != 0) return false;
    uintptr_t value = 0;
    for (size_t i = prefixLength; i < path.size(); ++i) {
        const wchar_t ch = path[i];
        if (ch < L'0' || ch > L'9') return false;
        const uintptr_t digit = static_cast<uintptr_t>(ch - L'0');
        if (value > (UINTPTR_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    if (value == 0) return false;
    hwnd = reinterpret_cast<HWND>(value);
    return true;
}

namespace detail {

inline std::wstring ImagePathOf(DWORD pid, std::unordered_map<DWORD, std::wstring>& cache) {
    const auto found = cache.find(pid);
    if (found != cache.end()) return found->second;
    std::wstring image;
    if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buffer[1024]{};
        DWORD size = static_cast<DWORD>(std::size(buffer));
        if (QueryFullProcessImageNameW(process, 0, buffer, &size)) image.assign(buffer, size);
        CloseHandle(process);
    }
    cache.emplace(pid, image);
    return image;
}

// The process of the first child window that belongs to someone else than the frame.
inline DWORD ProcessBehindFrame(HWND frame, DWORD frameProcess) {
    struct Search {
        DWORD frameProcess;
        DWORD found;
    } search{frameProcess, 0};
    EnumChildWindows(frame, [](HWND child, LPARAM lParam) -> BOOL {
        Search& state = *reinterpret_cast<Search*>(lParam);
        DWORD pid = 0;
        GetWindowThreadProcessId(child, &pid);
        if (pid == 0 || pid == state.frameProcess) return TRUE;
        state.found = pid;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

}  // namespace detail

// Every window Alt+Tab would list, topmost first, except `self`. Built fresh on
// each call; the caller drops it when the results are rebuilt.
inline std::vector<WindowInfo> EnumerateWindows(HWND self) {
    struct State {
        std::vector<WindowInfo> windows;
        HWND self;
        std::unordered_map<DWORD, std::wstring> images;  // per call: a process shows several windows
    } state{{}, self, {}};
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        State& st = *reinterpret_cast<State*>(lParam);
        WindowTraits traits;
        traits.visible = IsWindowVisible(hwnd) != FALSE;
        if (!traits.visible) return TRUE;
        traits.isSelf = hwnd == st.self;
        traits.hasOwner = GetWindow(hwnd, GW_OWNER) != nullptr;
        const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        traits.toolWindow = (exStyle & WS_EX_TOOLWINDOW) != 0;
        traits.appWindow = (exStyle & WS_EX_APPWINDOW) != 0;
        traits.deletedFromTaskList = GetPropW(hwnd, L"ITaskList_Deleted") != nullptr;
        DWORD cloaked = 0;
        traits.cloaked = SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
        wchar_t className[256]{};
        GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
        traits.coreWindow = IsCoreWindowClass(className);
        const int titleLength = GetWindowTextLengthW(hwnd);
        traits.hasTitle = titleLength > 0;
        if (!IsSwitchable(traits)) return TRUE;

        std::wstring title(static_cast<size_t>(titleLength) + 1, L'\0');
        title.resize(static_cast<size_t>(GetWindowTextW(hwnd, title.data(), titleLength + 1)));
        if (title.empty()) return TRUE;

        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        std::wstring image = detail::ImagePathOf(pid, st.images);
        if (IsApplicationFrameHost(image)) {
            if (const DWORD real = detail::ProcessBehindFrame(hwnd, pid)) image = detail::ImagePathOf(real, st.images);
        }
        st.windows.push_back({hwnd, std::move(title), ProcessDisplayName(image), std::move(image)});
        return TRUE;
    }, reinterpret_cast<LPARAM>(&state));
    return std::move(state.windows);
}

// Brings `target` to the foreground. If Windows refuses, borrows the foreground
// thread's input state for one more try, and flashes the taskbar button if that
// fails too. Returns whether the window now has the foreground. Never blocks.
inline bool BringWindowToForeground(HWND target) {
    if (SetForegroundWindow(target)) return true;
    const HWND front = GetForegroundWindow();
    const DWORD frontThread = front ? GetWindowThreadProcessId(front, nullptr) : 0;
    const DWORD thisThread = GetCurrentThreadId();
    const bool attached = frontThread != 0 && frontThread != thisThread &&
        AttachThreadInput(thisThread, frontThread, TRUE) != FALSE;
    // Asynchronous, so a hung target cannot stall this thread.
    SetWindowPos(target, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    const bool raised = SetForegroundWindow(target) != FALSE;
    if (attached) AttachThreadInput(thisThread, frontThread, FALSE);
    if (!raised) {
        FLASHWINFO flash{sizeof(flash), target, FLASHW_ALL | FLASHW_TIMERNOFG, 3, 0};
        FlashWindowEx(&flash);
    }
    return raised;
}

// Restores a minimized window and raises it, without ever waiting on the
// target for long (SC_RESTORE with a 200 ms timeout, then an asynchronous
// restore). Returns false when the window is gone or could not be raised.
inline bool ActivateWindow(HWND target) {
    if (!target || !IsWindow(target)) return false;
    if (IsIconic(target)) {
        // ShowWindow would block this thread while the target is not responding,
        // so the restore goes through a message with a timeout.
        DWORD_PTR ignored = 0;
        SendMessageTimeoutW(target, WM_SYSCOMMAND, SC_RESTORE, 0, SMTO_ABORTIFHUNG, 200, &ignored);
        if (IsIconic(target)) ShowWindowAsync(target, SW_RESTORE);
    }
    return BringWindowToForeground(target);
}

}  // namespace winswitch
}  // namespace leanlauncher
