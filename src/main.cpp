#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <imm.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "resource.h"
#include "search.h"
#include "settings.h"
#include "updates.h"
#include "file_index.h"
#include "calculator.h"

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;
using takeoff::AppCategory;
using takeoff::MatchScore;
using takeoff::Normalize;
using takeoff::ScoreApp;
using takeoff::ScoreFile;
using takeoff::SearchInput;
using takeoff::Settings;
using takeoff::FileIndex;
using takeoff::kFilesReadyMessage;

namespace {

#if defined(TAKEOFF_UI_TEST) || defined(QUICKLAUNCH_UI_TEST)
constexpr bool kUiTest = true;
constexpr wchar_t kWindowClass[] = L"TakeoffTestWindow";
constexpr wchar_t kMutexName[] = L"Local\\Takeoff.UiTest";
#else
constexpr bool kUiTest = false;
constexpr wchar_t kWindowClass[] = L"TakeoffWindow";
constexpr wchar_t kMutexName[] = L"Local\\Takeoff.SingleInstance";
#endif
constexpr int kHotkeyId = 1;
constexpr UINT kAppsReadyMessage = WM_APP + 1;
constexpr UINT kShowLauncherMessage = WM_APP + 2;
constexpr UINT kTrayMessage = WM_APP + 3;
constexpr UINT kIconReadyMessage = WM_APP + 4;
constexpr UINT kUpdateCheckCompletedMessage = WM_APP + 5;
constexpr UINT kExitLauncherMessage = WM_APP + 6;
constexpr UINT kShellNotifyMessage = WM_APP + 7;
constexpr UINT_PTR kCaretTimer = 1;
constexpr UINT_PTR kHotkeyTimer = 2;
constexpr UINT_PTR kRenderRetryTimer = 3;
constexpr UINT_PTR kTrimTimer = 4;
constexpr UINT_PTR kUpdateCheckTimer = 5;

struct AppEntry {
    std::wstring name;
    std::wstring normalizedName;
    std::wstring path;
    AppCategory category = AppCategory::Application;
    std::vector<std::wstring> aliases;
    std::wstring iconPath;
    std::wstring parameters;
};

struct RankedResult {
    size_t appIndex;
    int score;
};

int ScaleForDpi(int value, UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

bool IsLaunchableFile(const fs::path& path) {
    return takeoff::IsLaunchableExtension(path.extension().wstring());
}

std::wstring ExpandEnv(const wchar_t* path) {
    wchar_t expanded[MAX_PATH * 2]{};
    DWORD ret = ExpandEnvironmentStringsW(path, expanded, static_cast<DWORD>(std::size(expanded)));
    if (ret > 0 && ret <= std::size(expanded)) {
        return std::wstring(expanded);
    }
    return {};
}

void ScanDirectoryBounded(const fs::path& root, std::vector<AppEntry>& apps,
    int maxDepth = 4, const std::vector<std::wstring>& skipDirs = {}, bool exeOnly = false) {
    std::error_code error;
    if (!fs::exists(root, error)) return;
    fs::recursive_directory_iterator iterator(
        root, fs::directory_options::skip_permission_denied, error);
    const fs::recursive_directory_iterator end;

    auto shouldSkipDir = [&](const fs::path& dirPath) {
        std::wstring filename = dirPath.filename().wstring();
        if (filename.empty()) return false;
        if (filename[0] == L'.') return true;

        std::wstring lower = filename;
        std::transform(lower.begin(), lower.end(), lower.begin(),
            [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });

        if (lower == L"temp" || lower == L"tmp" || lower == L"packages" ||
            lower == L"package cache" || lower == L"node_modules" ||
            lower == L"crashdumps" || lower == L"stardock" ||
            lower == L"start10ctrlpnl" || lower == L"start11ctrlpnl" ||
            lower == L"start8ctrlpnl") {
            return true;
        }
        if (lower.size() >= 8 && lower.compare(lower.size() - 8, 8, L"-updater") == 0) {
            return true;
        }
        if (lower.size() >= 8 && lower.compare(lower.size() - 8, 8, L"_updater") == 0) {
            return true;
        }
        for (const auto& skip : skipDirs) {
            std::wstring skipLower = skip;
            std::transform(skipLower.begin(), skipLower.end(), skipLower.begin(),
                [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
            if (lower == skipLower) return true;
        }
        return false;
    };

    while (iterator != end) {
        if (error) {
            error.clear();
            iterator.increment(error);
            continue;
        }
        const auto& entry = *iterator;
        if (entry.is_directory(error)) {
            if (iterator.depth() >= maxDepth || shouldSkipDir(entry.path())) {
                iterator.disable_recursion_pending();
            }
            iterator.increment(error);
            continue;
        }
        if (entry.is_regular_file(error)) {
            const auto ext = entry.path().extension().wstring();
            const bool match = exeOnly
                ? (_wcsicmp(ext.c_str(), L".exe") == 0)
                : takeoff::IsLaunchableExtension(ext);
            if (match) {
                std::wstring name = entry.path().stem().wstring();
                if (!name.empty() && !takeoff::IsUninstaller(name) && !takeoff::IsHelperBinary(name)) {
                    std::wstring norm = Normalize(name);
                    if (norm == L"app" || norm == L"launcher" || norm == L"main" || norm == L"run") {
                        std::wstring parentName = entry.path().parent_path().filename().wstring();
                        if (!parentName.empty() && !takeoff::IsHelperBinary(parentName) && !takeoff::IsUninstaller(parentName)) {
                            std::wstring parentNorm = Normalize(parentName);
                            apps.push_back({
                                std::move(parentName),
                                std::move(parentNorm),
                                entry.path().wstring(),
                                AppCategory::Application,
                                {std::move(norm)}
                            });
                            iterator.increment(error);
                            continue;
                        }
                    }
                    apps.push_back({std::move(name), std::move(norm), entry.path().wstring()});
                }
            }
        }
        iterator.increment(error);
    }
}

void ScanDirectory(const fs::path& root, std::vector<AppEntry>& apps) {
    ScanDirectoryBounded(root, apps, 8, {});
}

void ScanAppsFolder(std::vector<AppEntry>& apps) {
    PIDLIST_ABSOLUTE rawAppsFolderId = nullptr;
    if (FAILED(SHGetKnownFolderIDList(
            FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr, &rawAppsFolderId)) || !rawAppsFolderId) {
        return;
    }
    takeoff::UniquePidl appsFolderId(rawAppsFolderId);

    ComPtr<IShellFolder> appsFolder;
    if (FAILED(SHBindToObject(
            nullptr, appsFolderId.get(), nullptr, IID_PPV_ARGS(&appsFolder))) || !appsFolder) {
        return;
    }

    ComPtr<IEnumIDList> enumerator;
    if (SUCCEEDED(appsFolder->EnumObjects(
            nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS | SHCONTF_STORAGE | SHCONTF_FASTITEMS, &enumerator)) && enumerator) {
        PITEMID_CHILD rawChild = nullptr;
        while (enumerator->Next(1, &rawChild, nullptr) == S_OK && rawChild) {
            takeoff::UniquePidl child(rawChild);

            STRRET displayNameResult{};
            wchar_t displayName[MAX_PATH]{};
            if (SUCCEEDED(appsFolder->GetDisplayNameOf(
                    child.get(), SHGDN_NORMAL, &displayNameResult))) {
                StrRetToBufW(&displayNameResult, child.get(), displayName, MAX_PATH);
                takeoff::FreeStrRet(displayNameResult);

                if (displayName[0] == L'@' || wcsstr(displayName, L"ms-resource:") == displayName) {
                    wchar_t resolved[MAX_PATH]{};
                    if (SUCCEEDED(SHLoadIndirectString(displayName, resolved, MAX_PATH, nullptr)) && resolved[0] != L'\0') {
                        wcsncpy_s(displayName, resolved, _TRUNCATE);
                    }
                }

                if (displayName[0] == L'@' || wcsstr(displayName, L"ms-resource:") == displayName || displayName[0] == L'\0') {
                    continue;
                }

                std::wstring parsingName;
                if (takeoff::ResolveShellItemParsingName(appsFolderId.get(), appsFolder.Get(), child.get(), parsingName)) {
                    std::wstring fullPath = takeoff::FormatAppsFolderPath(parsingName);

                    std::wstring name(displayName);
                    while (!name.empty() && (name.back() == L' ' || name.back() == L'\t')) name.pop_back();

                    if (!name.empty() && !takeoff::IsUninstaller(name) && !takeoff::IsHelperBinary(name)) {
                        apps.push_back({
                            std::move(name), Normalize(displayName),
                            std::move(fullPath),
                        });
                    }
                }
            }
        }
    }
}

void AddSystemItems(std::vector<AppEntry>& apps) {
    wchar_t systemDirectory[MAX_PATH]{};
    GetSystemDirectoryW(systemDirectory, MAX_PATH);
    const std::wstring sysDir(systemDirectory);
    wchar_t windowsDirectory[MAX_PATH]{};
    GetWindowsDirectoryW(windowsDirectory, MAX_PATH);
    const std::wstring winDir(windowsDirectory);

    struct SystemItemDef {
        const wchar_t* name;
        std::wstring path;
        std::wstring iconPath;
        std::initializer_list<const wchar_t*> aliases;
        bool isFile;
        std::wstring parameters = {};
    };

    const SystemItemDef items[] = {
        // --- Windows Settings ---
        {L"Settings", L"ms-settings:", sysDir + L"\\control.exe",
            {L"settings", L"preferences", L"options", L"config", L"control panel"}, false},
        {L"Windows Update", L"ms-settings:windowsupdate", sysDir + L"\\control.exe",
            {L"update", L"windows update", L"patch", L"check for updates", L"upgrade", L"wu"}, false},
        {L"Display Settings", L"ms-settings:display", sysDir + L"\\control.exe",
            {L"display", L"monitor", L"screen", L"resolution", L"scale", L"brightness", L"refresh rate", L"hdr"}, false},
        {L"Sound Settings", L"ms-settings:sound", sysDir + L"\\control.exe",
            {L"sound", L"audio", L"volume", L"speaker", L"microphone", L"headphones", L"output device"}, false},
        {L"Bluetooth & Devices", L"ms-settings:bluetooth", sysDir + L"\\control.exe",
            {L"bluetooth", L"bt", L"pair device", L"wireless", L"connect"}, false},
        {L"Wi-Fi Settings", L"ms-settings:network-wifi", sysDir + L"\\control.exe",
            {L"wifi", L"wi fi", L"wireless", L"wlan", L"internet", L"network"}, false},
        {L"Network Status", L"ms-settings:network-status", sysDir + L"\\control.exe",
            {L"network", L"internet", L"ethernet", L"lan", L"ip address", L"connection status"}, false},
        {L"Installed Apps", L"ms-settings:appsfeatures", sysDir + L"\\control.exe",
            {L"apps", L"installed apps", L"apps and features", L"uninstall", L"remove programs", L"add remove"}, false},
        {L"Default Apps", L"ms-settings:defaultapps", sysDir + L"\\control.exe",
            {L"default apps", L"default browser", L"file associations", L"open with"}, false},
        {L"Taskbar Settings", L"ms-settings:taskbar", sysDir + L"\\control.exe",
            {L"taskbar", L"taskbar settings", L"system tray", L"notification area"}, false},
        {L"Notifications", L"ms-settings:notifications", sysDir + L"\\control.exe",
            {L"notifications", L"focus assist", L"do not disturb", L"alerts"}, false},
        {L"Power & Battery", L"ms-settings:powersleep", sysDir + L"\\control.exe",
            {L"power", L"battery", L"sleep", L"energy saver", L"screen timeout"}, false},
        {L"Storage Settings", L"ms-settings:storagesense", sysDir + L"\\control.exe",
            {L"storage", L"disk space", L"free space", L"storage sense", L"clean disk"}, false},
        {L"Personalization / Background", L"ms-settings:personalization-background", sysDir + L"\\control.exe",
            {L"background", L"wallpaper", L"desktop background", L"personalization", L"theme"}, false},
        {L"Colors & Dark Mode", L"ms-settings:personalization-colors", sysDir + L"\\control.exe",
            {L"dark mode", L"light mode", L"accent color", L"colors", L"theme color"}, false},
        {L"Lock Screen", L"ms-settings:lockscreen", sysDir + L"\\control.exe",
            {L"lock screen", L"screensaver", L"lockscreen"}, false},
        {L"Date & Time", L"ms-settings:dateandtime", sysDir + L"\\control.exe",
            {L"date", L"time", L"clock", L"timezone", L"set time", L"ntp"}, false},
        {L"Sign-in Options", L"ms-settings:signinoptions", sysDir + L"\\control.exe",
            {L"sign in", L"pin", L"password", L"windows hello", L"fingerprint", L"face recognition"}, false},
        {L"Windows Security", L"ms-settings:windowsdefender", sysDir + L"\\control.exe",
            {L"security", L"windows security", L"defender", L"antivirus", L"virus protection", L"firewall"}, false},
        {L"Privacy & Security", L"ms-settings:privacy", sysDir + L"\\control.exe",
            {L"privacy", L"permissions", L"camera access", L"microphone access"}, false},
        {L"Printers & Scanners", L"ms-settings:printers", sysDir + L"\\control.exe",
            {L"printers", L"scanners", L"print", L"add printer"}, false},
        {L"Mouse Settings", L"ms-settings:mousetouchpad", sysDir + L"\\control.exe",
            {L"mouse", L"pointer", L"cursor speed", L"sensitivity"}, false},
        {L"Touchpad Settings", L"ms-settings:devices-touchpad", sysDir + L"\\control.exe",
            {L"touchpad", L"trackpad", L"gestures"}, false},
        {L"Keyboard & Typing", L"ms-settings:typing", sysDir + L"\\control.exe",
            {L"typing", L"keyboard", L"autocorrect", L"spell check"}, false},
        {L"Accessibility", L"ms-settings:easeofaccess-display", sysDir + L"\\control.exe",
            {L"accessibility", L"ease of access", L"text size", L"magnifier", L"high contrast"}, false},
        {L"Startup Apps", L"ms-settings:startupapps", sysDir + L"\\control.exe",
            {L"startup", L"startup apps", L"boot apps", L"autostart"}, false},

        // --- Windows Utilities ---
        {L"Task Manager", sysDir + L"\\taskmgr.exe", {},
            {L"taskmgr", L"task manager", L"processes", L"kill", L"end task", L"performance", L"cpu", L"ram", L"memory", L"tm"}, true},
        {L"Control Panel", sysDir + L"\\control.exe", {},
            {L"control panel", L"control", L"cpl", L"settings", L"cp"}, true},
        {L"Command Prompt", sysDir + L"\\cmd.exe", {},
            {L"cmd", L"command prompt", L"cli", L"shell", L"terminal", L"console", L"dos"}, true},
        {L"Windows PowerShell", sysDir + L"\\WindowsPowerShell\\v1.0\\powershell.exe", {},
            {L"powershell", L"ps", L"shell", L"terminal", L"cli"}, true},
        {L"Registry Editor", winDir + L"\\regedit.exe", {},
            {L"regedit", L"registry editor", L"registry", L"regedt32"}, true},
        {L"Disk Cleanup", sysDir + L"\\cleanmgr.exe", {},
            {L"cleanmgr", L"disk cleanup", L"clean disk", L"free space", L"temp files"}, true},
        {L"Snipping Tool", sysDir + L"\\SnippingTool.exe", {},
            {L"snipping tool", L"snip", L"screenshot", L"capture", L"screen clip"}, true},
        {L"Paint", sysDir + L"\\mspaint.exe", {},
            {L"paint", L"mspaint", L"draw", L"image editor", L"bitmap"}, true},
        {L"Notepad", winDir + L"\\notepad.exe", {},
            {L"notepad", L"text editor", L"txt", L"notes"}, true},
        {L"Calculator", sysDir + L"\\calc.exe", {},
            {L"calc", L"calculator", L"math", L"compute"}, true},
        {L"Character Map", sysDir + L"\\charmap.exe", {},
            {L"charmap", L"character map", L"symbols", L"unicode", L"special characters"}, true},
        {L"Remote Desktop Connection", sysDir + L"\\mstsc.exe", {},
            {L"mstsc", L"remote desktop", L"rdp", L"rdc"}, true},
        {L"Volume Mixer", sysDir + L"\\sndvol.exe", {},
            {L"sndvol", L"volume mixer", L"audio mixer", L"sound levels"}, true},
        {L"DirectX Diagnostic Tool", sysDir + L"\\dxdiag.exe", {},
            {L"dxdiag", L"directx", L"gpu", L"graphics info"}, true},
        {L"System Information", sysDir + L"\\msinfo32.exe", {},
            {L"msinfo32", L"system information", L"sysinfo", L"hardware info", L"specs", L"si"}, true},
        {L"Resource Monitor", sysDir + L"\\resmon.exe", {},
            {L"resmon", L"resource monitor", L"disk activity", L"network activity", L"memory"}, true},
        {L"Quick Assist", sysDir + L"\\quickassist.exe", {},
            {L"quick assist", L"quickassist", L"remote help"}, true},
        {L"Magnifier", sysDir + L"\\magnify.exe", {},
            {L"magnify", L"magnifier", L"zoom"}, true},
        {L"On-Screen Keyboard", sysDir + L"\\osk.exe", {},
            {L"osk", L"on screen keyboard", L"virtual keyboard"}, true},

        // --- Management Tools ---
        {L"Services", sysDir + L"\\services.msc", {},
            {L"services", L"services.msc", L"background services", L"daemon"}, true},
        {L"Device Manager", sysDir + L"\\devmgmt.msc", {},
            {L"devmgmt", L"device manager", L"devmgmt.msc", L"hardware", L"drivers", L"ports", L"usb", L"dm"}, true},
        {L"Disk Management", sysDir + L"\\diskmgmt.msc", {},
            {L"diskmgmt", L"disk management", L"diskmgmt.msc", L"partition", L"volumes", L"format drive"}, true},
        {L"Event Viewer", sysDir + L"\\eventvwr.msc", {},
            {L"eventvwr", L"event viewer", L"eventvwr.msc", L"logs", L"error logs", L"system log"}, true},
        {L"Computer Management", sysDir + L"\\compmgmt.msc", {},
            {L"compmgmt", L"computer management", L"compmgmt.msc", L"admin tools"}, true},
        {L"Group Policy Editor", sysDir + L"\\gpedit.msc", {},
            {L"gpedit", L"group policy", L"gpedit.msc", L"policy editor", L"gpe"}, true},
        {L"Local Security Policy", sysDir + L"\\secpol.msc", {},
            {L"secpol", L"security policy", L"secpol.msc"}, true},
        {L"Task Scheduler", sysDir + L"\\taskschd.msc", {},
            {L"taskschd", L"task scheduler", L"taskschd.msc", L"scheduled tasks", L"cron"}, true},
        {L"Windows Defender Firewall", sysDir + L"\\wf.msc", {},
            {L"wf.msc", L"firewall", L"windows firewall", L"advanced firewall", L"firewall rules"}, true},
        {L"System Configuration", sysDir + L"\\msconfig.exe", {},
            {L"msconfig", L"system configuration", L"boot", L"safe mode"}, true},
        {L"Environment Variables", sysDir + L"\\rundll32.exe", sysDir + L"\\sysdm.cpl",
            {L"env", L"environment variables", L"sysdm.cpl", L"system properties", L"path"}, true, L"sysdm.cpl,EditEnvironmentVariables"},
        {L"Network Connections", sysDir + L"\\control.exe", sysDir + L"\\ncpa.cpl",
            {L"ncpa.cpl", L"network connections", L"adapters", L"ethernet", L"wifi adapter"}, true, L"ncpa.cpl"},
        {L"Programs and Features", sysDir + L"\\control.exe", sysDir + L"\\appwiz.cpl",
            {L"appwiz.cpl", L"programs and features", L"uninstall", L"add remove programs"}, true, L"appwiz.cpl"},
        {L"Power Options", sysDir + L"\\control.exe", sysDir + L"\\powercfg.cpl",
            {L"powercfg.cpl", L"power options", L"power plan", L"sleep settings"}, true, L"powercfg.cpl"},
    };

    for (const auto& def : items) {
        if (def.isFile) {
            std::error_code ec;
            const std::wstring& checkPath = !def.iconPath.empty() ? def.iconPath : def.path;
            if (!fs::exists(checkPath, ec)) continue;
        }

        std::wstring name(def.name);
        std::wstring norm = Normalize(name);
        std::wstring pathStr = def.path;
        std::wstring iconStr = def.iconPath;
        std::wstring paramsStr = def.parameters;

        std::vector<std::wstring> aliasVec;
        for (const auto* a : def.aliases) {
            aliasVec.push_back(Normalize(a));
        }

        auto existing = std::find_if(apps.begin(), apps.end(), [&](const AppEntry& e) {
            return e.normalizedName == norm;
        });
        if (existing != apps.end()) {
            existing->category = AppCategory::System;
            existing->path = std::move(pathStr);
            existing->parameters = std::move(paramsStr);
            for (auto& a : aliasVec) {
                if (std::find(existing->aliases.begin(), existing->aliases.end(), a) == existing->aliases.end()) {
                    existing->aliases.push_back(std::move(a));
                }
            }
            if (!iconStr.empty()) {
                existing->iconPath = std::move(iconStr);
            }
        } else {
            apps.push_back({
                std::move(name),
                std::move(norm),
                std::move(pathStr),
                AppCategory::System,
                std::move(aliasVec),
                std::move(iconStr),
                std::move(paramsStr)
            });
        }
    }
}

std::vector<AppEntry> BuildAppIndex() {
    std::vector<AppEntry> apps;
    if constexpr (kUiTest) {
        wchar_t systemDirectory[MAX_PATH]{};
        GetSystemDirectoryW(systemDirectory, MAX_PATH);
        const std::wstring sysDir(systemDirectory);
        for (const wchar_t* name : {L"Calculator", L"Calendar", L"Camera", L"Clock",
                L"File Explorer", L"Firefox", L"Microsoft Edge", L"Notepad", L"Paint",
                L"Photos", L"Settings", L"Visual Studio Code", L"Windows Terminal"}) {
            // Known system icons exercise WIC decoding without indexing personal apps.
            const wchar_t* icon = name == std::wstring_view(L"Calculator") ? L"\\calc.exe"
                : name == std::wstring_view(L"Notepad") ? L"\\notepad.exe" : L"\\control.exe";
            const bool isSys = (name == std::wstring_view(L"Calculator") ||
                                name == std::wstring_view(L"Notepad") ||
                                name == std::wstring_view(L"Settings") ||
                                name == std::wstring_view(L"Windows Terminal") ||
                                name == std::wstring_view(L"Paint"));
            AppEntry entry{
                name,
                Normalize(name),
                sysDir + icon,
                isSys ? AppCategory::System : AppCategory::Application
            };
            if (name == std::wstring_view(L"Calculator")) {
                entry.aliases = {Normalize(L"calc"), Normalize(L"math")};
            } else if (name == std::wstring_view(L"Visual Studio Code")) {
                entry.aliases = {Normalize(L"vsc"), Normalize(L"vscode"), Normalize(L"code")};
            } else if (name == std::wstring_view(L"Windows Terminal")) {
                entry.aliases = {Normalize(L"wt"), Normalize(L"terminal")};
            }
            apps.push_back(std::move(entry));
        }
        return apps;
    }
    PWSTR knownFolderPath = nullptr;
    // 1. Start Menu (Current user and Common)
    if (SUCCEEDED(SHGetKnownFolderPath(
            FOLDERID_StartMenu, KF_FLAG_DEFAULT, nullptr, &knownFolderPath))) {
        ScanDirectory(knownFolderPath, apps);
        CoTaskMemFree(knownFolderPath);
        knownFolderPath = nullptr;
    }
    if (SUCCEEDED(SHGetKnownFolderPath(
            FOLDERID_Programs, KF_FLAG_DEFAULT, nullptr, &knownFolderPath))) {
        ScanDirectory(knownFolderPath, apps);
        CoTaskMemFree(knownFolderPath);
        knownFolderPath = nullptr;
    }
    if (SUCCEEDED(SHGetKnownFolderPath(
            FOLDERID_CommonStartMenu, KF_FLAG_DEFAULT, nullptr, &knownFolderPath))) {
        ScanDirectory(knownFolderPath, apps);
        CoTaskMemFree(knownFolderPath);
        knownFolderPath = nullptr;
    }
    if (SUCCEEDED(SHGetKnownFolderPath(
            FOLDERID_CommonPrograms, KF_FLAG_DEFAULT, nullptr, &knownFolderPath))) {
        ScanDirectory(knownFolderPath, apps);
        CoTaskMemFree(knownFolderPath);
        knownFolderPath = nullptr;
    }

    // 2. Windows Shell AppsFolder (All Start Screen / UWP / Packaged apps)
    ScanAppsFolder(apps);

    // 3. User & System locations for portable apps and executables:
    // %USERPROFILE%\Desktop and Common Desktop
    if (SUCCEEDED(SHGetKnownFolderPath(
            FOLDERID_Desktop, KF_FLAG_DEFAULT, nullptr, &knownFolderPath))) {
        ScanDirectoryBounded(knownFolderPath, apps, 2);
        CoTaskMemFree(knownFolderPath);
        knownFolderPath = nullptr;
    }
    if (SUCCEEDED(SHGetKnownFolderPath(
            FOLDERID_PublicDesktop, KF_FLAG_DEFAULT, nullptr, &knownFolderPath))) {
        ScanDirectoryBounded(knownFolderPath, apps, 2);
        CoTaskMemFree(knownFolderPath);
        knownFolderPath = nullptr;
    }


    // %USERPROFILE%\Applications
    {
        std::wstring appsPath = ExpandEnv(L"%USERPROFILE%\\Applications");
        if (!appsPath.empty()) {
            ScanDirectoryBounded(appsPath, apps, 3);
        }
    }

    // %LOCALAPPDATA%\Programs
    {
        std::wstring programsPath = ExpandEnv(L"%LOCALAPPDATA%\\Programs");
        if (!programsPath.empty()) {
            ScanDirectoryBounded(programsPath, apps, 3);
        }
    }

    // %LOCALAPPDATA% (skipping Programs since scanned above, and skipping Temp, Packages, Stardock, etc.)
    if (SUCCEEDED(SHGetKnownFolderPath(
            FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &knownFolderPath))) {
        ScanDirectoryBounded(knownFolderPath, apps, 2, {L"Programs", L"Stardock"}, true);
        CoTaskMemFree(knownFolderPath);
        knownFolderPath = nullptr;
    }

    // %PROGRAMDATA% (skipping Microsoft folder where Start Menu was already scanned)
    if (SUCCEEDED(SHGetKnownFolderPath(
            FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &knownFolderPath))) {
        ScanDirectoryBounded(knownFolderPath, apps, 2, {L"Microsoft", L"Stardock"}, true);
        CoTaskMemFree(knownFolderPath);
        knownFolderPath = nullptr;
    }

    std::unordered_set<std::wstring> seenNames;
    std::unordered_set<std::wstring> seenPaths;
    std::vector<AppEntry> uniqueApps;
    uniqueApps.reserve(apps.size() + 70);
    for (auto& app : apps) {
        std::wstring normPath = app.path;
        std::transform(normPath.begin(), normPath.end(), normPath.begin(),
            [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
        if (seenPaths.insert(normPath).second) {
            if (seenNames.insert(app.normalizedName).second) {
                uniqueApps.push_back(std::move(app));
            }
        }
    }
    AddSystemItems(uniqueApps);
    std::sort(uniqueApps.begin(), uniqueApps.end(), [](const AppEntry& left, const AppEntry& right) {
        return CompareStringOrdinal(
            left.name.c_str(), -1, right.name.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    return uniqueApps;
}

struct IconRequest {
    std::wstring path;
    UINT size = 0;
    UINT dpi = 96;
};

struct IconResult {
    std::wstring path;
    UINT size = 0;
    ComPtr<IWICBitmapSource> source;
};

// Detects whether a 256x256 image returned by the shell is actually a synthetic
// thumbnail plate (with an outer border box) around a small 32px/48px icon.
bool IsBoxedThumbnail(IWICBitmap* bitmap) {
    if (!bitmap) return false;
    UINT width = 0, height = 0;
    if (FAILED(bitmap->GetSize(&width, &height)) || width != 256 || height != 256) {
        return false;
    }
    ComPtr<IWICBitmapLock> lock;
    WICRect rect{0, 0, 256, 256};
    if (FAILED(bitmap->Lock(&rect, WICBitmapLockRead, &lock))) {
        return false;
    }
    UINT bufferSize = 0;
    WICInProcPointer data = nullptr;
    if (FAILED(lock->GetDataPointer(&bufferSize, &data)) || bufferSize < 256 * 256 * 4) {
        return false;
    }

    // Windows Shell synthesizes a 1px border around the 256x256 canvas when wrapping small icons.
    size_t edgeNonZero = 0;
    const BYTE* pixels = data;
    for (int x = 0; x < 256; ++x) {
        if (pixels[(0 * 256 + x) * 4 + 3] > 0) ++edgeNonZero;
        if (pixels[(255 * 256 + x) * 4 + 3] > 0) ++edgeNonZero;
    }
    for (int y = 1; y < 255; ++y) {
        if (pixels[(y * 256 + 0) * 4 + 3] > 0) ++edgeNonZero;
        if (pixels[(y * 256 + 255) * 4 + 3] > 0) ++edgeNonZero;
    }
    if (edgeNonZero <= 100) return false;

    // If it has a perimeter border, check if the interior is mostly empty (< 14,000 non-zero pixels).
    size_t totalNonZero = 0;
    for (int i = 0; i < 256 * 256; ++i) {
        if (pixels[i * 4 + 3] > 0) {
            ++totalNonZero;
            if (totalNonZero >= 14000) return false;
        }
    }
    return true;
}

// Runs on the icon worker thread. Extracts the highest quality shell icon,
// downscales using WIC Fant box filter, and stamps display DPI for 1:1 pixel rendering.
ComPtr<IWICBitmapSource> LoadIconSource(IWICImagingFactory* wic,
    const std::wstring& path, UINT size, UINT dpi) {
    ComPtr<IWICBitmapSource> source;
    ComPtr<IShellItemImageFactory> imageFactory;
    if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr,
            IID_PPV_ARGS(&imageFactory)))) {
        HBITMAP bitmap = nullptr;
        // 1. Try 256x256 jumbo icon first. For apps with high-res icon assets,
        // this supplies the sharpest master source for downscaling.
        if (SUCCEEDED(imageFactory->GetImage({256, 256},
                SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &bitmap)) && bitmap) {
            ComPtr<IWICBitmap> converted;
            if (SUCCEEDED(wic->CreateBitmapFromHBITMAP(bitmap, nullptr,
                    WICBitmapUsePremultipliedAlpha, &converted))) {
                if (!IsBoxedThumbnail(converted.Get())) {
                    source = converted;
                }
            }
            DeleteObject(bitmap);
            bitmap = nullptr;
        }

        // 2. If 256px failed or was a synthetic boxed thumbnail around a small icon,
        // query native standard tiers (48px or 32px) directly without thumbnail framing.
        if (!source) {
            if (FAILED(imageFactory->GetImage({48, 48}, SIIGBF_ICONONLY, &bitmap)) || !bitmap) {
                if (FAILED(imageFactory->GetImage({32, 32}, SIIGBF_ICONONLY, &bitmap)) || !bitmap) {
                    imageFactory->GetImage({32, 32}, SIIGBF_RESIZETOFIT, &bitmap);
                }
            }
            if (bitmap) {
                ComPtr<IWICBitmap> converted;
                if (SUCCEEDED(wic->CreateBitmapFromHBITMAP(bitmap, nullptr,
                        WICBitmapUsePremultipliedAlpha, &converted))) {
                    source = converted;
                }
                DeleteObject(bitmap);
            }
        }
    }

    // 3. Fallback to SHGetFileInfo if IShellItemImageFactory was unavailable.
    if (!source) {
        SHFILEINFOW info{};
        if (SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info),
                SHGFI_ICON | SHGFI_LARGEICON)) {
            ComPtr<IWICBitmap> converted;
            if (SUCCEEDED(wic->CreateBitmapFromHICON(info.hIcon, &converted))) {
                source = converted;
            }
            DestroyIcon(info.hIcon);
        }
    }
    if (!source) return nullptr;

    // 4. Downscale cleanly to target pixel size with WIC's Fant filter (area-averaging box filter).
    // Stamp display DPI so Direct2D renders 1:1 on the physical pixel grid without fractional resampling blur.
    UINT width = 0, height = 0;
    if (SUCCEEDED(source->GetSize(&width, &height)) && width == size && height == size) {
        ComPtr<IWICBitmap> copy;
        if (SUCCEEDED(wic->CreateBitmapFromSource(source.Get(), WICBitmapCacheOnLoad, &copy))) {
            if (dpi > 0) {
                copy->SetResolution(static_cast<double>(dpi), static_cast<double>(dpi));
            }
            return copy;
        }
        return source;
    }

    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICBitmap> scaled;
    if (SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
        SUCCEEDED(scaler->Initialize(source.Get(), size, size,
            WICBitmapInterpolationModeFant)) &&
        SUCCEEDED(wic->CreateBitmapFromSource(scaler.Get(), WICBitmapCacheOnLoad,
            &scaled))) {
        if (dpi > 0) {
            scaled->SetResolution(static_cast<double>(dpi), static_cast<double>(dpi));
        }
        return scaled;
    }
    return source;
}

#include "launcher.h"

bool ShouldStartMinimized(int nCmdShow) {
    if (nCmdShow == SW_HIDE || nCmdShow == SW_SHOWMINIMIZED ||
        nCmdShow == SW_MINIMIZE || nCmdShow == SW_SHOWMINNOACTIVE) {
        return true;
    }

    STARTUPINFOW si{sizeof(si)};
    GetStartupInfoW(&si);
    if ((si.dwFlags & STARTF_USESHOWWINDOW) &&
        (si.wShowWindow == SW_HIDE ||
         si.wShowWindow == SW_SHOWMINIMIZED ||
         si.wShowWindow == SW_MINIMIZE ||
         si.wShowWindow == SW_SHOWMINNOACTIVE)) {
        return true;
    }

    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        bool minimized = false;
        for (int i = 1; i < argc; ++i) {
            if (takeoff::IsMinimizedSwitch(argv[i])) {
                minimized = true;
                break;
            }
        }
        LocalFree(argv);
        if (minimized) return true;
    }

    return false;
}

bool ShouldReplaceRunningInstance() {
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        bool replace = false;
        for (int i = 1; i < argc; ++i) {
            if (takeoff::IsReplaceSwitch(argv[i])) {
                replace = true;
                break;
            }
        }
        LocalFree(argv);
        if (replace) return true;
    }

    return false;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int nCmdShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const bool startMinimized = ShouldStartMinimized(nCmdShow);
    const bool replaceInstance = ShouldReplaceRunningInstance();

    HANDLE mutex = CreateMutexW(nullptr, FALSE, kMutexName);
    if (mutex == nullptr) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (startMinimized && !replaceInstance) {
            CloseHandle(mutex);
            return 0;
        }

        HWND existing = FindWindowW(kWindowClass, nullptr);
        if (!existing) {
            EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
                wchar_t className[256];
                if (GetClassNameW(hwnd, className, static_cast<int>(std::size(className)))) {
                    if (wcscmp(className, kWindowClass) == 0) {
                        *reinterpret_cast<HWND*>(lParam) = hwnd;
                        return FALSE;
                    }
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&existing));
        }

        if (existing != nullptr) {
            DWORD pid = 0;
            GetWindowThreadProcessId(existing, &pid);
            HANDLE process = pid != 0 ? OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid) : nullptr;
            PostMessageW(existing, kExitLauncherMessage, 0, 0);
            if (process != nullptr) {
                if (WaitForSingleObject(process, 500) == WAIT_TIMEOUT) {
                    TerminateProcess(process, 0);
                    WaitForSingleObject(process, 500);
                }
                CloseHandle(process);
            }
        }
        CloseHandle(mutex);
        mutex = nullptr;

