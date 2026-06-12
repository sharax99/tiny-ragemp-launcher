#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <shellapi.h>
#include <string>
#include <cstdio>
#include <cstring>
#include <urlmon.h>
#include <thread>
#include <mutex>
#include <vector>
#include <atomic>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <bcrypt.h>
#include <nlohmann/json.hpp>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "bcrypt.lib")

static HWND                    g_hwnd               = nullptr;
static ID3D11Device*           g_pd3dDevice         = nullptr;
static ID3D11DeviceContext*    g_pd3dContext        = nullptr;
static IDXGISwapChain*         g_pSwapChain         = nullptr;
static ID3D11RenderTargetView* g_mainRTV            = nullptr;
static bool                    g_running            = true;
static UINT                    g_resizeWidth        = 0;
static UINT                    g_resizeHeight       = 0;

constexpr int    WINDOW_W       = 420;
constexpr int    WINDOW_H       = 520;
constexpr int    TITLE_BAR_H    = 32;
constexpr int    RESIZE_BORDER  = 0;   

enum class Language { EN, TR, RU };
enum class GamePlatform { UNKNOWN, STEAM, EPIC, ROCKSTAR };

struct DowngradeFileInfo {
    std::string relativePath;
    std::string sha256Current;     // Updated file hash (Rockstar's new version)
    std::string sha256Downgraded;  // Downgraded file hash (our target)
    std::string downloadUrl;
    uint64_t    sizeBytes = 0;
};

struct ProtectionStatus {
    bool steamManifestLocked = false;
    bool socialClubBlocked   = false;
    bool epicManifestLocked  = false;
    bool scOfflineMode       = false;
    bool filesDowngraded     = false;
    GamePlatform platform    = GamePlatform::UNKNOWN;
    std::string gtaPath;
    std::string platformName;
};

struct AppState {
    char        server[128] = "rage.sharax.com";
    char        port[8]     = "22005";
    std::string statusMsg;
    bool        statusIsError = false;
    DWORD       statusUntil   = 0;  
    Language    lang          = Language::EN;
};
static AppState g_app;

static const char* Tr(const char* en, const char* tr, const char* ru) {
    if (g_app.lang == Language::TR) return tr;
    if (g_app.lang == Language::RU) return ru;
    return en;
}

static std::atomic<bool> g_isDownloading{false};
static std::mutex        g_downloadMutex;
static std::string       g_downloadStatusMsg;

// ==================== DOWNGRADE SYSTEM STATE ====================
static std::atomic<bool>  g_isDowngrading{false};
static std::mutex         g_downgradeMutex;
static std::string        g_downgradeStatusMsg;
static ProtectionStatus   g_protectionStatus;
static float              g_downgradeProgress = 0.0f;

// Downgrade manifest URL — update this Gist with your downgrade info
static const wchar_t* DOWNGRADE_MANIFEST_URL = 
    L"https://gist.githubusercontent.com/sharax99/REPLACE_WITH_REAL_GIST_ID/raw/downgrade_manifest.json";

using json = nlohmann::json;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
static LRESULT WINAPI WndProc(HWND, UINT, WPARAM, LPARAM);
static bool CreateDeviceD3D(HWND hWnd);
static void CleanupDeviceD3D();
static void CreateRenderTarget();
static void CleanupRenderTarget();
static void ApplyDarkTheme();
static void DrawUI();
static void OnConnect();
static void SetStatus(const std::string& msg, bool isError);

static bool WriteRegistry(const char* valueName, const char* value)
{
    HKEY hKey;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\RAGE-MP", 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr,
                        &hKey, nullptr) != ERROR_SUCCESS)
        return false;
    LONG r = RegSetValueExA(hKey, valueName, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(value),
                            static_cast<DWORD>(strlen(value) + 1));
    RegCloseKey(hKey);
    return r == ERROR_SUCCESS;
}

static std::string ReadRegistry(const char* valueName)
{
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\RAGE-MP", 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return "";
    char buf[256] = {0};
    DWORD sz = sizeof(buf);
    if (RegQueryValueExA(hKey, valueName, nullptr, nullptr, reinterpret_cast<LPBYTE>(buf), &sz) == ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return std::string(buf);
    }
    RegCloseKey(hKey);
    return "";
}

static std::wstring GetLauncherDir()
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    return p.substr(0, p.find_last_of(L"\\/"));
}

static std::string ReadFileToString(const std::wstring& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::stringstream buffer;
    buffer << f.rdbuf();
    return buffer.str();
}

static void WriteStringToFile(const std::wstring& path, const std::string& content) {
    std::ofstream f(path);
    f << content;
}

// ==================== DOWNGRADE SYSTEM ====================

// --- Utility: Wide <-> Narrow string conversion ---
static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int sz = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring ws(sz, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), ws.data(), sz);
    return ws;
}

static std::string WideToUtf8(const std::wstring& ws) {
    if (ws.empty()) return "";
    int sz = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    std::string s(sz, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), s.data(), sz, nullptr, nullptr);
    return s;
}

// --- SHA-256 via Windows BCrypt ---
static std::string ComputeSHA256(const std::wstring& filePath) {
    HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return "";

    BCRYPT_ALG_HANDLE hAlg = NULL;
    BCRYPT_HASH_HANDLE hHash = NULL;
    std::string result;

    if (FAILED(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, NULL, 0)))
        { CloseHandle(hFile); return ""; }

    DWORD hashObjSize = 0, dataSize = 0;
    BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, (PBYTE)&hashObjSize, sizeof(DWORD), &dataSize, 0);

    std::vector<BYTE> hashObj(hashObjSize);
    if (FAILED(BCryptCreateHash(hAlg, &hHash, hashObj.data(), hashObjSize, NULL, 0, 0)))
        { BCryptCloseAlgorithmProvider(hAlg, 0); CloseHandle(hFile); return ""; }

    BYTE buffer[65536];
    DWORD bytesRead;
    while (ReadFile(hFile, buffer, sizeof(buffer), &bytesRead, NULL) && bytesRead > 0) {
        BCryptHashData(hHash, buffer, bytesRead, 0);
    }

    BYTE hash[32];
    BCryptFinishHash(hHash, hash, 32, 0);

    char hex[65];
    for (int i = 0; i < 32; i++) sprintf_s(hex + i * 2, 3, "%02x", hash[i]);
    hex[64] = 0;
    result = std::string(hex);

    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    CloseHandle(hFile);
    return result;
}

// --- GTA V Install Path Detection ---
static std::wstring FindGTAPathFromRegistry() {
    // Try Rockstar's registry key (works for all platforms)
    const char* keys[] = {
        "SOFTWARE\\WOW6432Node\\Rockstar Games\\Grand Theft Auto V",
        "SOFTWARE\\Rockstar Games\\Grand Theft Auto V",
        "SOFTWARE\\WOW6432Node\\Rockstar Games\\GTAV",
    };
    for (auto key : keys) {
        HKEY hKey;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, key, 0, KEY_READ | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS) {
            char buf[MAX_PATH] = {0};
            DWORD sz = sizeof(buf);
            if (RegQueryValueExA(hKey, "InstallFolder", nullptr, nullptr, (LPBYTE)buf, &sz) == ERROR_SUCCESS) {
                RegCloseKey(hKey);
                std::string path(buf);
                if (!path.empty() && GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES)
                    return Utf8ToWide(path);
            }
            RegCloseKey(hKey);
        }
    }
    return L"";
}

