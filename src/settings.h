#pragma once

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <string>
#include <string_view>

namespace takeoff {

constexpr uint16_t kModAlt = 0x0001;
constexpr uint16_t kModControl = 0x0002;
constexpr uint16_t kModShift = 0x0004;

constexpr uint16_t kVkBack = 0x08;
constexpr uint16_t kVkReturn = 0x0D;
constexpr uint16_t kVkSpace = 0x20;
constexpr uint16_t kVkOemPeriod = 0xBE;

struct HotkeyBinding {
    uint16_t modifiers = 0;
    uint16_t key = 0;
    bool disabled = false;

    bool operator==(const HotkeyBinding& other) const {
        if (disabled && other.disabled) return true;
        return !disabled && !other.disabled &&
               modifiers == other.modifiers && key == other.key;
    }
    bool operator!=(const HotkeyBinding& other) const { return !(*this == other); }
};

struct Settings {
    HotkeyBinding launcherHotkey{kModAlt, kVkSpace};
    HotkeyBinding actionsHotkey{kModControl, 'K'};
    HotkeyBinding administratorHotkey{kModControl, 0};
    HotkeyBinding quickLaunchHotkey{kModAlt, 0};
    bool runAtStartup = true;
    bool showTrayIcon = true;
    bool checkForUpdates = true;
    bool enableFileSearch = true;
    bool enableWebSearch = true;
};

inline std::wstring KeyName(uint16_t vk) {
    switch (vk) {
    case 0x08: return L"Backspace";
    case 0x09: return L"Tab";
    case 0x0D: return L"Enter";
    case 0x1B: return L"Esc";
    case 0x20: return L"Space";
    case 0x21: return L"PgUp";
    case 0x22: return L"PgDn";
    case 0x23: return L"End";
    case 0x24: return L"Home";
    case 0x2D: return L"Insert";
    case 0x2E: return L"Delete";
    case 0xBA: return L";";
    case 0xBB: return L"=";
    case 0xBC: return L",";
    case 0xBD: return L"-";
    case 0xBE: return L".";
    case 0xBF: return L"/";
    case 0xC0: return L"`";
    case 0xDB: return L"[";
    case 0xDC: return L"\\";
    case 0xDD: return L"]";
    case 0xDE: return L"'";
    default:
        if (vk >= 'A' && vk <= 'Z') return std::wstring(1, static_cast<wchar_t>(vk));
        if (vk >= '0' && vk <= '9') return std::wstring(1, static_cast<wchar_t>(vk));
        if (vk >= 0x70 && vk <= 0x87) return L"F" + std::to_wstring(vk - 0x70 + 1);
        if (vk >= 0x60 && vk <= 0x69) return L"Num" + std::to_wstring(vk - 0x60);
        return L"Key" + std::to_wstring(vk);
    }
}

inline std::wstring FormatModifiers(uint16_t modifiers) {
    std::wstring result;
    if (modifiers & kModControl) result += L"Ctrl";
    if (modifiers & kModAlt) {
        if (!result.empty()) result += L" + ";
        result += L"Alt";
    }
    if (modifiers & kModShift) {
        if (!result.empty()) result += L" + ";
        result += L"Shift";
    }
    return result;
}

inline std::wstring FormatBinding(const HotkeyBinding& binding) {
    if (binding.disabled) return L"Disabled";
    std::wstring result = FormatModifiers(binding.modifiers);
    if (binding.key) {
        if (!result.empty()) result += L" + ";
        result += KeyName(binding.key);
    }
    return result;
}

inline std::wstring FormatAdminBinding(const HotkeyBinding& binding) {
    if (binding.disabled) return L"Disabled";
    std::wstring result = FormatModifiers(binding.modifiers);
    if (!result.empty()) result += L" + ";
    result += L"Enter";
    return result;
}

inline std::wstring FormatQuickLaunchBinding(const HotkeyBinding& binding) {
    if (binding.disabled) return L"Disabled";
    std::wstring result = FormatModifiers(binding.modifiers);
    if (!result.empty()) result += L" + ";
    result += L"1\u20138";
    return result;
}

inline bool IsReservedInApp(const HotkeyBinding& binding) {
    if (binding.disabled || binding.key == 0) return false;
    if (binding.modifiers == 0) return true;
    if (binding.modifiers == kModControl) {
        switch (binding.key) {
        case 'A': case 'C': case 'V': case 'X': case 'Z': case 'L':
            return true;
        }
    }
    return false;
}

inline bool IsSystemReserved(uint16_t modifiers, uint16_t vk) {
    if (modifiers == kModAlt && (vk == 0x73 || vk == 0x09)) return true;
    if (modifiers == (kModControl | kModShift) && vk == 0x1B) return true;
    return false;
}

inline const wchar_t* HasInternalConflict(int targetRow, const HotkeyBinding& proposed, const Settings& settings) {
    if (proposed.disabled) return nullptr;
    if (targetRow == 0) {
        if (!settings.actionsHotkey.disabled && proposed == settings.actionsHotkey)
            return L"Conflicts with Actions menu shortcut.";
        if (!settings.administratorHotkey.disabled && proposed.key == kVkReturn &&
            proposed.modifiers == settings.administratorHotkey.modifiers)
            return L"Conflicts with Open as administrator shortcut.";
        if (!settings.quickLaunchHotkey.disabled && proposed.key >= '1' && proposed.key <= '8' &&
            proposed.modifiers == settings.quickLaunchHotkey.modifiers)
            return L"Conflicts with Quick launch shortcut.";
    } else if (targetRow == 1) {
        if (!settings.launcherHotkey.disabled && proposed == settings.launcherHotkey)
            return L"Conflicts with Open Takeoff shortcut.";
        if (!settings.administratorHotkey.disabled && proposed.key == kVkReturn &&
            proposed.modifiers == settings.administratorHotkey.modifiers)
            return L"Conflicts with Open as administrator shortcut.";
        if (!settings.quickLaunchHotkey.disabled && proposed.key >= '1' && proposed.key <= '8' &&
            proposed.modifiers == settings.quickLaunchHotkey.modifiers)
            return L"Conflicts with Quick launch shortcut.";
    } else if (targetRow == 2) {
        if (!settings.launcherHotkey.disabled && settings.launcherHotkey.key == kVkReturn &&
            proposed.modifiers == settings.launcherHotkey.modifiers)
            return L"Conflicts with Open Takeoff shortcut.";
        if (!settings.actionsHotkey.disabled && settings.actionsHotkey.key == kVkReturn &&
            proposed.modifiers == settings.actionsHotkey.modifiers)
            return L"Conflicts with Actions menu shortcut.";
    } else if (targetRow == 3) {
        if (!settings.launcherHotkey.disabled && settings.launcherHotkey.key >= '1' &&
            settings.launcherHotkey.key <= '8' && proposed.modifiers == settings.launcherHotkey.modifiers)
            return L"Conflicts with Open Takeoff shortcut.";
        if (!settings.actionsHotkey.disabled && settings.actionsHotkey.key >= '1' &&
            settings.actionsHotkey.key <= '8' && proposed.modifiers == settings.actionsHotkey.modifiers)
            return L"Conflicts with Actions menu shortcut.";
    }
    return nullptr;
}

inline HotkeyBinding MigrateLauncherHotkey(int preset) {
    switch (std::clamp(preset, 0, 3)) {
    case 0: return {kModAlt, kVkSpace};
    case 1: return {kModControl, kVkSpace};
    case 2: return {kModControl | kModAlt, kVkSpace};
    case 3: return {kModControl | kModShift, kVkSpace};
    }
    return {kModAlt, kVkSpace};
}

inline HotkeyBinding MigrateActionsHotkey(int preset) {
    switch (std::clamp(preset, 0, 2)) {
    case 0: return {kModControl, 'K'};
    case 1: return {kModControl, kVkOemPeriod};
    case 2: return {kModControl | kModShift, 'P'};
    }
    return {kModControl, 'K'};
}

inline HotkeyBinding MigrateAdminHotkey(int preset) {
    switch (std::clamp(preset, 0, 3)) {
    case 0: return {kModControl, 0};
    case 1: return {kModShift, 0};
    case 2: return {kModControl | kModShift, 0};
    case 3: return {0, 0, true};
    }
    return {kModControl, 0};
}

inline HotkeyBinding MigrateQuickLaunchHotkey(int preset) {
    switch (std::clamp(preset, 0, 3)) {
    case 0: return {kModAlt, 0};
    case 1: return {kModControl, 0};
    case 2: return {kModControl | kModAlt, 0};
    case 3: return {0, 0, true};
    }
    return {kModAlt, 0};
}

inline bool IsMinimizedSwitch(std::wstring_view arg) {
    if (arg.empty()) return false;
    auto equalsIgnoreCase = [](std::wstring_view a, std::wstring_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (towlower(a[i]) != towlower(b[i])) return false;
        }
        return true;
    };
    return equalsIgnoreCase(arg, L"--minimized") ||
           equalsIgnoreCase(arg, L"-minimized") ||
           equalsIgnoreCase(arg, L"/minimized") ||
           equalsIgnoreCase(arg, L"--startup") ||
           equalsIgnoreCase(arg, L"-startup") ||
           equalsIgnoreCase(arg, L"/startup") ||
           equalsIgnoreCase(arg, L"--hidden") ||
           equalsIgnoreCase(arg, L"-hidden") ||
           equalsIgnoreCase(arg, L"/hidden") ||
           equalsIgnoreCase(arg, L"-m") ||
           equalsIgnoreCase(arg, L"/m");
}

inline bool IsReplaceSwitch(std::wstring_view arg) {
    if (arg.empty()) return false;
    auto equalsIgnoreCase = [](std::wstring_view a, std::wstring_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (towlower(a[i]) != towlower(b[i])) return false;
        }
        return true;
    };
    return equalsIgnoreCase(arg, L"--replace") ||
           equalsIgnoreCase(arg, L"-replace") ||
           equalsIgnoreCase(arg, L"/replace") ||
           equalsIgnoreCase(arg, L"--update") ||
           equalsIgnoreCase(arg, L"-update") ||
           equalsIgnoreCase(arg, L"/update") ||
           equalsIgnoreCase(arg, L"-r") ||
           equalsIgnoreCase(arg, L"/r");
}

} // namespace takeoff

namespace quicklaunch = takeoff;

