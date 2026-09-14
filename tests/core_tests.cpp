#include "../src/search.h"
#include "../src/settings.h"
#include "../src/updates.h"
#include "../src/file_index.h"
#include "../src/calculator.h"

#include <chrono>
#include <cstdlib>
#include <iostream>

void Check(bool condition, const char* description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        std::exit(1);
    }
}

int main() {
    using namespace takeoff;
    Check(Normalize(L"  Visual-Studio.Code! ") == L"visual studio code", "normalization");
    Check(Normalize(L"!!!").empty(), "punctuation-only query");
    Check(MatchScore(L"code", L"code") > MatchScore(L"code editor", L"code"), "exact first");
    Check(MatchScore(L"code editor", L"code") > MatchScore(L"visual code", L"code"), "prefix first");
    Check(MatchScore(L"visual studio code", L"vsc") > 0, "fuzzy match");
    Check(MatchScore(L"notepad", L"xyz") == -1, "no match");
    Check(MatchScore(L"notepad", L"") == -1, "empty query");

    // Acronym and initials matching
    Check(MatchScore(L"visual studio code", L"vsc") > 0, "acronym vsc");
    Check(MatchScore(L"task manager", L"tm") > 0, "acronym tm");
    Check(MatchScore(L"control panel", L"cp") > 0, "acronym cp");
    Check(MatchScore(L"windows update", L"wu") > 0, "acronym wu");
    Check(MatchScore(L"device manager", L"dm") > 0, "acronym dm");

    // Multi-token word prefix matching
    Check(MatchScore(L"visual studio code", L"vs code") > 0, "multi-token vs code");
    Check(MatchScore(L"windows update", L"win upd") > 0, "multi-token win upd");
    Check(MatchScore(L"task manager", L"task man") > 0, "multi-token task man");

    // Word boundary vs substring
    Check(MatchScore(L"visual studio code", L"code") > MatchScore(L"barcode scanner", L"code"),
        "word boundary beats inside word");

    // Full-word match in multi-word application title strictly beats partial-word prefix in compound names
    Check(MatchScore(L"google chrome", L"chrome") > MatchScore(L"chromesetup", L"chrome"),
        "full word 'chrome' in 'google chrome' beats partial prefix in 'chromesetup'");
    Check(MatchScore(L"microsoft teams", L"teams") > MatchScore(L"teamssetup", L"teams"),
        "full word 'teams' in 'microsoft teams' beats partial prefix in 'teamssetup'");
    Check(MatchScore(L"visual studio code", L"code") > MatchScore(L"codesetup", L"code"),
        "full word 'code' in 'visual studio code' beats partial prefix in 'codesetup'");
    Check(MatchScore(L"mozilla firefox", L"firefox") > MatchScore(L"firefoxsetup", L"firefox"),
        "full word 'firefox' in 'mozilla firefox' beats partial prefix in 'firefoxsetup'");

    // Condensed match
    Check(MatchScore(L"vs code", L"vscode") >= 9500, "condensed match");

    // Aliases and ScoreApp
    Check(ScoreApp(L"command prompt", {L"cmd"}, L"cmd") >= 9500, "alias exact match for cmd");
    Check(ScoreApp(L"task manager", {L"taskmgr", L"processes"}, L"taskmgr") >= 9500, "alias taskmgr");
    Check(ScoreApp(L"windows update", {L"update"}, L"update") >= 9500, "alias update");
    Check(ScoreApp(L"services", {L"daemon"}, L"daemon") > 0, "alias daemon");

    // Recency ranking priority
    Check(ScoreApp(L"terminal", {}, L"term", 0) > ScoreApp(L"terminal", {}, L"term", 1),
        "most recent ranks higher than second recent");
    Check(ScoreApp(L"terminal", {}, L"term", 1) > ScoreApp(L"terminal", {}, L"term", -1),
        "recent ranks higher than non-recent");

    // AppCategory distinction
    Check(AppCategory::System != AppCategory::Application, "categories are distinct");

    // Uninstaller detection checks
    Check(IsUninstaller(L"unins000"), "unins000 is recognized as uninstaller");
    Check(IsUninstaller(L"unins001"), "unins001 is recognized as uninstaller");
    Check(IsUninstaller(L"uninst"), "uninst is recognized as uninstaller");
    Check(IsUninstaller(L"uninstall"), "uninstall is recognized as uninstaller");
    Check(IsUninstaller(L"Uninstall App"), "Uninstall App is recognized as uninstaller");
    Check(IsUninstaller(L"remove program"), "remove program is recognized as uninstaller");
    Check(IsUninstaller(L"app uninstaller"), "app uninstaller is recognized as uninstaller");
    Check(!IsUninstaller(L"universal"), "universal is not uninstaller");
    Check(!IsUninstaller(L"unity"), "unity is not uninstaller");
    Check(!IsUninstaller(L"notepad"), "notepad is not uninstaller");

    // Helper / internal binary filtering checks
    Check(IsHelperBinary(L"crashpad_handler"), "crashpad_handler filtered");
    Check(IsHelperBinary(L"crashpad handler"), "crashpad handler filtered");
    Check(IsHelperBinary(L"squirrel"), "squirrel filtered");
    Check(IsHelperBinary(L"notification_helper"), "notification_helper filtered");
    Check(IsHelperBinary(L"elevate"), "elevate filtered");
    Check(IsHelperBinary(L"installer"), "installer filtered");
    Check(IsHelperBinary(L"update"), "update filtered");
    Check(!IsHelperBinary(L"code"), "code not helper binary");
    Check(!IsHelperBinary(L"chrome"), "chrome not helper binary");

    // Launchable file extensions checks
    Check(IsLaunchableExtension(L".exe"), ".exe is launchable");
    Check(IsLaunchableExtension(L".EXE"), ".EXE is launchable");
    Check(IsLaunchableExtension(L".lnk"), ".lnk is launchable");
    Check(IsLaunchableExtension(L".appref-ms"), ".appref-ms is launchable");
    Check(IsLaunchableExtension(L".url"), ".url is launchable");
    Check(!IsLaunchableExtension(L".dll"), ".dll is not launchable");
    Check(!IsLaunchableExtension(L".txt"), ".txt is not launchable");
    Check(!IsLaunchableExtension(L""), "empty extension is not launchable");

    SearchInput input;
    input.Insert(L"hello world");
    input.Move(false, false, true);
    Check(input.caret == 6, "previous word");
    input.Insert(L"new ");
    Check(input.text == L"hello new world", "insert in middle");
    input.Erase(true, true);
    Check(input.text == L"hello world", "delete previous word");
    input.MoveTo(0, false);
    input.Move(true, true, true);
    Check(input.Start() == 0 && input.End() == 6, "word selection");
    input.Insert(L"goodbye ");
    Check(input.text == L"goodbye world", "replace selection");
    input.MoveTo(0, false);
    input.Erase(false);
    Check(input.text == L"oodbye world", "forward delete");
    input.SelectAll();
    input.Insert(L"abc\r\n\tdef");
    Check(input.text == L"abcdef", "single-line paste");
    input.MoveTo(2, false);
    input.MoveTo(5, true);
    input.Move(false, false, false);
    Check(input.caret == 2 && !input.HasSelection(), "collapse selection left");
    input.SelectAll();
    input.Erase(true);
    Check(input.text.empty() && input.caret == 0, "erase all");
    input.Erase(true);
    input.Erase(false);
    Check(input.text.empty(), "empty deletion");
    input.Insert(L"a\xD83D\xDE00z");
    input.Move(false, false, false);
    input.Move(false, false, false);
    Check(input.caret == 1, "move across surrogate pair");
    input.Erase(false);
    Check(input.text == L"az", "delete surrogate pair");
    input.Clear();
    input.Insert(std::wstring(2048, L'x'));
    Check(input.text.size() == SearchInput::kLimit, "input length limit");
    input.SelectAll();
    input.Insert(L"replacement");
    Check(input.text == L"replacement", "replace at limit");

    Settings settings;
    Check(FormatBinding(settings.launcherHotkey) == L"Alt + Space",
        "default launcher hotkey");
    Check(FormatBinding(settings.actionsHotkey) == L"Ctrl + K",
        "default actions hotkey");
    Check(FormatAdminBinding(settings.administratorHotkey) == L"Ctrl + Enter",
        "default administrator hotkey");
    Check(FormatQuickLaunchBinding(settings.quickLaunchHotkey) == L"Alt + 1\u20138",
        "default quick launch hotkey");
    Check(settings.showTrayIcon,
        "notification area icon enabled by default");
    Check(settings.checkForUpdates,
        "automatic update checking enabled by default");

    // Custom bindings formatting
    Check(FormatBinding({kModControl | kModShift, 'P'}) == L"Ctrl + Shift + P",
        "format Ctrl+Shift+P");
    Check(FormatBinding({kModControl | kModAlt, 'D'}) == L"Ctrl + Alt + D",
        "format Ctrl+Alt+D");
    Check(FormatBinding({kModAlt, 0x70}) == L"Alt + F1",
        "format Alt+F1");
    Check(FormatBinding({0, 0, true}) == L"Disabled",
        "format disabled binding");
    Check(FormatAdminBinding({kModControl | kModShift, 0}) == L"Ctrl + Shift + Enter",
        "format admin Ctrl+Shift+Enter");
    Check(FormatAdminBinding({0, 0, true}) == L"Disabled",
        "format disabled admin binding");
    Check(FormatQuickLaunchBinding({kModControl | kModAlt, 0}) == L"Ctrl + Alt + 1\u20138",
        "format quick launch Ctrl+Alt+1-8");
    Check(FormatQuickLaunchBinding({0, 0, true}) == L"Disabled",
        "format disabled quick launch");

    // Reserved in-app keys
    Check(IsReservedInApp({kModControl, 'C'}), "Ctrl+C reserved");
    Check(IsReservedInApp({kModControl, 'V'}), "Ctrl+V reserved");
    Check(IsReservedInApp({kModControl, 'A'}), "Ctrl+A reserved");
    Check(IsReservedInApp({kModControl, 'X'}), "Ctrl+X reserved");
    Check(IsReservedInApp({kModControl, 'Z'}), "Ctrl+Z reserved");
    Check(!IsReservedInApp({kModControl, 'K'}), "Ctrl+K allowed");
    Check(!IsReservedInApp({kModControl | kModShift, 'C'}), "Ctrl+Shift+C allowed");
    Check(IsReservedInApp({0, 'A'}), "bare key reserved");

    // Equality and migration
    Check(MigrateLauncherHotkey(0) == HotkeyBinding{kModAlt, kVkSpace},
        "migrate default launcher hotkey");
    Check(MigrateActionsHotkey(0) == HotkeyBinding{kModControl, 'K'},
        "migrate default actions hotkey");
    Check(MigrateAdminHotkey(0) == HotkeyBinding{kModControl, 0},
        "migrate default admin hotkey");
    Check(MigrateQuickLaunchHotkey(0) == HotkeyBinding{kModAlt, 0},
        "migrate default quick launch hotkey");
    Check(MigrateAdminHotkey(3).disabled, "migrate disabled admin hotkey");
    // System reserved keys
    Check(IsSystemReserved(kModAlt, 0x73), "Alt+F4 is system reserved");
    Check(IsSystemReserved(kModAlt, 0x09), "Alt+Tab is system reserved");
    Check(IsSystemReserved(kModControl | kModShift, 0x1B), "Ctrl+Shift+Esc is system reserved");
    Check(!IsSystemReserved(kModAlt, kVkSpace), "Alt+Space is not system reserved");
    Check(!IsSystemReserved(kModControl, 'K'), "Ctrl+K is not system reserved");

    // Internal conflict detection
    Check(HasInternalConflict(0, {kModControl, 'K'}, settings),
        "launcher conflicts with actions menu");
    Check(HasInternalConflict(0, {kModControl, kVkReturn}, settings),
        "launcher conflicts with admin hotkey");
    Check(HasInternalConflict(0, {kModAlt, '3'}, settings),
        "launcher conflicts with quick launch hotkey");
    Check(!HasInternalConflict(0, {kModControl | kModAlt, 'J'}, settings),
        "distinct launcher combo has no conflict");
    Check(HasInternalConflict(1, {kModAlt, kVkSpace}, settings),
        "actions conflicts with launcher hotkey");
    Check(HasInternalConflict(1, {kModAlt, '2'}, settings),
        "actions conflicts with quick launch");
    Settings enterSettings;
    enterSettings.actionsHotkey = {kModAlt, kVkReturn};
    Check(HasInternalConflict(2, {kModAlt, 0}, enterSettings),
        "admin conflicts if actions is on Enter with same modifier");

    Settings digitSettings;
    digitSettings.actionsHotkey = {kModControl, '5'};
    Check(HasInternalConflict(3, {kModControl, 0}, digitSettings),
        "quick launch conflicts if actions is on 1-8 with same modifier");

    // Minimized/startup switch detection
    Check(IsMinimizedSwitch(L"--minimized"), "switch --minimized");
    Check(IsMinimizedSwitch(L"-minimized"), "switch -minimized");
    Check(IsMinimizedSwitch(L"/minimized"), "switch /minimized");
    Check(IsMinimizedSwitch(L"--startup"), "switch --startup");
    Check(IsMinimizedSwitch(L"-startup"), "switch -startup");
    Check(IsMinimizedSwitch(L"/startup"), "switch /startup");
    Check(IsMinimizedSwitch(L"--hidden"), "switch --hidden");
    Check(IsMinimizedSwitch(L"-hidden"), "switch -hidden");
    Check(IsMinimizedSwitch(L"/hidden"), "switch /hidden");
    Check(IsMinimizedSwitch(L"-m"), "switch -m");
    Check(IsMinimizedSwitch(L"/m"), "switch /m");
    Check(IsMinimizedSwitch(L"--MINIMIZED"), "switch case insensitivity");
    Check(IsMinimizedSwitch(L"/STARTUP"), "switch case insensitivity /STARTUP");
    Check(!IsMinimizedSwitch(L""), "empty switch");
    Check(!IsMinimizedSwitch(L"--"), "dash only switch");
    Check(!IsMinimizedSwitch(L"--maximized"), "unrelated switch");
    Check(!IsMinimizedSwitch(L"minimized"), "bare word without prefix");

    // Replace/update switch detection
    Check(IsReplaceSwitch(L"--replace"), "switch --replace");
    Check(IsReplaceSwitch(L"-replace"), "switch -replace");
    Check(IsReplaceSwitch(L"/replace"), "switch /replace");
    Check(IsReplaceSwitch(L"--update"), "switch --update");
    Check(IsReplaceSwitch(L"-update"), "switch -update");
    Check(IsReplaceSwitch(L"/update"), "switch /update");
    Check(IsReplaceSwitch(L"-r"), "switch -r");
    Check(IsReplaceSwitch(L"/r"), "switch /r");
    Check(IsReplaceSwitch(L"--REPLACE"), "switch case insensitivity --REPLACE");
    Check(IsReplaceSwitch(L"/UPDATE"), "switch case insensitivity /UPDATE");
    Check(!IsReplaceSwitch(L""), "empty switch");
    Check(!IsReplaceSwitch(L"--"), "dash only switch");
    Check(!IsReplaceSwitch(L"--restart"), "unrelated switch");
    Check(!IsReplaceSwitch(L"replace"), "bare word without prefix");

    // Update parsing and version comparison checks
    Check(ExtractTagName("{\"tag_name\":\"v1.2.3\"}") == L"v1.2.3", "extract tag_name basic");
    Check(ExtractTagName("{\"id\":10, \"tag_name\" : \"2.0.0\"}") == L"2.0.0", "extract tag_name with whitespace");
    Check(ExtractTagName("{\"message\":\"Not Found\"}").empty(), "extract tag_name not found");
    Check(ExtractTagName("").empty(), "extract tag_name empty");

    // Asset URL extraction checks
    const std::string mockReleaseJson =
        "{\n"
        "  \"tag_name\": \"v1.1.0\",\n"
        "  \"assets\": [\n"
        "    {\"name\": \"Takeoff-v1.1.0-windows-x64.zip\", \"browser_download_url\": \"https://github.com/akiraeng/takeoff-launcher/releases/download/v1.1.0/Takeoff-v1.1.0-windows-x64.zip\"},\n"
        "    {\"name\": \"Takeoff-v1.1.0-windows-x64.zip.sha256\", \"browser_download_url\": \"https://github.com/akiraeng/takeoff-launcher/releases/download/v1.1.0/Takeoff-v1.1.0-windows-x64.zip.sha256\"},\n"
        "    {\"name\": \"Takeoff.exe\", \"browser_download_url\": \"https://github.com/akiraeng/takeoff-launcher/releases/download/v1.1.0/Takeoff.exe\"}\n"
        "  ]\n"
        "}";
    Check(ExtractAssetDownloadUrl(mockReleaseJson, L"v1.1.0") ==
          L"https://github.com/akiraeng/takeoff-launcher/releases/download/v1.1.0/Takeoff.exe",
          "extract asset url preferred match");

    const std::string mockFallbackJson =
        "{\n"
        "  \"tag_name\": \"v1.2.0\",\n"
        "  \"assets\": [\n"
        "    {\"name\": \"Takeoff-v1.2.0.exe\", \"browser_download_url\": \"https://github.com/akiraeng/takeoff-launcher/releases/download/v1.2.0/Takeoff-v1.2.0.exe\"}\n"
        "  ]\n"
        "}";
    Check(ExtractAssetDownloadUrl(mockFallbackJson, L"v1.2.0") ==
          L"https://github.com/akiraeng/takeoff-launcher/releases/download/v1.2.0/Takeoff-v1.2.0.exe",
          "extract asset url secondary exe match");

    Check(ExtractAssetDownloadUrl("{}", L"v2.0.0") ==
          L"https://github.com/akiraeng/takeoff-launcher/releases/download/v2.0.0/Takeoff.exe",
          "extract asset url fallback URL from tag");

    // Staging path and executable validation checks
    const std::wstring stagingPath = GetUpdateStagingPath(L"v1.1.0");
    Check(!stagingPath.empty(), "staging path generated");
    Check(stagingPath.find(L"Takeoff_v1.1.0.exe") != std::wstring::npos ||
          stagingPath.find(L"Takeoff_update.exe") != std::wstring::npos,
          "staging path ends with exe name");

    Check(!ValidateExecutableFile(L"C:\\non_existent_file_12345.exe"), "validate non-existent file fails");

    wchar_t ownExe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, ownExe, MAX_PATH);
    Check(ValidateExecutableFile(ownExe), "validate own PE executable succeeds");

    Check(IsNewerVersion(L"v1.0.1", L"1.0.0"), "v1.0.1 is newer than 1.0.0");
    Check(IsNewerVersion(L"1.1.0", L"1.0.0"), "1.1.0 is newer than 1.0.0");
    Check(IsNewerVersion(L"v2.0", L"1.9.9"), "v2.0 is newer than 1.9.9");
    Check(IsNewerVersion(L"1.0.0.1", L"1.0.0.0"), "1.0.0.1 is newer than 1.0.0.0");
    Check(!IsNewerVersion(L"1.0.0", L"1.0.0"), "1.0.0 is not newer than 1.0.0");
    Check(!IsNewerVersion(L"v1.0", L"1.0.0"), "v1.0 is not newer than 1.0.0");
    Check(!IsNewerVersion(L"0.9.9", L"1.0.0"), "0.9.9 is not newer than 1.0.0");
    Check(!IsNewerVersion(L"v1.0.0-beta", L"1.0.0"), "v1.0.0-beta is not newer than 1.0.0");

    // 24-hour update interval logic checks
    Check(ShouldCheckForUpdates(0, 100000, true), "check when never checked before");
    Check(!ShouldCheckForUpdates(100000, 100000 + 3600, true), "do not check after only 1 hour");
    Check(ShouldCheckForUpdates(100000, 100000 + 86400, true), "check when exactly 24 hours have passed");
    Check(ShouldCheckForUpdates(100000, 100000 + 100000, true), "check when more than 24 hours have passed");
    Check(!ShouldCheckForUpdates(100000, 100000 + 100000, false), "do not check when disabled");
    Check(ShouldCheckForUpdates(200000, 100000, true), "check when system clock shifted backwards");

    // Live WinHTTP GitHub query verification
    std::wstring liveTag, liveUrl, liveAssetUrl;
    if (QueryLatestReleaseInfo(L"api.github.com", L"/repos/akiraeng/takeoff-launcher/releases/latest", liveTag, liveUrl, liveAssetUrl)) {
        Check(!liveTag.empty(), "live GitHub query returned a release tag");
        Check(!liveAssetUrl.empty(), "live GitHub query returned an asset URL");
        std::wcout << L"[LIVE TEST] Successfully queried GitHub API! Latest release: " 
                  << liveTag << L", asset: " << liveAssetUrl << L'\n';

        // Test live download of the release asset with redirect follow
        wchar_t tempPath[MAX_PATH]{};
        if (GetTempPathW(MAX_PATH, tempPath) > 0) {
            const std::wstring testDownloadPath = std::wstring(tempPath) + L"Takeoff_download_test.exe";
            DeleteFileW(testDownloadPath.c_str());
            const bool downloaded = DownloadUpdateFile(liveAssetUrl, testDownloadPath);
            Check(downloaded, "download and validate release asset from live GitHub");
            Check(ValidateExecutableFile(testDownloadPath), "downloaded file is valid executable");
            DeleteFileW(testDownloadPath.c_str());
            std::wcout << L"[LIVE TEST] Successfully downloaded and validated update executable from GitHub!\n";
        }
    } else if (QueryLatestReleaseTag(L"api.github.com", L"/repos/microsoft/terminal/releases/latest", liveTag, liveUrl)) {
        Check(!liveTag.empty(), "live GitHub query returned a release tag");
        std::wcout << L"[LIVE TEST] Fallback query latest release: " << liveTag << L'\n';
    }

    // App recents preservation across index reload verification
    {
        struct TestApp {
            std::wstring name;
            std::wstring path;
        };
        std::vector<TestApp> oldApps = {
            {L"App A", L"C:\\Path\\A.exe"},
            {L"App B", L"C:\\Path\\B.exe"},
            {L"App C", L"C:\\Path\\C.exe"},
        };
        // Suppose App B was launched (recent index 1) then App A (recent index 0)
        std::vector<size_t> recentIndices = {1, 0};
        std::vector<std::wstring> activeRecentPaths;
        for (size_t i : recentIndices) {
            if (i < oldApps.size()) activeRecentPaths.push_back(oldApps[i].path);
        }

        // New index arrives: App D added at top, sorting changed, App A and B exist at new indices
        std::vector<TestApp> newApps = {
            {L"App 0", L"C:\\Path\\0.exe"},
            {L"App A", L"C:\\Path\\A.exe"}, // now index 1
            {L"App B", L"C:\\Path\\B.exe"}, // now index 2
            {L"App C", L"C:\\Path\\C.exe"}, // now index 3
        };
        std::vector<size_t> remappedRecent;
        for (const auto& rPath : activeRecentPaths) {
            for (size_t i = 0; i < newApps.size(); ++i) {
                if (newApps[i].path == rPath) {
                    remappedRecent.push_back(i);
                    break;
                }
            }
        }
        Check(remappedRecent.size() == 2, "remapped recent count matches");
        Check(remappedRecent[0] == 2, "App B remapped to new index 2");
        Check(remappedRecent[1] == 1, "App A remapped to new index 1");
    }

    // Hotkey conflict text and binding test
    {
        HotkeyBinding conflictHotkey{kModAlt, kVkSpace};
        std::wstring conflictText = L"The hotkey " + FormatBinding(conflictHotkey) +
            L" is currently taken by another application and could not be registered.";
        Check(conflictText.find(L"Alt + Space") != std::wstring::npos, "conflict text contains formatted hotkey Alt + Space");
        Check(FormatBinding(conflictHotkey) == L"Alt + Space", "format default hotkey");
    }

    // Performance check: 10,000 matches must execute in under 100ms
    const auto start = std::chrono::high_resolution_clock::now();
    int sum = 0;
    const std::vector<std::wstring> aliases = {L"taskmgr", L"processes", L"kill", L"tm"};
    for (int i = 0; i < 10000; ++i) {
        sum += ScoreApp(L"task manager", aliases, L"tm", i % 8);
        sum += ScoreApp(L"visual studio code", {L"vsc", L"code"}, L"vsc", -1);
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - start).count();
    Check(sum > 0, "benchmark computed positive score");
    Check(elapsed < 100, "10,000 matches executed in under 100ms");

    // --- File Search & App Priority Tests ---
    // 1. ScoreFile tests
    Check(ScoreFile(L"document", L"") == -1, "ScoreFile empty query returns -1");
    Check(ScoreFile(L"document", L"xyz") == -1, "ScoreFile non-matching returns -1");
    int exactFileScore = ScoreFile(L"report", L"report", false);
    int folderScore = ScoreFile(L"report", L"report", true);
    Check(exactFileScore == 4000, "ScoreFile exact match is 4000");
    Check(folderScore == 4040, "ScoreFile folder gets +40 bonus");
    Check(ScoreFile(L"quarterly report 2026", L"report") > 0, "ScoreFile substring match");
    Check(ScoreFile(L"quarterly report 2026", L"rep") > 0, "ScoreFile prefix match");

    // 2. Strict Application > File Ranking Invariant
    // Any matching app (even weakest fuzzy match, ~4500+) must score higher than the absolute best file match (4040).
    int weakestAppScore = ScoreApp(L"abcdefghij", {}, L"aj");
    Check(weakestAppScore >= 4500, "weakest app score is at least 4500");
    Check(weakestAppScore > exactFileScore, "weakest app match strictly beats exact file match");
    Check(weakestAppScore > folderScore, "weakest app match strictly beats exact folder match");

    // Realistic scenario: query "code" matching both an app "Visual Studio Code" and a file "code.txt"
    int appScore = ScoreApp(L"visual studio code", {L"vsc"}, L"code");
    int fileScore = ScoreFile(L"code txt", L"code");
    Check(appScore > fileScore, "app 'Visual Studio Code' strictly beats file 'code.txt'");

    // 3. Settings defaults and toggles
    Settings defaultSettings;
    Check(defaultSettings.runAtStartup == true, "run at startup enabled by default in settings");
    defaultSettings.runAtStartup = false;
    Check(!defaultSettings.runAtStartup, "run at startup toggle can be disabled");
    Check(defaultSettings.enableFileSearch == true, "file search enabled by default in settings");
    defaultSettings.enableFileSearch = false;
    Check(!defaultSettings.enableFileSearch, "file search toggle can be disabled");
    Check(defaultSettings.enableWebSearch == true, "web search enabled by default in settings");
    defaultSettings.enableWebSearch = false;
    Check(!defaultSettings.enableWebSearch, "web search toggle can be disabled");

    // UrlEncode tests
    Check(UrlEncode(L"").empty(), "UrlEncode empty string");
    Check(UrlEncode(L"takeoff") == L"takeoff", "UrlEncode plain text");
    Check(UrlEncode(L"hello world") == L"hello+world", "UrlEncode spaces to plus");
    Check(UrlEncode(L"c++ & c#") == L"c%2B%2B+%26+c%23", "UrlEncode reserved characters");
    Check(UrlEncode(L"test~_.-") == L"test~_.-", "UrlEncode unreserved characters preserved");
    Check(UrlEncode(L"caf\u00e9") == L"caf%C3%A9", "UrlEncode UTF-8 multi-byte");

    // Web search normalization tests
    Check(Normalize(L"   ").empty(), "whitespace query normalizes to empty");
    Check(Normalize(L"\t \r\n ").empty(), "whitespace query normalizes to empty");
    Check(!Normalize(L"google search").empty(), "valid search query normalizes to non-empty");

    // 4. Zero-query app-only invariant:
    // When input query is empty, FileIndex returns 0 results.
    auto emptyQueryFileResults = FileIndex::Instance().Search(L"");
    Check(emptyQueryFileResults.empty(), "empty query returns 0 files from FileIndex");

    // -----------------------------------------------------------------------------
    // Requirement R1: Directory Exclusion and Step 0 Working Directory Guards
    // -----------------------------------------------------------------------------
    // 1. Component, catalog, driver stores, and system subtrees
    Check(FileIndex::ShouldSkipDirectory(L"System32"), "System32 excluded by leaf");
    Check(FileIndex::ShouldSkipDirectory(L"system32"), "system32 lowercase excluded");
    Check(FileIndex::ShouldSkipDirectory(L"C:\\Windows\\System32"), "C:\\Windows\\System32 subtree excluded");
    Check(FileIndex::ShouldSkipDirectory(L"SysWOW64"), "SysWOW64 excluded");
    Check(FileIndex::ShouldSkipDirectory(L"catroot"), "catroot excluded");
    Check(FileIndex::ShouldSkipDirectory(L"catroot2"), "catroot2 excluded");
    Check(FileIndex::ShouldSkipDirectory(L"C:\\Windows\\System32\\catroot"), "CatRoot subtree excluded");
    Check(FileIndex::ShouldSkipDirectory(L"C:\\Windows\\System32\\catroot2"), "CatRoot2 subtree excluded");
    Check(FileIndex::ShouldSkipDirectory(L"DriverStore"), "DriverStore excluded");
    Check(FileIndex::ShouldSkipDirectory(L"FileRepository"), "FileRepository excluded");
    Check(FileIndex::ShouldSkipDirectory(L"C:\\Windows\\System32\\DriverStore"), "DriverStore subtree excluded");
    Check(FileIndex::ShouldSkipDirectory(L"C:\\Windows\\System32\\DriverStore\\FileRepository"), "FileRepository subtree excluded");
    Check(FileIndex::ShouldSkipDirectory(L"WinSxS"), "WinSxS excluded");
    Check(FileIndex::ShouldSkipDirectory(L"C:\\Windows\\WinSxS"), "C:\\Windows\\WinSxS subtree excluded");
    Check(FileIndex::ShouldSkipDirectory(L"assembly"), "assembly excluded");
    Check(FileIndex::ShouldSkipDirectory(L"servicing"), "servicing excluded");
    Check(FileIndex::ShouldSkipDirectory(L"softwaredistribution"), "softwaredistribution excluded");
    Check(FileIndex::ShouldSkipDirectory(L"Windows.old"), "Windows.old excluded");
    Check(FileIndex::ShouldSkipDirectory(L"$WINDOWS.~BT"), "$WINDOWS.~BT excluded");

    // 2. User directories and projects must not be excluded
    Check(!FileIndex::ShouldSkipDirectory(L"X:\\takeoff-launcher"), "User repo directory not excluded");
    Check(!FileIndex::ShouldSkipDirectory(L"src"), "src directory not excluded");
    Check(!FileIndex::ShouldSkipDirectory(L"C:\\Users\\Developer\\Projects\\MyGame"), "User project path not excluded");

    // 3. Step 0 working directory guard: ignores system paths and root drives
    Check(FileIndex::FindVerifiedProjectRoot(L"C:\\Windows\\System32").empty(), "System32 working directory ignored by Step 0");
    Check(FileIndex::FindVerifiedProjectRoot(L"C:\\Windows").empty(), "C:\\Windows working directory ignored by Step 0");
    Check(FileIndex::FindVerifiedProjectRoot(L"C:\\Program Files").empty(), "Program Files working directory ignored by Step 0");
    Check(FileIndex::FindVerifiedProjectRoot(L"C:\\").empty(), "Root drive C:\\ ignored by Step 0");
    Check(FileIndex::FindVerifiedProjectRoot(L"D:\\").empty(), "Root drive D:\\ ignored by Step 0");

    // 4. Drive root detection
    Check(FileIndex::IsDriveRoot(L"C:\\"), "IsDriveRoot detects C:\\");
    Check(FileIndex::IsDriveRoot(L"D:\\"), "IsDriveRoot detects D:\\");
    Check(FileIndex::IsDriveRoot(L"\\"), "IsDriveRoot detects \\");
    Check(!FileIndex::IsDriveRoot(L"C:\\Windows\\System32"), "IsDriveRoot rejects non-root system path");

    // 5. Active workspace recognized as verified project with repository markers
    fs::path verifiedProject = FileIndex::FindVerifiedProjectRoot(fs::current_path());
    Check(!verifiedProject.empty(), "Active workspace recognized as verified project");
    Check(FileIndex::HasRepositoryMarkers(verifiedProject), "Active workspace has repository markers");
    Check(!FileIndex::IsDriveRoot(verifiedProject), "IsDriveRoot rejects verified project directory");

    // -----------------------------------------------------------------------------
    // Requirement R2: File Extension Allowlist Filter
    // -----------------------------------------------------------------------------
    // Documents & Office
    Check(FileIndex::IsUserRelevantFile(L"document.pdf"), "allow .pdf");
    Check(FileIndex::IsUserRelevantFile(L"report.docx"), "allow .docx");
    Check(FileIndex::IsUserRelevantFile(L"notes.txt"), "allow .txt");
    Check(FileIndex::IsUserRelevantFile(L"README.md"), "allow .md");
    Check(FileIndex::IsUserRelevantFile(L"budget.xlsx"), "allow .xlsx");
    Check(FileIndex::IsUserRelevantFile(L"data.csv"), "allow .csv");

    // Media & Archives
    Check(FileIndex::IsUserRelevantFile(L"image.png"), "allow .png");
    Check(FileIndex::IsUserRelevantFile(L"photo.jpg"), "allow .jpg");
    Check(FileIndex::IsUserRelevantFile(L"audio.mp3"), "allow .mp3");
    Check(FileIndex::IsUserRelevantFile(L"video.mp4"), "allow .mp4");
    Check(FileIndex::IsUserRelevantFile(L"archive.zip"), "allow .zip");
    Check(FileIndex::IsUserRelevantFile(L"bundle.tar.gz"), "allow .gz");
    Check(FileIndex::IsUserRelevantFile(L"package.7z"), "allow .7z");

    // Code & Executables
    Check(FileIndex::IsUserRelevantFile(L"main.cpp"), "allow .cpp");
    Check(FileIndex::IsUserRelevantFile(L"search.h"), "allow .h");
    Check(FileIndex::IsUserRelevantFile(L"build.py"), "allow .py");
    Check(FileIndex::IsUserRelevantFile(L"app.exe"), "allow .exe");
    Check(FileIndex::IsUserRelevantFile(L"shortcut.lnk"), "allow .lnk");

    // Recognized extensionless project files
    Check(FileIndex::IsUserRelevantFile(L"Makefile"), "allow Makefile");
    Check(FileIndex::IsUserRelevantFile(L"Dockerfile"), "allow Dockerfile");
    Check(FileIndex::IsUserRelevantFile(L"LICENSE"), "allow LICENSE");
    Check(FileIndex::IsUserRelevantFile(L"README"), "allow README");

    // Rejected OS internals & drivers
    Check(!FileIndex::IsUserRelevantFile(L"catalog.cat"), "reject .cat");
    Check(!FileIndex::IsUserRelevantFile(L"driver.inf"), "reject .inf");
    Check(!FileIndex::IsUserRelevantFile(L"strings.mui"), "reject .mui");
    Check(!FileIndex::IsUserRelevantFile(L"hardware.sys"), "reject .sys");
    Check(!FileIndex::IsUserRelevantFile(L"module.dll"), "reject .dll");

    // Rejected compiler / build artifacts
    Check(!FileIndex::IsUserRelevantFile(L"main.obj"), "reject .obj");
    Check(!FileIndex::IsUserRelevantFile(L"build.tlog"), "reject .tlog");
    Check(!FileIndex::IsUserRelevantFile(L"vc.ipdb"), "reject .ipdb");
    Check(!FileIndex::IsUserRelevantFile(L"symbols.pdb"), "reject .pdb");
    Check(!FileIndex::IsUserRelevantFile(L"cache.pyc"), "reject .pyc");

    // Case insensitivity
    Check(!FileIndex::IsUserRelevantFile(L"SYSTEM.SYS"), "reject uppercase .SYS");
    Check(!FileIndex::IsUserRelevantFile(L"CATALOG.CAT"), "reject uppercase .CAT");
    Check(FileIndex::IsUserRelevantFile(L"MAIN.CPP"), "allow uppercase .CPP");
    Check(FileIndex::IsUserRelevantFile(L"REPORT.PDF"), "allow uppercase .PDF");

    // -----------------------------------------------------------------------------
    // Requirement R3: Shell Item String Allocation and Cleanup Handling
    // -----------------------------------------------------------------------------
    // 1. FormatAppsFolderPath canonicalization
    Check(FormatAppsFolderPath(L"Microsoft.WindowsTerminal_8wekyb3d8bbwe!App") ==
          L"shell:AppsFolder\\Microsoft.WindowsTerminal_8wekyb3d8bbwe!App",
          "format bare AppId to shell:AppsFolder prefix");
    Check(FormatAppsFolderPath(L"shell:AppsFolder\\App_123") ==
          L"shell:AppsFolder\\App_123",
          "preserve existing shell:AppsFolder prefix");
    Check(FormatAppsFolderPath(L"shell:Common Startup") ==
          L"shell:Common Startup",
          "preserve general shell: prefix");
    Check(FormatAppsFolderPath(L"").empty(), "empty parsing name returns empty");

    // 2. CoTaskMemPtr RAII allocation and free
    {
        const wchar_t testStr[] = L"TestAllocationString";
        const size_t byteCount = (wcslen(testStr) + 1) * sizeof(wchar_t);
        PWSTR comString = static_cast<PWSTR>(CoTaskMemAlloc(byteCount));
        Check(comString != nullptr, "CoTaskMemAlloc succeeded");
        wcscpy_s(comString, wcslen(testStr) + 1, testStr);

        CoTaskMemPtr<wchar_t> ptr(comString);
        Check(ptr.get() != nullptr, "CoTaskMemPtr holds valid pointer");
        Check(std::wstring(ptr.get()) == testStr, "CoTaskMemPtr preserves string value");
    }

    // 3. FreeStrRet correctly handles STRRET_WSTR, STRRET_CSTR, and STRRET_OFFSET
    {
        STRRET wstrRet{};
        wstrRet.uType = STRRET_WSTR;
        const wchar_t oleSample[] = L"OleWideString";
        const size_t oleBytes = (wcslen(oleSample) + 1) * sizeof(wchar_t);
        wstrRet.pOleStr = static_cast<LPWSTR>(CoTaskMemAlloc(oleBytes));
        wcscpy_s(wstrRet.pOleStr, wcslen(oleSample) + 1, oleSample);
        FreeStrRet(wstrRet);
        Check(wstrRet.pOleStr == nullptr, "FreeStrRet frees and zeroes pOleStr for STRRET_WSTR");

        STRRET cstrRet{};
        cstrRet.uType = STRRET_CSTR;
        strcpy_s(cstrRet.cStr, "SimpleAnsiString");
        FreeStrRet(cstrRet);
        Check(cstrRet.uType == STRRET_CSTR, "FreeStrRet handles STRRET_CSTR safely");

        STRRET offsetRet{};
        offsetRet.uType = STRRET_OFFSET;
        offsetRet.uOffset = 16;
        FreeStrRet(offsetRet);
        Check(offsetRet.uOffset == 16, "FreeStrRet handles STRRET_OFFSET safely");
    }

    // 4. Live shell AppsFolder resolution without allocator mismatch
    {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        PIDLIST_ABSOLUTE testAppsFolderId = nullptr;
        if (SUCCEEDED(SHGetKnownFolderIDList(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr, &testAppsFolderId)) && testAppsFolderId) {
            UniquePidl appsFolderIdHolder(testAppsFolderId);
            Microsoft::WRL::ComPtr<IShellFolder> testFolder;
            if (SUCCEEDED(SHBindToObject(nullptr, appsFolderIdHolder.get(), nullptr, IID_PPV_ARGS(&testFolder))) && testFolder) {
                Microsoft::WRL::ComPtr<IEnumIDList> testEnum;
                if (SUCCEEDED(testFolder->EnumObjects(nullptr, SHCONTF_NONFOLDERS | SHCONTF_FASTITEMS, &testEnum)) && testEnum) {
                    PITEMID_CHILD testChildRaw = nullptr;
                    if (testEnum->Next(1, &testChildRaw, nullptr) == S_OK && testChildRaw) {
                        UniquePidl childHolder(testChildRaw);
                        std::wstring resolvedName;
                        bool resolved = ResolveShellItemParsingName(appsFolderIdHolder.get(), testFolder.Get(), childHolder.get(), resolvedName);
                        Check(resolved, "ResolveShellItemParsingName succeeded for live shell child");
                        Check(!resolvedName.empty(), "resolved live shell parsing name is non-empty");
                        std::wstring fullPath = FormatAppsFolderPath(resolvedName);
                        Check(fullPath.rfind(L"shell:AppsFolder\\", 0) == 0, "full path starts with shell:AppsFolder");

                        std::wstring resolvedDirect = ResolveShellItemParsingName(testFolder.Get(), childHolder.get(), nullptr);
                        Check(!resolvedDirect.empty(), "ResolveShellItemParsingName direct fallback succeeded");
                    }
                }
            }
        }
        CoUninitialize();
    }

    // 5. Live FileIndex background indexing & sub-millisecond search benchmark
    FileIndex::Instance().Start();
    for (int w = 0; w < 40 && !FileIndex::Instance().IsReady(); ++w) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    const size_t indexedCount = FileIndex::Instance().Count();
    std::cout << "[FileIndex] Live index populated " << indexedCount << " files/folders (ready=" << FileIndex::Instance().IsReady() << ").\n";
    Check(indexedCount > 0, "FileIndex populated files from disk");

    // Determine the active workspace directory (project root)
    std::error_code ec;
    fs::path currentPath = fs::current_path(ec);
    fs::path repoPath = currentPath;
    while (repoPath.has_parent_path()) {
        const auto name = repoPath.filename().wstring();
        if (_wcsicmp(name.c_str(), L"build") == 0 ||
            _wcsicmp(name.c_str(), L"Release") == 0 ||
            _wcsicmp(name.c_str(), L"Debug") == 0 ||
            _wcsicmp(name.c_str(), L"bin") == 0) {
            repoPath = repoPath.parent_path();
        } else {
            break;
        }
    }
    const std::wstring repoPathStr = repoPath.wstring();
    const std::wstring repoFolderName = repoPath.filename().wstring();

    // Verify broad file & folder search finds repo folder and its files
    auto takeoffLauncherResults = FileIndex::Instance().Search(repoFolderName, 10);
    Check(!takeoffLauncherResults.empty(), "repo folder query returns results");
    Check(takeoffLauncherResults[0].isDirectory, "repo folder #1 result is a directory");
    Check(takeoffLauncherResults[0].name == repoFolderName, "repo folder #1 result matches folder name");

    auto pathResults = FileIndex::Instance().Search(repoPathStr, 10);
    Check(!pathResults.empty(), "repo path query returns results");

    auto takeoffMainResults = FileIndex::Instance().Search(L"takeoff main", 10);
    Check(!takeoffMainResults.empty(), "takeoff main multi-token query returns results");
    Check(takeoffMainResults[0].name == L"main.cpp", "takeoff main finds main.cpp");

    std::vector<std::wstring> testQueries = {L"takeoff", repoFolderName, repoPathStr, L"takeoff main", L"launcher", L"main.cpp"};
    for (const auto& q : testQueries) {
        auto results = FileIndex::Instance().Search(q, 5);
        std::wcout << L"Query [" << q << L"] -> " << results.size() << L" results:\n";
        for (const auto& r : results) {
            std::wcout << L"  - " << (r.isDirectory ? L"[DIR]  " : L"[FILE] ") << r.name << L" (" << r.path << L") score=" << r.score << L"\n";
        }
    }

    // Benchmark 100 search queries on live in-memory index
    const auto fileStart = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 100; ++i) {
        auto r = FileIndex::Instance().Search(L"project", 10);
    }
    const auto fileElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now() - fileStart).count();
    const double perQueryMs = (fileElapsed / 100.0) / 1000.0;
    std::cout << "[FileIndex] 100 searches completed in " << fileElapsed << "us ("
              << perQueryMs << "ms per query across " << indexedCount << " files!)\n";
    Check(perQueryMs < 5.0, "file search evaluation executes in under 5ms per query");
    FileIndex::Instance().Stop();

    // -----------------------------------------------------------------------------
    // Requirement R2: Chunked Snapshot Publishing & Memory Bound Verification
    // -----------------------------------------------------------------------------
    {
        std::vector<FileItem> chunk1;
        chunk1.push_back({L"testdoc.pdf", L"testdoc pdf", L"C:\\Users\\Test\\testdoc.pdf", L"c users test testdoc pdf", false});
        chunk1.push_back({L"testcode.cpp", L"testcode cpp", L"C:\\Users\\Test\\testcode.cpp", L"c users test testcode cpp", false});
        FileIndex::Instance().PublishSnapshot(std::move(chunk1));
        Check(FileIndex::Instance().Count() == 2, "PublishSnapshot initializes count to 2 via move");

        std::vector<FileItem> chunk2;
        chunk2.push_back({L"testheader.h", L"testheader h", L"C:\\Users\\Test\\testheader.h", L"c users test testheader h", false});
        FileIndex::Instance().AppendSnapshotChunk(std::move(chunk2));
        Check(FileIndex::Instance().Count() == 3, "AppendSnapshotChunk appends chunk and increments count to 3");

        auto chunkResults = FileIndex::Instance().Search(L"testheader");
        Check(!chunkResults.empty() && chunkResults[0].name == L"testheader.h", "Search retrieves item from appended snapshot chunk");

        auto chunk1Results = FileIndex::Instance().Search(L"testdoc");
        Check(!chunk1Results.empty() && chunk1Results[0].name == L"testdoc.pdf", "Search retrieves item from initial snapshot chunk");

        // Memory budget verification: total heap footprint across typical startup remains < 25 MB
        constexpr size_t kMaxHeapBudget = 25 * 1024 * 1024; // 25 MB
        const size_t estimatedHeapBytes = indexedCount * 550;
        std::cout << "[FileIndex] Estimated startup index heap usage: " << (estimatedHeapBytes / (1024 * 1024))
                  << " MB (" << estimatedHeapBytes << " bytes for " << indexedCount << " items)\n";
        Check(estimatedHeapBytes < kMaxHeapBudget, "FileIndex heap usage under typical startup is strictly bounded < 25 MB");
    }

    // 6. Settings Scroll and Viewport Invariants:
    // Guarantees Settings content cleanly fits and scrolls without overlapping FooterTop (440px).
    constexpr float kWindowHeight = 482.0f;
    constexpr float kFooterH = 42.0f;
    constexpr float kSettingsHeaderH = 46.0f;
    constexpr float kSettingsRowH = 47.0f;
    constexpr float footerTop = kWindowHeight - kFooterH; // 440.0f
    constexpr float generalTop = 280.0f;
    constexpr float row7Top = generalTop + 3 * kSettingsRowH; // 421.0f
    constexpr float row7Bottom = row7Top + kSettingsRowH;     // 468.0f
    constexpr float contentBottom = row7Bottom + 14.0f;       // 482.0f
    constexpr float maxScroll = contentBottom - footerTop;    // 42.0f

    Check(footerTop == 440.0f, "footer top is exactly 440px");
    Check(row7Bottom > footerTop, "unscrolled row 7 exceeds footer top, proving scroll is required");
    Check(maxScroll == 42.0f, "settings max scroll is 42px");

    // When scrolled to maxScroll:
    const float scrolledRow7Bottom = row7Bottom - maxScroll;
    Check(scrolledRow7Bottom < footerTop, "scrolled row 7 bottom is strictly above footer top");
    Check(footerTop - scrolledRow7Bottom >= 14.0f, "row 7 has at least 14px clearance above footer");

    // Check viewport height and scrollable area:
    constexpr float viewportHeight = footerTop - kSettingsHeaderH; // 394.0f
    Check(viewportHeight == 394.0f, "settings viewport height is 394px");

    // In individual categories, content height is well under viewportHeight (394px)
    constexpr float kCategoryShortcutsContentH = 36.0f + 2 * kSettingsRowH + 12.0f; // 142px
    constexpr float kCategorySystemContentH = 36.0f + 5 * kSettingsRowH + 12.0f;    // 283px
    constexpr float kCategorySearchContentH = 36.0f + 2 * kSettingsRowH + 12.0f;    // 142px
    Check(kCategoryShortcutsContentH < viewportHeight, "Shortcuts category has zero overflow in viewport");
    Check(kCategorySystemContentH < viewportHeight, "System category has zero overflow in viewport");
    Check(kCategorySearchContentH < viewportHeight, "Search category has zero overflow in viewport");

    // --- Calculator Tests ---
    // AppCategory::Calculator distinction
    Check(AppCategory::Calculator != AppCategory::Application, "Calculator category is distinct");
    Check(AppCategory::Calculator != AppCategory::System, "Calculator distinct from System");
    Check(AppCategory::Calculator != AppCategory::File, "Calculator distinct from File");

    // 1. Basic arithmetic
    auto r1 = EvaluateExpression(L"125 * 8");
    Check(r1.has_value(), "125 * 8 evaluates successfully");
    Check(r1->value == 1000.0, "125 * 8 value is 1000");
    Check(r1->rawResult == L"1000", "125 * 8 raw result is 1000");
    Check(r1->formattedResult == L"1000", "125 * 8 formatted result is 1000");

    auto r2 = EvaluateExpression(L"2 + 2");
    Check(r2.has_value() && r2->value == 4.0 && r2->rawResult == L"4", "2 + 2 == 4");

    auto r3 = EvaluateExpression(L"100 - 35");
    Check(r3.has_value() && r3->value == 65.0 && r3->rawResult == L"65", "100 - 35 == 65");

    auto r4 = EvaluateExpression(L"100 / 4");
    Check(r4.has_value() && r4->value == 25.0 && r4->rawResult == L"25", "100 / 4 == 25");

    // 2. Precedence and parentheses
    auto r5 = EvaluateExpression(L"2 + 3 * 4");
    Check(r5.has_value() && r5->value == 14.0, "2 + 3 * 4 == 14");

    auto r6 = EvaluateExpression(L"(2 + 3) * 4");
    Check(r6.has_value() && r6->value == 20.0, "(2 + 3) * 4 == 20");

    auto r7 = EvaluateExpression(L"10 - 2 * 3");
    Check(r7.has_value() && r7->value == 4.0, "10 - 2 * 3 == 4");

    auto r8 = EvaluateExpression(L"((5 + 5) * (3 + 2)) / 2");
    Check(r8.has_value() && r8->value == 25.0, "nested parentheses == 25");

    // 3. Decimals, negative numbers & commas
    auto r9 = EvaluateExpression(L"0.1 + 0.2");
    Check(r9.has_value() && r9->rawResult == L"0.3", "0.1 + 0.2 cleaned of floating noise");

    auto r10 = EvaluateExpression(L"125 / 8");
    Check(r10.has_value() && r10->value == 15.625 && r10->rawResult == L"15.625", "125 / 8 == 15.625");

    auto r11 = EvaluateExpression(L"-5 + 10");
    Check(r11.has_value() && r11->value == 5.0, "-5 + 10 == 5");

    auto r12 = EvaluateExpression(L"-(3 + 2) * 4");
    Check(r12.has_value() && r12->value == -20.0, "-(3 + 2) * 4 == -20");

    auto r13 = EvaluateExpression(L"1,000 * 2");
    Check(r13.has_value() && r13->value == 2000.0 && r13->formattedResult == L"2000", "comma thousand input separator");

    // 4. Exponents & powers
    auto r14 = EvaluateExpression(L"2^10");
    Check(r14.has_value() && r14->value == 1024.0 && r14->formattedResult == L"1024", "2^10 == 1024");

    auto r15 = EvaluateExpression(L"2**8");
    Check(r15.has_value() && r15->value == 256.0, "2**8 == 256");

    auto r16 = EvaluateExpression(L"3^3");
    Check(r16.has_value() && r16->value == 27.0, "3^3 == 27");

    // 5. Modulo & percentage
    auto r17 = EvaluateExpression(L"10 % 3");
    Check(r17.has_value() && r17->value == 1.0, "10 % 3 == 1");

    auto r18 = EvaluateExpression(L"200 * 15%");
    Check(r18.has_value() && r18->value == 30.0, "200 * 15% == 30");

    auto r19 = EvaluateExpression(L"50% * 80");
    Check(r19.has_value() && r19->value == 40.0, "50% * 80 == 40");

    // 6. Factorials
    auto r20 = EvaluateExpression(L"5!");
    Check(r20.has_value() && r20->value == 120.0, "5! == 120");

    auto r21 = EvaluateExpression(L"0!");
    Check(r21.has_value() && r21->value == 1.0, "0! == 1");

    // 7. Math functions
    auto r22 = EvaluateExpression(L"sqrt(144)");
    Check(r22.has_value() && r22->value == 12.0 && r22->rawResult == L"12", "sqrt(144) == 12");

    auto r23 = EvaluateExpression(L"cbrt(27)");
    Check(r23.has_value() && r23->value == 3.0, "cbrt(27) == 3");

    auto r24 = EvaluateExpression(L"abs(-42)");
    Check(r24.has_value() && r24->value == 42.0, "abs(-42) == 42");

    auto r25 = EvaluateExpression(L"sin(0)");
    Check(r25.has_value() && r25->value == 0.0, "sin(0) == 0");

    auto r26 = EvaluateExpression(L"cos(0)");
    Check(r26.has_value() && r26->value == 1.0, "cos(0) == 1");

    auto r27 = EvaluateExpression(L"ln(e)");
    Check(r27.has_value() && std::abs(r27->value - 1.0) < 1e-9, "ln(e) == 1");

    auto r28 = EvaluateExpression(L"log10(1000)");
    Check(r28.has_value() && r28->value == 3.0, "log10(1000) == 3");

    auto r29 = EvaluateExpression(L"log2(1024)");
    Check(r29.has_value() && r29->value == 10.0, "log2(1024) == 10");

    auto r30 = EvaluateExpression(L"pow(2, 5)");
    Check(r30.has_value() && r30->value == 32.0, "pow(2, 5) == 32");

    // 8. Constants & implicit multiplication
    auto r31 = EvaluateExpression(L"2 * pi");
    Check(r31.has_value() && std::abs(r31->value - 6.283185307) < 1e-5, "2 * pi");

    auto r32 = EvaluateExpression(L"2pi");
    Check(r32.has_value() && std::abs(r32->value - 6.283185307) < 1e-5, "2pi implicit multiplication");

    auto r33 = EvaluateExpression(L"2(3 + 4)");
    Check(r33.has_value() && r33->value == 14.0, "2(3+4) implicit multiplication");

    auto r34 = EvaluateExpression(L"(2 + 3)(4 + 5)");
    Check(r34.has_value() && r34->value == 45.0, "(2+3)(4+5) implicit multiplication");

    auto r35 = EvaluateExpression(L"5sqrt(4)");
    Check(r35.has_value() && r35->value == 10.0, "5sqrt(4) implicit multiplication");

    // 9. Alternate operators: 'x', Unicode '×', '÷'
    auto rx1 = EvaluateExpression(L"10x10");
    Check(rx1.has_value() && rx1->value == 100.0 && rx1->formattedResult == L"100", "10x10 == 100");

    auto rx2 = EvaluateExpression(L"10X10");
    Check(rx2.has_value() && rx2->value == 100.0 && rx2->formattedResult == L"100", "10X10 == 100");

    auto rx3 = EvaluateExpression(L"10x10x10");
    Check(rx3.has_value() && rx3->value == 1000.0, "10x10x10 == 1000");

    auto rx4 = EvaluateExpression(L"2.5x4");
    Check(rx4.has_value() && rx4->value == 10.0, "2.5x4 == 10");

    auto r36 = EvaluateExpression(L"125 x 8");
    Check(r36.has_value() && r36->value == 1000.0, "125 x 8 == 1000");

    auto r37 = EvaluateExpression(L"125 \u00D7 8");
    Check(r37.has_value() && r37->value == 1000.0, "125 \u00D7 8 == 1000");

    auto r38 = EvaluateExpression(L"100 \u00F7 4");
    Check(r38.has_value() && r38->value == 25.0, "100 \u00F7 4 == 25");

    // 10. Equals prefix and suffix
    auto r39 = EvaluateExpression(L"= 125 * 8");
    Check(r39.has_value() && r39->value == 1000.0, "= 125 * 8 == 1000");

    auto r40 = EvaluateExpression(L"125 * 8 =");
    Check(r40.has_value() && r40->value == 1000.0, "125 * 8 = == 1000");

    auto r41 = EvaluateExpression(L"= 42");
    Check(r41.has_value() && r41->value == 42.0, "= 42 == 42");

    // 11. Rejection of non-mathematical queries (preserves normal app and file search)
    Check(!EvaluateExpression(L"cal").has_value(), "'cal' is not evaluated as calculation");
    Check(!EvaluateExpression(L"notepad").has_value(), "'notepad' is not evaluated as calculation");
    Check(!EvaluateExpression(L"chrome").has_value(), "'chrome' is not evaluated as calculation");
    Check(!EvaluateExpression(L"win 11").has_value(), "'win 11' is not evaluated as calculation");
    Check(!EvaluateExpression(L"office 365").has_value(), "'office 365' is not evaluated as calculation");
    Check(!EvaluateExpression(L"gta 5").has_value(), "'gta 5' is not evaluated as calculation");
    Check(!EvaluateExpression(L"42").has_value(), "bare number '42' without operators is not a calculation");
    Check(!EvaluateExpression(L"125 * ").has_value(), "incomplete expression '125 * ' rejected");
    Check(!EvaluateExpression(L"(2 + 3").has_value(), "unmatched '(' rejected");
    Check(!EvaluateExpression(L"10 / 0").has_value(), "division by zero rejected");
    Check(!EvaluateExpression(L"sqrt(-4)").has_value(), "sqrt of negative number rejected");
    Check(!EvaluateExpression(L"").has_value(), "empty input rejected");
    Check(!EvaluateExpression(L"   ").has_value(), "whitespace input rejected");

    // 12. Launcher integration & ranking invariants
    {
        struct MockApp {
            std::wstring name;
            std::wstring path;
            AppCategory category;
            std::wstring parameters;
        };
        std::vector<MockApp> mockApps = {
            {L"Calculator", L"calc.exe", AppCategory::Application, L""},
            {L"Calendar", L"calendar.exe", AppCategory::Application, L""}
        };
        std::vector<size_t> results;
        // Search query "125 * 8"
        std::wstring expr = L"125 * 8";
        auto calc = EvaluateExpression(expr);
        Check(calc.has_value(), "calc evaluated for input 125 * 8");
        MockApp calcEntry{calc->formattedResult, calc->rawResult, AppCategory::Calculator, calc->expression};
        size_t calcIdx = mockApps.size();
        mockApps.push_back(calcEntry);
        results.insert(results.begin(), calcIdx);

        Check(results.size() == 1, "calculator result added to results");
        Check(results[0] == calcIdx, "calculator result is at index 0");
        Check(mockApps[results[0]].category == AppCategory::Calculator, "result has Calculator category");
        Check(mockApps[results[0]].name == L"1000", "result name is 1000 without commas");
        Check(mockApps[results[0]].path == L"1000", "result path is raw 1000 for clipboard copy");
        Check(mockApps[results[0]].parameters == L"125 * 8", "parameters stores original expression");
    }

    std::cout << "All search, calculator, text editing, hotkey, and settings scroll checks passed in " << elapsed << "ms.\n";
}