static std::wstring FindGTAPath() {
    // 1. Try registry
    std::wstring regPath = FindGTAPathFromRegistry();
    if (!regPath.empty()) return regPath;

    // 2. Try config file next to launcher
    std::wstring configPath = GetLauncherDir() + L"\\gta_path.txt";
    std::string cfgContent = ReadFileToString(configPath);
    cfgContent.erase(cfgContent.find_last_not_of(" \n\r\t") + 1);
    if (!cfgContent.empty()) {
        std::wstring p = Utf8ToWide(cfgContent);
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES)
            return p;
    }

    // 3. Common default paths
    const wchar_t* defaults[] = {
        L"C:\\Program Files\\Rockstar Games\\Grand Theft Auto V",
        L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Grand Theft Auto V",
        L"D:\\SteamLibrary\\steamapps\\common\\Grand Theft Auto V",
        L"E:\\SteamLibrary\\steamapps\\common\\Grand Theft Auto V",
        L"C:\\Program Files\\Epic Games\\GTAV",
    };
    for (auto dp : defaults) {
        if (GetFileAttributesW(dp) != INVALID_FILE_ATTRIBUTES)
            return dp;
    }

    return L"";
}

// --- Platform Detection ---
static GamePlatform DetectGamePlatform(const std::wstring& gtaPath) {
    // Steam: has steam_api64.dll
    if (GetFileAttributesW((gtaPath + L"\\steam_api64.dll").c_str()) != INVALID_FILE_ATTRIBUTES)
        return GamePlatform::STEAM;

    // Epic: has EOSSDK-Win64-Shipping.dll or PlayGTAV.exe
    if (GetFileAttributesW((gtaPath + L"\\EOSSDK-Win64-Shipping.dll").c_str()) != INVALID_FILE_ATTRIBUTES)
        return GamePlatform::EPIC;

    // Rockstar (fallback)
    return GamePlatform::ROCKSTAR;
}

static const char* PlatformToString(GamePlatform p) {
    switch (p) {
        case GamePlatform::STEAM:     return "Steam";
        case GamePlatform::EPIC:      return "Epic Games";
        case GamePlatform::ROCKSTAR:  return "Rockstar";
        default:                      return "Unknown";
    }
}

// --- Steam App Manifest Protection ---
static std::wstring FindSteamAppsPath() {
    // Read Steam install path from registry
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return L"";

    char buf[MAX_PATH] = {0};
    DWORD sz = sizeof(buf);
    if (RegQueryValueExA(hKey, "SteamPath", nullptr, nullptr, (LPBYTE)buf, &sz) != ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return L"";
    }
    RegCloseKey(hKey);

    std::wstring steamPath = Utf8ToWide(std::string(buf));

    // Check main steamapps folder
    std::wstring mainApps = steamPath + L"\\steamapps";
    if (GetFileAttributesW((mainApps + L"\\appmanifest_271590.acf").c_str()) != INVALID_FILE_ATTRIBUTES)
        return mainApps;

    // Parse libraryfolders.vdf to find other libraries
    std::string vdf = ReadFileToString(steamPath + L"\\config\\libraryfolders.vdf");
    if (!vdf.empty()) {
        // Simple parse: find "path" values
        size_t pos = 0;
        while ((pos = vdf.find("\"path\"", pos)) != std::string::npos) {
            pos = vdf.find("\"", pos + 6);
            if (pos == std::string::npos) break;
            pos++;
            size_t end = vdf.find("\"", pos);
            if (end == std::string::npos) break;
            std::string libPath = vdf.substr(pos, end - pos);
            // Replace \\\\ with backslash
            std::string cleaned;
            for (size_t i = 0; i < libPath.size(); i++) {
                if (libPath[i] == '\\' && i + 1 < libPath.size() && libPath[i + 1] == '\\')
                    i++; // skip double backslash
                cleaned += libPath[i];
            }
            std::wstring wLib = Utf8ToWide(cleaned) + L"\\steamapps";
            if (GetFileAttributesW((wLib + L"\\appmanifest_271590.acf").c_str()) != INVALID_FILE_ATTRIBUTES)
                return wLib;
            pos = end + 1;
        }
    }

    return L"";
}

static bool ProtectSteamManifest() {
    std::wstring steamApps = FindSteamAppsPath();
    if (steamApps.empty()) return false;

    std::wstring manifestPath = steamApps + L"\\appmanifest_271590.acf";

    // Remove read-only first so we can modify
    SetFileAttributesW(manifestPath.c_str(), FILE_ATTRIBUTE_NORMAL);

    std::string content = ReadFileToString(manifestPath);
    if (content.empty()) return false;

    // Replace StateFlags with "4" (FullyInstalled)
    // Format: \t"StateFlags"\t\t"X"
    auto replaceField = [&](const std::string& field, const std::string& newValue) {
        size_t pos = content.find("\"" + field + "\"");
        if (pos == std::string::npos) return;
        // Find the value after the field
        pos = content.find("\"", pos + field.size() + 2);
        if (pos == std::string::npos) return;
        pos++; // skip opening quote
        size_t end = content.find("\"", pos);
        if (end == std::string::npos) return;
        content.replace(pos, end - pos, newValue);
    };

    replaceField("StateFlags", "4");
    replaceField("AutoUpdateBehavior", "1");
    replaceField("ScheduledAutoUpdate", "0");

    WriteStringToFile(manifestPath, content);

    // Lock as read-only — Steam can't modify it
    SetFileAttributesW(manifestPath.c_str(), FILE_ATTRIBUTE_READONLY);

    return true;
}

// --- Social Club Firewall Protection ---
static bool RunElevatedCommand(const std::wstring& cmd) {
    // Since launcher already runs as admin (requireAdministrator manifest),
    // we can just use CreateProcessW directly
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};

    std::wstring cmdCopy = cmd;
    DWORD exitCode = 1;
    if (CreateProcessW(nullptr, cmdCopy.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 10000); // 10s timeout
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return exitCode == 0;
}

static bool IsFirewallRuleExists(const std::wstring& ruleName) {
    std::wstring cmd = L"netsh advfirewall firewall show rule name=\"" + ruleName + L"\"";
    return RunElevatedCommand(cmd);
}

static bool BlockGTAVLauncherFirewall(const std::wstring& gtaPath) {
    std::wstring launcherExe = gtaPath + L"\\GTAVLauncher.exe";
    if (GetFileAttributesW(launcherExe.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false; // GTAVLauncher.exe doesn't exist

    std::wstring ruleName = L"SharaxDowngradeProtection";

    // Check if rule already exists
    if (IsFirewallRuleExists(ruleName))
        return true; // Already protected

    // Add outbound block rule
    std::wstring cmd = L"netsh advfirewall firewall add rule "
        L"name=\"" + ruleName + L"\" "
        L"dir=out "
        L"action=block "
        L"program=\"" + launcherExe + L"\" "
        L"enable=yes "
        L"profile=any";

    return RunElevatedCommand(cmd);
}

// --- Social Club Offline Mode ---
static bool SetSCOfflineMode() {
    HKEY hKey;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, 
                        "Software\\Rockstar Games\\Grand Theft Auto V",
                        0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE, nullptr, &hKey, nullptr) != ERROR_SUCCESS)
        return false;

    const char* cmdLine = "-scOfflineOnly";
    LONG r = RegSetValueExA(hKey, "CmdLine", 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(cmdLine),
                            static_cast<DWORD>(strlen(cmdLine) + 1));
    RegCloseKey(hKey);
    return r == ERROR_SUCCESS;
}

