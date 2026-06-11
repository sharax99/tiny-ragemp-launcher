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

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

static HWND                    g_hwnd               = nullptr;
static ID3D11Device*           g_pd3dDevice         = nullptr;
static ID3D11DeviceContext*    g_pd3dContext        = nullptr;
static IDXGISwapChain*         g_pSwapChain         = nullptr;
static ID3D11RenderTargetView* g_mainRTV            = nullptr;
static bool                    g_running            = true;
static UINT                    g_resizeWidth        = 0;
static UINT                    g_resizeHeight       = 0;

constexpr int    WINDOW_W       = 420;
constexpr int    WINDOW_H       = 380;
constexpr int    TITLE_BAR_H    = 32;
constexpr int    RESIZE_BORDER  = 0;   

enum class Language { EN, TR, RU };

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
        cmd += L"Invoke-WebRequest -UseBasicParsing -Uri '";
        cmd += zipUrl;
        cmd += L"' -OutFile '";
        cmd += zipDest;
        cmd += L"'; if ($?) { Expand-Archive -Path '";
        cmd += zipDest;
        cmd += L"' -DestinationPath '";
        cmd += rage;
        cmd += L"' -Force } else { exit 1 }\"";

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

    std::wstring base = GetLauncherDir();
    std::wstring rage = base + L"\\RAGEMP";

    if (!SpawnUpdater(rage)) {
        SetStatus("Cannot find or launch RAGEMP\\updater.exe", true);
        return;
    }

    SetStatus("Launching game...", false);
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

    const char* ver = "v1.0.0";
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

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(g_hwnd);
    UnregisterClassW(wc.lpszClassName, hInst);
    return 0;
}