        for (int attempt = 0; attempt < 20; ++attempt) {
            mutex = CreateMutexW(nullptr, FALSE, kMutexName);
            if (mutex != nullptr && GetLastError() != ERROR_ALREADY_EXISTS) {
                break;
            }
            if (mutex != nullptr) {
                CloseHandle(mutex);
                mutex = nullptr;
            }
            Sleep(100);
        }
        if (mutex == nullptr) {
            return 1;
        }
    }

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int exitCode = 1;
    {
        // Release graphics/COM resources before uninitializing the apartment.
        LauncherWindow launcher;
        if (launcher.Create(instance)) {
            const HWND hwnd = launcher.Handle();
            std::thread indexThread([hwnd] {
                const HRESULT indexComResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                auto apps = std::make_unique<std::vector<AppEntry>>();
                try {
                    *apps = BuildAppIndex();
                } catch (const fs::filesystem_error&) {
                    // An inaccessible shell entry must not terminate the launcher.
                }
                if (PostMessageW(hwnd, kAppsReadyMessage, 0,
                        reinterpret_cast<LPARAM>(apps.get()))) {
                    apps.release();
                }
                if (SUCCEEDED(indexComResult)) CoUninitialize();
            });
            if (!startMinimized) {
                PostMessageW(hwnd, kShowLauncherMessage, 0, 0);
            }
            MSG message{};
            BOOL status = 0;
            while ((status = GetMessageW(&message, nullptr, 0, 0)) > 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            indexThread.join();
            // A shutdown can overtake the asynchronous index and icon delivery.
            while (PeekMessageW(&message, nullptr, kAppsReadyMessage, kAppsReadyMessage, PM_REMOVE)) {
                delete reinterpret_cast<std::vector<AppEntry>*>(message.lParam);
            }
            while (PeekMessageW(&message, nullptr, kIconReadyMessage, kIconReadyMessage, PM_REMOVE)) {
                delete reinterpret_cast<IconResult*>(message.lParam);
            }
            exitCode = status == -1 ? 1 : 0;
        }
    }
    if (SUCCEEDED(comResult)) CoUninitialize();
    CloseHandle(mutex);
    return exitCode;
}