// --- Epic Games Store Protection ---
static bool ProtectEpicInstallation(const std::wstring& gtaPath) {
    // Find Epic manifests directory
    wchar_t programData[MAX_PATH];
    if (!GetEnvironmentVariableW(L"PROGRAMDATA", programData, MAX_PATH))
        return false;

    std::wstring manifestDir = std::wstring(programData) + 
        L"\\Epic\\EpicGamesLauncher\\Data\\Manifests";

    if (GetFileAttributesW(manifestDir.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;

    // Find GTA V manifest (.item file)
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW((manifestDir + L"\\*.item").c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return false;

    bool found = false;
    do {
        std::wstring itemPath = manifestDir + L"\\" + fd.cFileName;
        std::string content = ReadFileToString(itemPath);
        // Check if this manifest is for GTA V (AppID or install location)
        if (content.find("Grand Theft Auto V") != std::string::npos ||
            content.find("GTA5") != std::string::npos ||
            content.find("271590") != std::string::npos) {
            // Lock this manifest as read-only
            SetFileAttributesW(itemPath.c_str(), FILE_ATTRIBUTE_READONLY);
            found = true;
            break;
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);

    return found;
}

// --- Apply All Protections ---
static ProtectionStatus ApplyUpdateProtection(GamePlatform platform, 
                                               const std::wstring& gtaPath) {
    ProtectionStatus status;
    status.platform = platform;
    status.gtaPath = WideToUtf8(gtaPath);
    status.platformName = PlatformToString(platform);

    // Social Club protection — applies to ALL platforms
    status.socialClubBlocked = BlockGTAVLauncherFirewall(gtaPath);
    status.scOfflineMode = SetSCOfflineMode();

    // Platform-specific protection
    switch (platform) {
        case GamePlatform::STEAM:
            status.steamManifestLocked = ProtectSteamManifest();
            break;
        case GamePlatform::EPIC:
            status.epicManifestLocked = ProtectEpicInstallation(gtaPath);
            break;
        case GamePlatform::ROCKSTAR:
            // Social Club protection is sufficient
            break;
        default:
            break;
    }

    return status;
}

// --- Downgrade Check & Download Thread ---
static void SetDowngradeStatus(const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_downgradeMutex);
    g_downgradeStatusMsg = msg;
}

static bool DownloadFileTo(const std::wstring& url, const std::wstring& dest) {
    // Use PowerShell for reliable HTTPS downloads with progress
    std::wstring cmd = L"powershell -NoProfile -NonInteractive -Command \"";
    cmd += L"[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12; ";
    cmd += L"Invoke-WebRequest -UseBasicParsing -Uri '";
    cmd += url;
    cmd += L"' -OutFile '";
    cmd += dest;
    cmd += L"'\"";

    return RunElevatedCommand(cmd);
}

static void DowngradeCheckThread() {
    g_isDowngrading = true;
    SetDowngradeStatus("Checking game version...");

    // 1. Find GTA V path
    std::wstring gtaPath = FindGTAPath();
    if (gtaPath.empty()) {
        SetDowngradeStatus("GTA V not found");
        Sleep(3000);
        SetDowngradeStatus("");
        g_isDowngrading = false;
        return;
    }

    // 2. Detect platform
    GamePlatform platform = DetectGamePlatform(gtaPath);
    {
        std::lock_guard<std::mutex> lock(g_downgradeMutex);
        g_protectionStatus.platform = platform;
        g_protectionStatus.platformName = PlatformToString(platform);
        g_protectionStatus.gtaPath = WideToUtf8(gtaPath);
    }

    SetDowngradeStatus("Downloading downgrade manifest...");

    // 3. Download manifest
    std::wstring base = GetLauncherDir();
    std::wstring tempManifest = base + L"\\downgrade_manifest.json";

    HRESULT hr = URLDownloadToFileW(nullptr, DOWNGRADE_MANIFEST_URL, 
                                     tempManifest.c_str(), 0, nullptr);
    if (FAILED(hr)) {
        // Can't reach manifest server — check if already downgraded via local state
        std::wstring stateFile = base + L"\\downgrade_state.json";
        std::string stateContent = ReadFileToString(stateFile);
        if (!stateContent.empty()) {
            try {
                json state = json::parse(stateContent);
                std::lock_guard<std::mutex> lock(g_downgradeMutex);
                g_protectionStatus.filesDowngraded = state.value("downgraded", false);
            } catch (...) {}
        }
        SetDowngradeStatus("");
        g_isDowngrading = false;
        return;
    }

    // 4. Parse manifest
    std::string manifestContent = ReadFileToString(tempManifest);
    DeleteFileW(tempManifest.c_str());

    if (manifestContent.empty()) {
        SetDowngradeStatus("");
        g_isDowngrading = false;
        return;
    }

    try {
        json manifest = json::parse(manifestContent);
        std::string targetVersion = manifest.value("target_game_version", "");
        
        // Get platform key
        std::string platformKey;
        switch (platform) {
            case GamePlatform::STEAM:     platformKey = "steam"; break;
            case GamePlatform::EPIC:      platformKey = "epic"; break;
            case GamePlatform::ROCKSTAR:  platformKey = "rockstar"; break;
            default:                      platformKey = "rockstar"; break;
        }

        if (!manifest.contains("platforms") || !manifest["platforms"].contains(platformKey)) {
            SetDowngradeStatus("No downgrade data for " + std::string(PlatformToString(platform)));
            Sleep(3000);
            SetDowngradeStatus("");
            g_isDowngrading = false;
            return;
        }

        auto platformData = manifest["platforms"][platformKey];
        auto files = platformData["files"];

        bool needsDowngrade = false;
        bool alreadyDowngraded = true;
        std::vector<DowngradeFileInfo> filesToDowngrade;

        // 5. Check each file's hash
        SetDowngradeStatus("Verifying game files...");
        for (auto& fileEntry : files) {
            DowngradeFileInfo info;
            info.relativePath = fileEntry.value("path", "");
            info.sha256Current = fileEntry.value("sha256_current", "");
            info.sha256Downgraded = fileEntry.value("sha256_downgraded", "");
            info.downloadUrl = fileEntry.value("download_url", "");
            info.sizeBytes = fileEntry.value("size_bytes", (uint64_t)0);

            std::wstring fullPath = gtaPath + L"\\" + Utf8ToWide(info.relativePath);
            std::string fileHash = ComputeSHA256(fullPath);

            if (fileHash == info.sha256Current) {
                // File is updated (Rockstar's new version) — needs downgrade
                needsDowngrade = true;
                alreadyDowngraded = false;
                filesToDowngrade.push_back(info);
            } else if (fileHash == info.sha256Downgraded) {
                // Already downgraded — good
            } else if (fileHash.empty()) {
                // File not found — skip
            } else {
                // Unknown hash — might be a different version, try downgrade anyway
                needsDowngrade = true;
                alreadyDowngraded = false;
                filesToDowngrade.push_back(info);
            }
        }

        if (alreadyDowngraded || !needsDowngrade) {
            // Already downgraded — just verify protections
            SetDowngradeStatus("Game already downgraded, verifying protections...");
            auto status = ApplyUpdateProtection(platform, gtaPath);
            status.filesDowngraded = true;
            {
                std::lock_guard<std::mutex> lock(g_downgradeMutex);
                g_protectionStatus = status;
            }
            SetDowngradeStatus("");
            g_isDowngrading = false;
            return;
        }

        // 6. Download and replace files
        int totalFiles = (int)filesToDowngrade.size();
        int currentFile = 0;

        for (auto& info : filesToDowngrade) {
            currentFile++;
            std::string progressMsg = "Downgrading (" + std::to_string(currentFile) + "/" + 
                                      std::to_string(totalFiles) + "): " + info.relativePath;
            SetDowngradeStatus(progressMsg);
            g_downgradeProgress = (float)currentFile / (float)totalFiles;

            std::wstring fullPath = gtaPath + L"\\" + Utf8ToWide(info.relativePath);
            std::wstring backupPath = fullPath + L".original.bak";
            std::wstring tempPath = fullPath + L".downgrade.tmp";

            // Backup original file (if not already backed up)
            if (GetFileAttributesW(backupPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
                CopyFileW(fullPath.c_str(), backupPath.c_str(), TRUE);
            }

            // Download downgraded file
            if (!DownloadFileTo(Utf8ToWide(info.downloadUrl), tempPath)) {
                SetDowngradeStatus("Download failed: " + info.relativePath);
                Sleep(3000);
                DeleteFileW(tempPath.c_str());
                continue;
            }

            // Verify downloaded file hash
            std::string dlHash = ComputeSHA256(tempPath);
            if (!info.sha256Downgraded.empty() && dlHash != info.sha256Downgraded) {
                SetDowngradeStatus("Hash mismatch: " + info.relativePath);
                Sleep(2000);
                DeleteFileW(tempPath.c_str());
                continue;
            }

            // Replace: delete current, rename temp to target
            DeleteFileW(fullPath.c_str());
            MoveFileW(tempPath.c_str(), fullPath.c_str());
        }

        // 7. Apply protections
        SetDowngradeStatus("Applying update protections...");
        auto status = ApplyUpdateProtection(platform, gtaPath);
        status.filesDowngraded = true;
        {
            std::lock_guard<std::mutex> lock(g_downgradeMutex);
            g_protectionStatus = status;
        }

        // 8. Save downgrade state
        json state;
        state["downgraded"] = true;
        state["target_version"] = targetVersion;
        state["platform"] = platformKey;
        state["timestamp"] = std::to_string(GetTickCount64());
        WriteStringToFile(GetLauncherDir() + L"\\downgrade_state.json", state.dump(2));

        SetDowngradeStatus("Downgrade complete!");
        Sleep(2000);
        SetDowngradeStatus("");

    } catch (const std::exception& e) {
        SetDowngradeStatus(std::string("Manifest error: ") + e.what());
        Sleep(3000);
        SetDowngradeStatus("");
    }

    g_isDowngrading = false;
    g_downgradeProgress = 0.0f;
}

static void CheckForUpdatesThread()
{
    g_isDownloading = true;
    {
        std::lock_guard<std::mutex> lock(g_downloadMutex);
        g_downloadStatusMsg = "Checking for updates...";
    }

    std::wstring base = GetLauncherDir();
    std::wstring rage = base + L"\\RAGEMP";
    CreateDirectoryW(rage.c_str(), nullptr);

    // 1. Download remote version.txt
    std::wstring remoteVersionUrl = L"https://gist.githubusercontent.com/sharax99/b9207708918b109edf2e2ef5a745cb9c/raw/04c8ac6292254c29145428e687bc9718c51ac02b/gistfile1.txt";
    std::wstring tempVersionFile = rage + L"\\version_remote.txt";
    
    HRESULT hr = URLDownloadToFileW(nullptr, remoteVersionUrl.c_str(), tempVersionFile.c_str(), 0, nullptr);
    if (FAILED(hr)) {
        // Assume no update if we can't reach the server
        std::lock_guard<std::mutex> lock(g_downloadMutex);
        g_downloadStatusMsg = "";
        g_isDownloading = false;
        return;
    }

    std::string remoteVersion = ReadFileToString(tempVersionFile);
    DeleteFileW(tempVersionFile.c_str());

    std::wstring localVersionFile = rage + L"\\version.txt";
    std::string localVersion = ReadFileToString(localVersionFile);

    // Remove whitespace/newlines from versions
    remoteVersion.erase(remoteVersion.find_last_not_of(" \n\r\t") + 1);
    localVersion.erase(localVersion.find_last_not_of(" \n\r\t") + 1);

    if (remoteVersion != localVersion && !remoteVersion.empty()) {
        // Need to update
        {
            std::lock_guard<std::mutex> lock(g_downloadMutex);
            g_downloadStatusMsg = "Downloading update.zip...";
        }

        std::wstring zipUrl = L"https://www.dropbox.com/scl/fi/j4dn4gvp9mempck97d2h6/RAGEMP.zip?rlkey=bjrq4u20k4i26vc0yyv0phzfd&st=qvl2aedv&dl=1";
        std::wstring zipDest = rage + L"\\update.zip";

        std::wstring cmd = L"powershell -NoProfile -NonInteractive -Command \"";
        cmd += L"[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12; ";
        cmd += L"try { Invoke-WebRequest -UseBasicParsing -Uri '";
        cmd += zipUrl;
        cmd += L"' -OutFile '";
        cmd += zipDest;
        cmd += L"'; Expand-Archive -Path '";
        cmd += zipDest;
        cmd += L"' -DestinationPath '";
        cmd += rage;
        cmd += L"' -Force; exit 0 } catch { exit 1 }\"";

        STARTUPINFOW si{}; si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};

        DWORD exitCode = 1;
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, INFINITE);
            GetExitCodeProcess(pi.hProcess, &exitCode);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }

        DeleteFileW(zipDest.c_str());

        if (exitCode == 0) {
            WriteStringToFile(localVersionFile, remoteVersion);
        } else {
            {
                std::lock_guard<std::mutex> lock(g_downloadMutex);
                g_downloadStatusMsg = "Download / Extraction failed!";
            }
            Sleep(3000);
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_downloadMutex);
        g_downloadStatusMsg = "";
    }
    g_isDownloading = false;
}

static bool KillProcessByName(const wchar_t* processName) {
    // Use taskkill for reliable process termination
    std::wstring cmd = L"taskkill /F /IM ";
    cmd += processName;
    cmd += L" /T"; // kill child processes too

    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};

    std::wstring cmdCopy = cmd;
    if (CreateProcessW(nullptr, cmdCopy.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 5000);
        DWORD exitCode = 1;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return exitCode == 0;
    }
    return false;
}

static bool IsProcessRunning(const wchar_t* processName) {
    std::wstring cmd = L"tasklist /FI \"IMAGENAME eq ";
    cmd += processName;
    cmd += L"\" /NH";

    // Create pipes to read output
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE hReadPipe, hWritePipe;
    CreatePipe(&hReadPipe, &hWritePipe, &sa, 0);
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    PROCESS_INFORMATION pi{};

    std::wstring cmdCopy = cmd;
    bool running = false;
    if (CreateProcessW(nullptr, cmdCopy.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hWritePipe);
        char buf[4096] = {0};
        DWORD bytesRead;
        ReadFile(hReadPipe, buf, sizeof(buf) - 1, &bytesRead, nullptr);
        WaitForSingleObject(pi.hProcess, 3000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        // If the process name appears in output, it's running
        std::string output(buf);
        std::string needle = WideToUtf8(std::wstring(processName));
        // Case-insensitive search
        std::transform(output.begin(), output.end(), output.begin(), ::tolower);
        std::transform(needle.begin(), needle.end(), needle.begin(), ::tolower);
        running = (output.find(needle) != std::string::npos);
    } else {
        CloseHandle(hWritePipe);
    }
    CloseHandle(hReadPipe);
    return running;
}

// Add firewall rules to block platform launchers from phoning home
static void BlockPlatformNetworkAccess(GamePlatform platform) {
    // Block Rockstar Social Club / Launcher from communicating
    // These are critical: they trigger version checks and auto-updates
    const wchar_t* processesToBlock[] = {
        L"SocialClubHelper.exe",
        L"RockstarService.exe", 
        L"LauncherPatcher.exe",
        L"Launcher.exe",        // Rockstar Games Launcher
    };

    for (auto proc : processesToBlock) {
        std::wstring ruleName = std::wstring(L"SharaxStealth_") + proc;
        if (IsFirewallRuleExists(ruleName)) continue;

        // Find full path of the process (if running)
        // We block by process name which is sufficient
        std::wstring cmd = L"netsh advfirewall firewall add rule "
            L"name=\"" + ruleName + L"\" "
            L"dir=out "
            L"action=block "
            L"program=\"%ProgramFiles%\\Rockstar Games\\Launcher\\" + std::wstring(proc) + L"\" "
            L"enable=yes "
            L"profile=any";
        RunElevatedCommand(cmd);
    }

    // Platform-specific blocks
    if (platform == GamePlatform::STEAM) {
        // Block Steam from checking game file integrity 
        std::wstring ruleName = L"SharaxStealth_SteamService";
        if (!IsFirewallRuleExists(ruleName)) {
            std::wstring cmd = L"netsh advfirewall firewall add rule "
                L"name=\"" + ruleName + L"\" "
                L"dir=out "
                L"action=block "
                L"program=\"%ProgramFiles(x86)%\\Steam\\bin\\cef\\cef.win7x64\\steamwebhelper.exe\" "
                L"enable=yes "
                L"profile=any";
            RunElevatedCommand(cmd);
        }
    }

    if (platform == GamePlatform::EPIC) {
        // Block Epic's background services 
        std::wstring ruleName = L"SharaxStealth_EpicService";
        if (!IsFirewallRuleExists(ruleName)) {
            std::wstring cmd = L"netsh advfirewall firewall add rule "
                L"name=\"" + ruleName + L"\" "
                L"dir=out "
                L"action=block "
                L"program=\"%ProgramFiles(x86)%\\Epic Games\\Launcher\\Engine\\Binaries\\Win64\\EpicGamesLauncher.exe\" "
                L"enable=yes "
                L"profile=any";
            RunElevatedCommand(cmd);
        }
    }
}

static void StealthKillPlatformProcesses(GamePlatform platform) {
    // Always kill Rockstar components — they exist on ALL platforms
    KillProcessByName(L"GTAVLauncher.exe");
    KillProcessByName(L"SocialClubHelper.exe");
    KillProcessByName(L"RockstarService.exe");
    KillProcessByName(L"LauncherPatcher.exe");
    KillProcessByName(L"PlayGTAV.exe");
    
    // Platform-specific cleanup
    switch (platform) {
        case GamePlatform::STEAM:
            // Don't kill Steam entirely — just stop its game monitoring
            // Steam uses SteamService.exe for updates
            KillProcessByName(L"SteamService.exe");
            break;
        case GamePlatform::EPIC:
            // Kill Epic's background processes that monitor game state
            KillProcessByName(L"EpicGamesLauncher.exe");
            KillProcessByName(L"EpicWebHelper.exe");
            break;
        case GamePlatform::ROCKSTAR:
            // Kill the Rockstar Games Launcher
            KillProcessByName(L"Launcher.exe");
            break;
        default:
            break;
    }

    Sleep(500);
}

static void CleanupStealthFirewallRules() {
    const wchar_t* ruleNames[] = {
        L"SharaxStealth_SocialClubHelper.exe",
        L"SharaxStealth_RockstarService.exe",
        L"SharaxStealth_LauncherPatcher.exe",
        L"SharaxStealth_Launcher.exe",
        L"SharaxStealth_SteamService",
        L"SharaxStealth_EpicService",
    };
    for (auto rule : ruleNames) {
        std::wstring cmd = L"netsh advfirewall firewall delete rule name=\"";
        cmd += rule;
        cmd += L"\"";
        RunElevatedCommand(cmd);
    }
}

static void PrepareStealth(GamePlatform platform) {
    // NOTE: We do NOT use firewall blocking — it breaks RAGEMP's 
    // Social Club authentication. Instead we use passive protections only.

    // 1. Set Social Club offline mode via registry
    SetSCOfflineMode();

    // 2. Platform-specific manifest protection (read-only lock)
    //    This prevents the platform from modifying game files
    switch (platform) {
        case GamePlatform::STEAM:
            ProtectSteamManifest();
            break;
        case GamePlatform::EPIC: {
            std::wstring gtaPath = FindGTAPath();
            if (!gtaPath.empty())
                ProtectEpicInstallation(gtaPath);
            break;
        }
        default:
            break;
    }
}

static bool SpawnUpdater(const std::wstring& ragempDir)
{
    std::wstring exe = ragempDir + L"\\updater.exe";

    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;

    std::wstring cmdline = L"\"" + exe + L"\"";

    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    BOOL ok = CreateProcessW(
        nullptr,
        cmdline.data(),
        nullptr, nullptr, FALSE,
        0,
        nullptr,
        ragempDir.c_str(),
        &si, &pi);

    if (ok) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return ok != FALSE;
}


static void OnConnect()
{
    if (g_app.server[0] == 0) {
        SetStatus("Server cannot be empty", true);
        return;
    }
    if (g_app.port[0] == 0) {
        SetStatus("Port cannot be empty", true);
        return;
    }

    if (!WriteRegistry("launch2.ip", g_app.server)) {
        SetStatus("Failed to write launch2.ip to registry", true);
        return;
    }
    if (!WriteRegistry("launch2.port", g_app.port)) {
        SetStatus("Failed to write launch2.port to registry", true);
        return;
    }

    // === STEALTH MODE: Kill platform launchers before game launch ===
    SetStatus("Preparing stealth launch...", false);
    
    std::wstring gtaPath = FindGTAPath();
    GamePlatform platform = GamePlatform::UNKNOWN;
    if (!gtaPath.empty()) {
        platform = DetectGamePlatform(gtaPath);
    }
    
    // Activate stealth: block network + kill processes + protect manifests
    PrepareStealth(platform);

    // === Launch RAGEMP ===
    std::wstring base = GetLauncherDir();
    std::wstring rage = base + L"\\RAGEMP";

    if (!SpawnUpdater(rage)) {
        SetStatus("Cannot find or launch RAGEMP\\updater.exe", true);
        return;
    }

    SetStatus("Launching game (stealth)...", false);
}

static void SetStatus(const std::string& msg, bool isError)
{
    g_app.statusMsg     = msg;
    g_app.statusIsError = isError;
    g_app.statusUntil   = GetTickCount() + 4000;
}

static void ApplyDarkTheme()
{
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 0.0f;
    s.ChildRounding     = 6.0f;
    s.FrameRounding     = 4.0f;
    s.GrabRounding      = 4.0f;
    s.PopupRounding     = 4.0f;
    s.ScrollbarRounding = 4.0f;
    s.TabRounding       = 4.0f;
    s.FrameBorderSize   = 0.0f;
    s.WindowBorderSize  = 0.0f;
    s.WindowPadding     = ImVec2(0, 0);
    s.FramePadding      = ImVec2(10, 8);
    s.ItemSpacing       = ImVec2(8, 8);

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]        = ImVec4(0.06f, 0.06f, 0.07f, 1.00f);
    c[ImGuiCol_ChildBg]         = ImVec4(0.10f, 0.10f, 0.11f, 1.00f);  
    c[ImGuiCol_Text]            = ImVec4(0.92f, 0.92f, 0.93f, 1.00f);
    c[ImGuiCol_TextDisabled]    = ImVec4(0.45f, 0.45f, 0.48f, 1.00f);
    c[ImGuiCol_FrameBg]         = ImVec4(0.04f, 0.04f, 0.05f, 1.00f);
    c[ImGuiCol_FrameBgHovered]  = ImVec4(0.07f, 0.07f, 0.08f, 1.00f);
    c[ImGuiCol_FrameBgActive]   = ImVec4(0.09f, 0.09f, 0.10f, 1.00f);
    c[ImGuiCol_Button]          = ImVec4(0.16f, 0.16f, 0.18f, 1.00f);
    c[ImGuiCol_ButtonHovered]   = ImVec4(0.22f, 0.22f, 0.24f, 1.00f);
    c[ImGuiCol_ButtonActive]    = ImVec4(0.28f, 0.28f, 0.30f, 1.00f);
    c[ImGuiCol_Border]          = ImVec4(0.18f, 0.18f, 0.20f, 1.00f);
    c[ImGuiCol_Separator]       = ImVec4(0.18f, 0.18f, 0.20f, 1.00f);
}

static ImFont* g_fontTitle = nullptr;
static ImFont* g_fontBody  = nullptr;
static ImFont* g_fontSmall = nullptr;
static ImFont* g_fontBig   = nullptr;

static void LoadFonts()
{
    ImGuiIO& io = ImGui::GetIO();
    const char* segoe = "C:\\Windows\\Fonts\\segoeui.ttf";
    const char* segoeBold = "C:\\Windows\\Fonts\\segoeuib.ttf";

    if (GetFileAttributesA(segoe) != INVALID_FILE_ATTRIBUTES) {
        static const ImWchar ranges[] = {
            0x0020, 0x00FF, // Basic Latin + Latin Supplement
            0x0100, 0x017F, // Latin Extended-A (includes Turkish)
            0x0400, 0x052F, // Cyrillic + Cyrillic Supplement
            0x2DE0, 0x2DFF, // Cyrillic Extended-A
            0xA640, 0xA69F, // Cyrillic Extended-B
            0
        };
        g_fontBody  = io.Fonts->AddFontFromFileTTF(segoe, 15.0f, nullptr, ranges);
        g_fontSmall = io.Fonts->AddFontFromFileTTF(segoe, 12.0f, nullptr, ranges);
        g_fontBig   = io.Fonts->AddFontFromFileTTF(segoe, 22.0f, nullptr, ranges);
        if (GetFileAttributesA(segoeBold) != INVALID_FILE_ATTRIBUTES)
            g_fontTitle = io.Fonts->AddFontFromFileTTF(segoeBold, 16.0f, nullptr, ranges);
        else
            g_fontTitle = g_fontBody;
    }
}

static void DrawTitleBar()
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImVec2(0, 0);
    ImVec2 p1 = ImVec2((float)WINDOW_W, (float)TITLE_BAR_H);

    dl->AddRectFilled(p0, p1, IM_COL32(15, 15, 17, 255));

    if (g_fontTitle) ImGui::PushFont(g_fontTitle);
    ImVec2 textSize = ImGui::CalcTextSize("RAGE Launcher - github.com/winnerchester");
    dl->AddText(ImVec2(14, (TITLE_BAR_H - textSize.y) * 0.5f),
                IM_COL32(230, 230, 235, 255), "RAGE Launcher - github.com/winnerchester");
    if (g_fontTitle) ImGui::PopFont();

    const float btnSize = (float)TITLE_BAR_H;
    ImGui::SetCursorPos(ImVec2(WINDOW_W - btnSize, 0));
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.78f, 0.20f, 0.20f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.55f, 0.15f, 0.15f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    if (ImGui::Button("X##close", ImVec2(btnSize, btnSize))) {
        g_running = false;
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
}

static void DrawUI()
{
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)WINDOW_W, (float)WINDOW_H));

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::Begin("##root", nullptr, flags);

    DrawTitleBar();

    const float kSidePad = 28.0f;
    const float kFieldW  = WINDOW_W - kSidePad * 2.0f;
    const float kBtnW    = 160.0f;

    auto CenterX = [&](float w) {
        ImGui::SetCursorPosX((WINDOW_W - w) * 0.5f);
    };

    ImGui::SetCursorPosY((float)TITLE_BAR_H + 28.0f);
    if (g_fontBig) ImGui::PushFont(g_fontBig);
    {
        const char* t = Tr("Connect to server", "Sunucuya bağlan", "Подключение к серверу");
        CenterX(ImGui::CalcTextSize(t).x);
        ImGui::TextUnformatted(t);
    }
    if (g_fontBig) ImGui::PopFont();

    if (g_fontSmall) ImGui::PushFont(g_fontSmall);
    {
        const char* s = Tr("Enter the address you want to join", "Katılmak istediğiniz adresi girin", "Введите адрес для подключения");
        CenterX(ImGui::CalcTextSize(s).x);
        ImGui::TextColored(ImVec4(0.50f, 0.50f, 0.55f, 1.0f), "%s", s);
    }
    if (g_fontSmall) ImGui::PopFont();

    ImGui::Dummy(ImVec2(0.0f, 18.0f));

    ImGui::SetCursorPosX(kSidePad);
    ImGui::SetNextItemWidth(kFieldW);
    ImGui::InputTextWithHint("##server", Tr("hostname or IP", "sunucu adresi veya IP", "адрес сервера или IP"),
                             g_app.server, sizeof(g_app.server));

    ImGui::SetCursorPosX(kSidePad);
    ImGui::SetNextItemWidth(kFieldW);
    ImGui::InputTextWithHint("##port", Tr("port (default 22005)", "port (varsayılan 22005)", "порт (по умолчанию 22005)"),
                             g_app.port, sizeof(g_app.port),
                             ImGuiInputTextFlags_CharsDecimal);

    ImGui::Dummy(ImVec2(0.0f, 10.0f));

    CenterX(kBtnW);
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.14f, 0.14f, 0.16f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.22f, 0.24f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.28f, 0.28f, 0.30f, 1.0f));
    
    ImGui::BeginDisabled(g_isDownloading.load());
    if (ImGui::Button(Tr("Connect", "Bağlan", "Подключиться"), ImVec2(kBtnW, 36.0f))) {
        OnConnect();
    }
    ImGui::EndDisabled();
    
    ImGui::PopStyleColor(3);

    std::string dlMsg;
    {
        std::lock_guard<std::mutex> lock(g_downloadMutex);
        dlMsg = g_downloadStatusMsg;
    }
    if (!dlMsg.empty()) {
        const char* disp = dlMsg.c_str();
        if (dlMsg == "Checking for updates...") disp = Tr("Checking for updates...", "Güncellemeler kontrol ediliyor...", "Проверка обновлений...");
        else if (dlMsg == "Downloading update.zip...") disp = Tr("Downloading update.zip...", "update.zip indiriliyor...", "Загрузка update.zip...");
        else if (dlMsg == "Extracting update.zip...") disp = Tr("Extracting update.zip...", "update.zip çıkarılıyor...", "Распаковка update.zip...");
        else if (dlMsg == "Download / Extraction failed!") disp = Tr("Download / Extraction failed!", "İndirme / Çıkarma başarısız!", "Ошибка загрузки / распаковки!");
        
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        if (g_fontSmall) ImGui::PushFont(g_fontSmall);
        CenterX(ImGui::CalcTextSize(disp).x);
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.2f, 1.0f), "%s", disp);
        if (g_fontSmall) ImGui::PopFont();
    }

    if (!g_app.statusMsg.empty() && GetTickCount() < g_app.statusUntil) {
        const char* disp = g_app.statusMsg.c_str();
        if (g_app.statusMsg == "Server cannot be empty") disp = Tr("Server cannot be empty", "Sunucu boş olamaz", "Сервер не может быть пустым");
        else if (g_app.statusMsg == "Port cannot be empty") disp = Tr("Port cannot be empty", "Port boş olamaz", "Порт не может быть пустым");
        else if (g_app.statusMsg == "Failed to write launch2.ip to registry") disp = Tr("Failed to write launch2.ip to registry", "Kayıt defterine launch2.ip yazılamadı", "Не удалось записать launch2.ip в реестр");
        else if (g_app.statusMsg == "Failed to write launch2.port to registry") disp = Tr("Failed to write launch2.port to registry", "Kayıt defterine launch2.port yazılamadı", "Не удалось записать launch2.port в реестр");
        else if (g_app.statusMsg == "Cannot find or launch RAGEMP\\updater.exe") disp = Tr("Cannot find or launch RAGEMP\\updater.exe", "RAGEMP\\updater.exe bulunamadı veya başlatılamadı", "Не удалось найти или запустить RAGEMP\\updater.exe");
        else if (g_app.statusMsg == "Launching game...") disp = Tr("Launching game...", "Oyun başlatılıyor...", "Запуск игры...");

        ImGui::SetCursorPosX(kSidePad);
        if (g_fontSmall) ImGui::PushFont(g_fontSmall);
        ImVec4 col = g_app.statusIsError
                         ? ImVec4(0.90f, 0.40f, 0.40f, 1.0f)
                         : ImVec4(0.55f, 0.80f, 0.55f, 1.0f);
        ImGui::TextColored(col, "%s", disp);
        if (g_fontSmall) ImGui::PopFont();
    }

    // ==================== DOWNGRADE PROTECTION STATUS PANEL ====================
    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    
    // Separator line
    ImDrawList* panelDL = ImGui::GetWindowDrawList();
    float sepY = ImGui::GetCursorScreenPos().y;
    panelDL->AddLine(ImVec2(kSidePad, sepY), ImVec2(WINDOW_W - kSidePad, sepY), 
                     IM_COL32(40, 40, 45, 255), 1.0f);
    ImGui::Dummy(ImVec2(0.0f, 8.0f));

    // Panel title
    if (g_fontTitle) ImGui::PushFont(g_fontTitle);
    ImGui::SetCursorPosX(kSidePad);
    ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.80f, 1.0f), "%s", 
        Tr("Downgrade Protection", "Downgrade Koruma", "\xd0\x97\xd0\xb0\xd1\x89\xd0\xb8\xd1\x82\xd0\xb0 \xd0\xb4\xd0\xb0\xd1\x83\xd0\xbd\xd0\xb3\xd1\x80\xd0\xb5\xd0\xb9\xd0\xb4\xd0\xb0"));
    if (g_fontTitle) ImGui::PopFont();

    if (g_fontSmall) ImGui::PushFont(g_fontSmall);

    // Downgrade status message (if active)
    std::string dgMsg;
    {
        std::lock_guard<std::mutex> lock(g_downgradeMutex);
        dgMsg = g_downgradeStatusMsg;
    }
    if (!dgMsg.empty()) {
        ImGui::SetCursorPosX(kSidePad);
        ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1.0f), "%s", dgMsg.c_str());
        
        // Progress bar during downgrade
        if (g_downgradeProgress > 0.0f && g_downgradeProgress < 1.0f) {
            ImGui::SetCursorPosX(kSidePad);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.0f, 0.82f, 0.70f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.08f, 0.08f, 0.09f, 1.0f));
            ImGui::ProgressBar(g_downgradeProgress, ImVec2(kFieldW, 4.0f), "");
            ImGui::PopStyleColor(2);
        }
    }

    // Protection status indicators
    ProtectionStatus ps;
    {
        std::lock_guard<std::mutex> lock(g_downgradeMutex);
        ps = g_protectionStatus;
    }

    auto StatusLine = [&](const char* label, bool ok) {
        ImGui::SetCursorPosX(kSidePad + 8.0f);
        if (ok)
            ImGui::TextColored(ImVec4(0.30f, 0.85f, 0.45f, 1.0f), "[OK]");
        else
            ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.60f, 1.0f), "[--]");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.70f, 1.0f), "%s", label);
    };

    // Platform info
    if (ps.platform != GamePlatform::UNKNOWN) {
        ImGui::SetCursorPosX(kSidePad + 8.0f);
        ImGui::TextColored(ImVec4(0.50f, 0.70f, 0.90f, 1.0f), "%s: %s", 
            Tr("Platform", "Platform", "\xd0\x9f\xd0\xbb\xd0\xb0\xd1\x82\xd1\x84\xd0\xbe\xd1\x80\xd0\xbc\xd0\xb0"), 
            ps.platformName.c_str());
    }

    // File status
    StatusLine(Tr("Game files", "Oyun dosyalari", "\xd0\xa4\xd0\xb0\xd0\xb9\xd0\xbb\xd1\x8b \xd0\xb8\xd0\xb3\xd1\x80\xd1\x8b"), ps.filesDowngraded);
    
    // Social Club firewall
    StatusLine(Tr("Social Club firewall", "Social Club guvenlik duvari", "\xd0\x91\xd1\x80\xd0\xb0\xd0\xbd\xd0\xb4\xd0\xbc\xd0\xb0\xd1\x83\xd1\x8d\xd1\x80 Social Club"), ps.socialClubBlocked);
    
    // SC Offline mode
    StatusLine(Tr("Offline mode", "Cevrimdisi mod", "\xd0\x9e\xd1\x84\xd1\x84\xd0\xbb\xd0\xb0\xd0\xb9\xd0\xbd \xd1\x80\xd0\xb5\xd0\xb6\xd0\xb8\xd0\xbc"), ps.scOfflineMode);

    // Platform-specific
    if (ps.platform == GamePlatform::STEAM) {
        StatusLine(Tr("Steam manifest locked", "Steam manifest kilitli", "\xd0\x9c\xd0\xb0\xd0\xbd\xd0\xb8\xd1\x84\xd0\xb5\xd1\x81\xd1\x82 Steam \xd0\xb7\xd0\xb0\xd0\xb1\xd0\xbb\xd0\xbe\xd0\xba\xd0\xb8\xd1\x80\xd0\xbe\xd0\xb2\xd0\xb0\xd0\xbd"), ps.steamManifestLocked);
    } else if (ps.platform == GamePlatform::EPIC) {
        StatusLine(Tr("Epic manifest locked", "Epic manifest kilitli", "\xd0\x9c\xd0\xb0\xd0\xbd\xd0\xb8\xd1\x84\xd0\xb5\xd1\x81\xd1\x82 Epic \xd0\xb7\xd0\xb0\xd0\xb1\xd0\xbb\xd0\xbe\xd0\xba\xd0\xb8\xd1\x80\xd0\xbe\xd0\xb2\xd0\xb0\xd0\xbd"), ps.epicManifestLocked);
    }

    if (g_fontSmall) ImGui::PopFont();
    // ==================== END PROTECTION PANEL ====================

    if (g_fontSmall) ImGui::PushFont(g_fontSmall);
    const char* foot = "winner was here";
    ImVec2 fsz = ImGui::CalcTextSize(foot);
    ImGui::SetCursorPos(ImVec2((WINDOW_W - fsz.x) * 0.5f,
                               WINDOW_H - fsz.y - 14.0f));
    ImGui::TextColored(ImVec4(0.35f, 0.35f, 0.40f, 1.0f), "%s", foot);

    const char* footerText = "make rage:mp playable again";
    ImVec2 fsz2 = ImGui::CalcTextSize(footerText);
    ImGui::SetCursorPos(ImVec2((WINDOW_W - fsz2.x) * 0.5f,
                               WINDOW_H - fsz.y - fsz2.y - 18.0f));
    ImGui::TextColored(ImVec4(0.35f, 0.35f, 0.40f, 1.0f), "%s", footerText);

    const char* ver = "v2.0.0";
    ImVec2 vsz = ImGui::CalcTextSize(ver);
    ImGui::SetCursorPos(ImVec2(WINDOW_W - vsz.x - 8.0f, WINDOW_H - vsz.y - 8.0f));
    ImGui::TextColored(ImVec4(0.35f, 0.35f, 0.40f, 1.0f), "%s", ver);
    
    // Language Toggle
    ImGui::SetCursorPos(ImVec2(WINDOW_W - 80.0f, TITLE_BAR_H + 8.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, g_app.lang == Language::EN ? ImVec4(1,1,1,1) : ImVec4(0.5f,0.5f,0.5f,1));
    ImGui::Text("EN");
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (ImGui::IsItemClicked()) { g_app.lang = Language::EN; WriteRegistry("launcher_lang", "EN"); }
    ImGui::PopStyleColor();
    
    ImGui::SameLine(); ImGui::TextColored(ImVec4(0.35f, 0.35f, 0.40f, 1.0f), "|"); ImGui::SameLine();
    
    ImGui::PushStyleColor(ImGuiCol_Text, g_app.lang == Language::TR ? ImVec4(1,1,1,1) : ImVec4(0.5f,0.5f,0.5f,1));
    ImGui::Text("TR");
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (ImGui::IsItemClicked()) { g_app.lang = Language::TR; WriteRegistry("launcher_lang", "TR"); }
    ImGui::PopStyleColor();

    ImGui::SameLine(); ImGui::TextColored(ImVec4(0.35f, 0.35f, 0.40f, 1.0f), "|"); ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Text, g_app.lang == Language::RU ? ImVec4(1,1,1,1) : ImVec4(0.5f,0.5f,0.5f,1));
    ImGui::Text("RU");
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (ImGui::IsItemClicked()) { g_app.lang = Language::RU; WriteRegistry("launcher_lang", "RU"); }
    ImGui::PopStyleColor();

    if (g_fontSmall) ImGui::PopFont();

    ImGui::End();
}

static bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount         = 2;
    sd.BufferDesc.Width    = 0;
    sd.BufferDesc.Height   = 0;
    sd.BufferDesc.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator   = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags                = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage          = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow         = hWnd;
    sd.SampleDesc.Count     = 1;
    sd.SampleDesc.Quality   = 0;
    sd.Windowed             = TRUE;
    sd.SwapEffect           = DXGI_SWAP_EFFECT_DISCARD;

    UINT createFlags = 0;
    D3D_FEATURE_LEVEL fl;
    D3D_FEATURE_LEVEL flArray[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createFlags,
        flArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
        &g_pd3dDevice, &fl, &g_pd3dContext);

    if (FAILED(hr)) return false;

    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain)  { g_pSwapChain->Release();  g_pSwapChain  = nullptr; }
    if (g_pd3dContext) { g_pd3dContext->Release(); g_pd3dContext = nullptr; }
    if (g_pd3dDevice)  { g_pd3dDevice->Release();  g_pd3dDevice  = nullptr; }
}

static void CreateRenderTarget()
{
    ID3D11Texture2D* backBuf = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuf));
    if (backBuf) {
        g_pd3dDevice->CreateRenderTargetView(backBuf, nullptr, &g_mainRTV);
        backBuf->Release();
    }
}

static void CleanupRenderTarget()
{
    if (g_mainRTV) { g_mainRTV->Release(); g_mainRTV = nullptr; }
}

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            g_resizeWidth  = (UINT)LOWORD(lParam);
            g_resizeHeight = (UINT)HIWORD(lParam);
        }
        return 0;

    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_KEYMENU) return 0;  
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_NCCALCSIZE:
        if (wParam == TRUE) return 0;
        break;

    case WM_NCHITTEST: {
        POINT p = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hWnd, &p);
        if (p.y >= 0 && p.y < TITLE_BAR_H && p.x < WINDOW_W - TITLE_BAR_H)
            return HTCAPTION;
        return HTCLIENT;
    }
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"RageLauncherCls";
    wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(101));
    wc.hIconSm       = LoadIconW(hInst, MAKEINTRESOURCEW(101));
    RegisterClassExW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int x  = (sw - WINDOW_W) / 2;
    int y  = (sh - WINDOW_H) / 2;

    g_hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"RAGE Launcher - github.com/winnerchester",
        WS_POPUP,                            
        x, y, WINDOW_W, WINDOW_H,
        nullptr, nullptr, hInst, nullptr);

    {
        DWM_WINDOW_CORNER_PREFERENCE pref = DWMWCP_ROUND;
        DwmSetWindowAttribute(g_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                              &pref, sizeof(pref));
    }

    std::string savedLang = ReadRegistry("launcher_lang");
    if (savedLang == "TR") g_app.lang = Language::TR;
    else if (savedLang == "RU") g_app.lang = Language::RU;
    else g_app.lang = Language::EN;

    if (!CreateDeviceD3D(g_hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, hInst);
        MessageBoxW(nullptr, L"Failed to initialize D3D11", L"RAGE Launcher", MB_ICONERROR);
        return 1;
    }

    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);

    std::thread(CheckForUpdatesThread).detach();
    std::thread(DowngradeCheckThread).detach();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    LoadFonts();
    ApplyDarkTheme();

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dContext);

    MSG msg{};
    while (g_running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) g_running = false;
        }
        if (!g_running) break;

        if (g_resizeWidth && g_resizeHeight) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_resizeWidth, g_resizeHeight,
                                        DXGI_FORMAT_UNKNOWN, 0);
            g_resizeWidth = g_resizeHeight = 0;
            CreateRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        DrawUI();

        ImGui::Render();
        const float clear[4] = { 0.06f, 0.06f, 0.07f, 1.00f };
        g_pd3dContext->OMSetRenderTargets(1, &g_mainRTV, nullptr);
        g_pd3dContext->ClearRenderTargetView(g_mainRTV, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    // Cleanup stealth firewall rules on exit
    CleanupStealthFirewallRules();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(g_hwnd);
    UnregisterClassW(wc.lpszClassName, hInst);
    return 0;
}
