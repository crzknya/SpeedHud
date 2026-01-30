#include <Windows.h>
#include <thread>
#include <d3d11.h>
#include <dxgi.h>
#include <string>
#include <sstream>
#include <fstream>
#include <atomic>
#include <vector>      
#include <Psapi.h>     
#include <filesystem> 
#include "MinHook.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "Psapi.lib") 

// ----------------------------
// Globals
// ----------------------------
typedef HRESULT(__stdcall* PresentFn)(IDXGISwapChain*, UINT, UINT);
PresentFn oPresent = nullptr;

ID3D11Device* g_Device = nullptr;
ID3D11DeviceContext* g_Context = nullptr;
ID3D11RenderTargetView* g_RTV = nullptr;

std::atomic<bool> g_ImGuiInitialized(false);

// HUD
constexpr uintptr_t g_SpeedAddress = 0x2A869AC;

// HUD state
bool g_ShowSpeed = true;
bool g_ShowDistance = true;
float g_DistanceMeters = 0.0f;

// Menu and scale
bool g_ShowMenu = false;
float g_HudFontScale = 3.0f;
float g_MenuFontScale = 3.0f;
ImVec2 g_HudPos = ImVec2(10, 10);
bool g_HudPosInitialized = false;

// INI
std::string g_IniPath;
std::string g_DllDirectory;  // Added to store DLL directory
std::string g_FontFile;      // Font file name from config

// Cached module handle
HMODULE g_GameModule = nullptr;

// Input debouncing
constexpr float INPUT_DEBOUNCE_TIME = 0.15f;
float g_LastInputTime = 0.0f;

// ----------------------------
// TIME CONTROL GLOBALS
// ----------------------------
// Time Of Day patterns and patches
constexpr const char* TODTickDisablePattern = "F3 41 0F 11 95 38 01 00 00";
constexpr BYTE TODTickDisablePatch[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };

constexpr const char* TODTickRevertPattern = "90 90 90 90 90 90 90 90 90 F3 0F 10 05";
constexpr BYTE TODTickRevertPatch[] = { 0xF3, 0x41, 0x0F, 0x11, 0x95, 0x38, 0x01, 0x00, 0x00 };

constexpr uintptr_t TODOffset = 0x138;
constexpr uintptr_t TODPointer = 0x02A588F8;

// Time control state
bool g_IsTimePassing = true;
bool g_IsTimelapsing = false;
std::atomic<bool> g_TimelapseThreadRunning(false);
std::thread g_TimelapseThread;

// Hotkey state (using VK codes for simplicity)
constexpr int HOTKEY_FREEZE_TIME = VK_F1;      // F1 - Toggle freeze time
constexpr int HOTKEY_TIMELAPSE = VK_F2;         // F2 - Toggle timelapse

// ----------------------------
// FORWARD DECLARATIONS
// ----------------------------
void StopTimelapse();
void ToggleFreezeTime();
void ToggleTimelapse();

// ----------------------------
// UTILITY FUNCTIONS
// ----------------------------
inline void CreateRenderTarget(IDXGISwapChain* swapChain)
{
    if (g_RTV)
    {
        g_RTV->Release();
        g_RTV = nullptr;
    }

    ID3D11Texture2D* backBuffer = nullptr;
    if (SUCCEEDED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))))
    {
        g_Device->CreateRenderTargetView(backBuffer, nullptr, &g_RTV);
        backBuffer->Release();
    }
}

inline void ClampHudPosition(ImVec2& pos, const ImVec2& textSize, const ImVec2& screenSize)
{
    pos.x = max(0.0f, min(pos.x, screenSize.x - textSize.x));
    pos.y = max(0.0f, min(pos.y, screenSize.y - textSize.y));
}

void CreateDefaultConfig()
{
    std::ofstream ini(g_IniPath);
    if (ini.is_open())
    {
        ini << "[HUD]\n"
            "PosX=10\n"
            "PosY=10\n"
            "Scale=3.0\n"
            "ShowSpeed=1\n"
            "ShowDistance=1\n"
            "FontFile=\n";
    }
}

void LoadConfig()
{
    std::ifstream f(g_IniPath);
    if (!f.good())
        CreateDefaultConfig();

    char buffer[256];

    GetPrivateProfileStringA("HUD", "PosX", "10", buffer, sizeof(buffer), g_IniPath.c_str());
    g_HudPos.x = static_cast<float>(atof(buffer));

    GetPrivateProfileStringA("HUD", "PosY", "10", buffer, sizeof(buffer), g_IniPath.c_str());
    g_HudPos.y = static_cast<float>(atof(buffer));

    GetPrivateProfileStringA("HUD", "Scale", "3.0", buffer, sizeof(buffer), g_IniPath.c_str());
    g_HudFontScale = static_cast<float>(atof(buffer));

    GetPrivateProfileStringA("HUD", "ShowSpeed", "1", buffer, sizeof(buffer), g_IniPath.c_str());
    g_ShowSpeed = (buffer[0] == '1');

    GetPrivateProfileStringA("HUD", "ShowDistance", "1", buffer, sizeof(buffer), g_IniPath.c_str());
    g_ShowDistance = (buffer[0] == '1');

    GetPrivateProfileStringA("HUD", "FontFile", "", buffer, sizeof(buffer), g_IniPath.c_str());
    g_FontFile = buffer;
}

void SaveConfig()
{
    char buffer[32];

    snprintf(buffer, sizeof(buffer), "%.1f", g_HudPos.x);
    WritePrivateProfileStringA("HUD", "PosX", buffer, g_IniPath.c_str());

    snprintf(buffer, sizeof(buffer), "%.1f", g_HudPos.y);
    WritePrivateProfileStringA("HUD", "PosY", buffer, g_IniPath.c_str());

    snprintf(buffer, sizeof(buffer), "%.2f", g_HudFontScale);
    WritePrivateProfileStringA("HUD", "Scale", buffer, g_IniPath.c_str());

    WritePrivateProfileStringA("HUD", "ShowSpeed", g_ShowSpeed ? "1" : "0", g_IniPath.c_str());
    WritePrivateProfileStringA("HUD", "ShowDistance", g_ShowDistance ? "1" : "0", g_IniPath.c_str());
}

inline bool HandleDebouncedInput(int vkey, float currentTime)
{
    if ((GetAsyncKeyState(vkey) & 1) && (currentTime - g_LastInputTime > INPUT_DEBOUNCE_TIME))
    {
        g_LastInputTime = currentTime;
        return true;
    }
    return false;
}

// ----------------------------
// FONT LOADING FUNCTIONS
// ----------------------------

// Find TTF file based on config or search directory
std::string FindTTFFile(const std::string& directory)
{
    namespace fs = std::filesystem;

    // Create path to speedfonts folder
    std::string fontDir = directory + "\\speedfonts";

    // First, check if a specific font is specified in config
    if (!g_FontFile.empty())
    {
        // Build full path in speedfonts folder
        std::string fullPath = fontDir + "\\" + g_FontFile;

        try
        {
            if (fs::exists(fullPath) && fs::is_regular_file(fullPath))
            {
                std::string extension = fs::path(fullPath).extension().string();
                std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

                if (extension == ".ttf")
                {
                    return fullPath;
                }
            }
        }
        catch (...)
        {
            // If check fails, fall through to auto-detection
        }
    }

    // If no font specified in config or file doesn't exist, search for any .ttf file in speedfonts folder
    try
    {
        // Check if speedfonts directory exists
        if (!fs::exists(fontDir) || !fs::is_directory(fontDir))
        {
            return "";  // No speedfonts folder, return empty
        }

        for (const auto& entry : fs::directory_iterator(fontDir))
        {
            if (entry.is_regular_file())
            {
                std::string extension = entry.path().extension().string();
                // Convert extension to lowercase for case-insensitive comparison
                std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

                if (extension == ".ttf")
                {
                    return entry.path().string();
                }
            }
        }
    }
    catch (...)
    {
        // If directory iteration fails, return empty string
    }

    return "";
}

// Load custom font or default with anti-aliasing
void LoadImGuiFont()
{
    ImGuiIO& io = ImGui::GetIO();

    // Try to find a .ttf file in the DLL directory
    std::string ttfPath = FindTTFFile(g_DllDirectory);

    if (!ttfPath.empty())
    {
        // Load custom TTF font
        // Using a reasonable default size, you can adjust this
        ImFont* font = io.Fonts->AddFontFromFileTTF(ttfPath.c_str(), 16.0f);

        if (font != nullptr)
        {
            // Font loaded successfully
            io.Fonts->Build();
            return;
        }
        // If loading failed, fall through to default font
    }

    // Use default font with anti-aliasing (smoothened)
    ImFontConfig fontConfig;
    fontConfig.OversampleH = 3;  // Horizontal oversampling for smoother fonts
    fontConfig.OversampleV = 2;  // Vertical oversampling
    fontConfig.PixelSnapH = false;  // Don't snap to pixel grid for smoother appearance

    io.Fonts->AddFontDefault(&fontConfig);
    io.Fonts->Build();
}

// ----------------------------
// TIME CONTROL FUNCTIONS
// ----------------------------

// Simple pattern scan implementation
uintptr_t PatternScan(const char* pattern)
{
    // Get module info
    MODULEINFO modInfo;
    GetModuleInformation(GetCurrentProcess(), g_GameModule, &modInfo, sizeof(MODULEINFO));

    uintptr_t base = (uintptr_t)modInfo.lpBaseOfDll;
    uintptr_t size = modInfo.SizeOfImage;

    // Parse pattern
    std::vector<int> patternBytes;
    std::istringstream iss(pattern);
    std::string byteStr;

    while (iss >> byteStr)
    {
        if (byteStr == "??" || byteStr == "?")
            patternBytes.push_back(-1);
        else
            patternBytes.push_back(std::stoi(byteStr, nullptr, 16));
    }

    // Scan
    for (uintptr_t i = 0; i < size - patternBytes.size(); i++)
    {
        bool found = true;
        for (size_t j = 0; j < patternBytes.size(); j++)
        {
            if (patternBytes[j] != -1 && *(BYTE*)(base + i + j) != patternBytes[j])
            {
                found = false;
                break;
            }
        }
        if (found)
            return base + i;
    }

    return 0;
}

bool ApplyPatch(uintptr_t address, const BYTE* patch, size_t size)
{
    DWORD oldProtect;
    if (!VirtualProtect((LPVOID)address, size, PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;

    memcpy((void*)address, patch, size);

    VirtualProtect((LPVOID)address, size, oldProtect, &oldProtect);
    return true;
}

void StopTODTicker()
{
    uintptr_t address = PatternScan(TODTickDisablePattern);
    if (address)
    {
        ApplyPatch(address, TODTickDisablePatch, sizeof(TODTickDisablePatch));
        g_IsTimePassing = false;
    }
}

void ResumeTODTicker()
{
    uintptr_t address = PatternScan(TODTickRevertPattern);
    if (address)
    {
        ApplyPatch(address, TODTickRevertPatch, sizeof(TODTickRevertPatch));
        g_IsTimePassing = true;
    }
}

float GetCurrentTOD()
{
    uintptr_t baseAddress = (uintptr_t)g_GameModule + TODPointer;
    uintptr_t pointer = *(uintptr_t*)baseAddress;
    if (pointer == 0)
        return 0.0f;

    uintptr_t valueAddress = pointer + TODOffset;
    return *(float*)valueAddress;
}

void SetTOD(float time)
{
    uintptr_t baseAddress = (uintptr_t)g_GameModule + TODPointer;
    uintptr_t pointer = *(uintptr_t*)baseAddress;
    if (pointer == 0)
        return;

    uintptr_t valueAddress = pointer + TODOffset;
    *(float*)valueAddress = time;
}

void TimelapseThreadFunction()
{
    g_TimelapseThreadRunning.store(true);

    while (g_IsTimelapsing)
    {
        float currentTime = GetCurrentTOD();
        float newTime = currentTime + 0.05f;  // Speed of timelapse
        if (newTime >= 24.0f)
            newTime -= 24.0f;

        SetTOD(newTime);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));  // ~60 FPS
    }

    g_TimelapseThreadRunning.store(false);
}

void ToggleFreezeTime()
{
    if (g_IsTimelapsing)
    {
        // Stop timelapse first
        g_IsTimelapsing = false;
        if (g_TimelapseThread.joinable())
            g_TimelapseThread.join();
    }

    if (g_IsTimePassing)
        StopTODTicker();
    else
        ResumeTODTicker();
}

void ToggleTimelapse()
{
    if (!g_IsTimelapsing)
    {
        // Start timelapse
        if (g_IsTimePassing)
            StopTODTicker();

        g_IsTimelapsing = true;
        if (g_TimelapseThread.joinable())
            g_TimelapseThread.join();

        g_TimelapseThread = std::thread(TimelapseThreadFunction);
    }
    else
    {
        // Stop timelapse
        StopTimelapse();
    }
}

void StopTimelapse()
{
    if (g_IsTimelapsing)
    {
        g_IsTimelapsing = false;
        if (g_TimelapseThread.joinable())
            g_TimelapseThread.join();
    }
}

// ----------------------------
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

HRESULT __stdcall hkPresent(IDXGISwapChain* swapChain, UINT sync, UINT flags)
{
    if (!g_ImGuiInitialized.load(std::memory_order_acquire))
    {
        if (SUCCEEDED(swapChain->GetDevice(IID_PPV_ARGS(&g_Device))))
        {
            g_Device->GetImmediateContext(&g_Context);

            DXGI_SWAP_CHAIN_DESC sd;
            swapChain->GetDesc(&sd);

            CreateRenderTarget(swapChain);

            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

            ImGui_ImplWin32_Init(sd.OutputWindow);
            ImGui_ImplDX11_Init(g_Device, g_Context);

            g_GameModule = GetModuleHandleA(nullptr);

            // Get DLL directory and INI path
            char dllPath[MAX_PATH];
            HMODULE hModule = nullptr;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCSTR)&hkPresent, &hModule);
            GetModuleFileNameA(hModule, dllPath, MAX_PATH);

            std::string path(dllPath);
            size_t pos = path.find_last_of("\\/");
            if (pos != std::string::npos)
            {
                g_DllDirectory = path.substr(0, pos);
                g_IniPath = g_DllDirectory + "\\speedhud.ini";
            }

            // Load config first to get font file setting
            LoadConfig();

            // Load custom font or default smoothened font
            LoadImGuiFont();

            g_ImGuiInitialized.store(true, std::memory_order_release);
        }
    }

    if (g_ImGuiInitialized.load(std::memory_order_acquire))
    {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGuiIO& io = ImGui::GetIO();

        // Cached pointer calculation
        static float* speedPtr = reinterpret_cast<float*>((uintptr_t)g_GameModule + g_SpeedAddress);
        float speedKmh = *speedPtr;

        // Distance update
        float speedMps = speedKmh * 0.277778f;
        g_DistanceMeters += speedMps * io.DeltaTime;

        // HUD rendering
        if (!g_HudPosInitialized)
        {
            ImGui::SetNextWindowPos(g_HudPos, ImGuiCond_Once);
            g_HudPosInitialized = true;
        }
        else
        {
            ImGui::SetNextWindowPos(g_HudPos, ImGuiCond_Always);
        }

        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("##HUD", nullptr,
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoBackground);

        ImGui::SetWindowFontScale(g_HudFontScale);

        // Build HUD text
        std::string hudText;
        if (g_ShowSpeed && g_ShowDistance)
        {
            char buffer[64];
            snprintf(buffer, sizeof(buffer), "%.1f km/h\n%.0f m", speedKmh, g_DistanceMeters);
            hudText = buffer;
        }
        else if (g_ShowSpeed)
        {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%.1f km/h", speedKmh);
            hudText = buffer;
        }
        else if (g_ShowDistance)
        {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%.0f m", g_DistanceMeters);
            hudText = buffer;
        }



        if (!hudText.empty())
            ImGui::TextUnformatted(hudText.c_str());

        ImVec2 textSize = ImGui::GetWindowSize();
        ImGui::SetWindowFontScale(1.0f);
        ImGui::End();
        ImGui::PopStyleVar(2); // Pop WindowPadding and WindowBorderSize
        ImGui::PopStyleColor();

        // Menu toggle
        static bool wasMenuOpen = false;
        if (GetAsyncKeyState(VK_HOME) & 1)
            g_ShowMenu = !g_ShowMenu;

        if (wasMenuOpen && !g_ShowMenu)
            SaveConfig();
        wasMenuOpen = g_ShowMenu;

        // HUD Settings Menu
        if (g_ShowMenu)
        {
            float oldScale = io.FontGlobalScale;
            io.FontGlobalScale = g_MenuFontScale;

            ImGui::SetNextWindowPos(
                ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                ImGuiCond_Once, ImVec2(0.5f, 0.5f));

            ImGui::Begin("Settings", &g_ShowMenu,
                ImGuiWindowFlags_AlwaysAutoResize |
                ImGuiWindowFlags_NoCollapse);

            ImGui::SeparatorText("HUD Controls");
            ImGui::TextUnformatted("Arrow keys: move HUD");
            ImGui::TextUnformatted("Ctrl +/-: scale HUD");
            ImGui::TextUnformatted("Ctrl + 0 : Toggle speed");
            ImGui::TextUnformatted("Ctrl + 9 : Toggle distance");
            ImGui::TextUnformatted("Ctrl + 8 : Reset distance");
            ImGui::TextUnformatted("Ctrl + S : Save HUD");

            ImGui::Spacing();
            ImGui::SeparatorText("Time Controls");
            ImGui::TextUnformatted("F1 : Toggle freeze time");
            ImGui::TextUnformatted("F2 : Toggle timelapse");

            // Status display
            ImGui::Spacing();
            ImGui::SeparatorText("Status");
            ImGui::Text("Time: %s", g_IsTimePassing ? "Running" : "Frozen");
            if (!g_IsTimePassing)
            {
                float currentTime = GetCurrentTOD();
                ImGui::Text("Current Time: %.2fh", currentTime);
            }
            if (g_IsTimelapsing)
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Timelapse: Active");

            ImGui::End();
            io.FontGlobalScale = oldScale;

            // Input handling
            bool ctrlPressed = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            static float currentTime = 0.0f;
            currentTime += io.DeltaTime;

            if (!ctrlPressed)
            {
                // Movement controls
                if (GetAsyncKeyState(VK_LEFT) & 0x8000)  g_HudPos.x -= 200.0f * io.DeltaTime;
                if (GetAsyncKeyState(VK_RIGHT) & 0x8000) g_HudPos.x += 200.0f * io.DeltaTime;
                if (GetAsyncKeyState(VK_UP) & 0x8000)    g_HudPos.y -= 200.0f * io.DeltaTime;
                if (GetAsyncKeyState(VK_DOWN) & 0x8000)  g_HudPos.y += 200.0f * io.DeltaTime;

                ClampHudPosition(g_HudPos, textSize, io.DisplaySize);
            }

            if (ctrlPressed)
            {
                if (HandleDebouncedInput('0', currentTime)) g_ShowSpeed = !g_ShowSpeed;
                if (HandleDebouncedInput('9', currentTime)) g_ShowDistance = !g_ShowDistance;
                if (HandleDebouncedInput('8', currentTime)) g_DistanceMeters = 0.0f;
                if (HandleDebouncedInput('S', currentTime)) SaveConfig();

                if (HandleDebouncedInput(VK_OEM_PLUS, currentTime) || HandleDebouncedInput(VK_ADD, currentTime))
                    g_HudFontScale = min(g_HudFontScale + 0.1f, 10.0f);

                if (HandleDebouncedInput(VK_OEM_MINUS, currentTime) || HandleDebouncedInput(VK_SUBTRACT, currentTime))
                    g_HudFontScale = max(g_HudFontScale - 0.1f, 0.1f);
            }
        }

        // Time control hotkeys (work even when menu is closed)
        static float hotkeyTime = 0.0f;
        hotkeyTime += io.DeltaTime;

        if (HandleDebouncedInput(HOTKEY_FREEZE_TIME, hotkeyTime))
        {
            ToggleFreezeTime();
        }

        if (HandleDebouncedInput(HOTKEY_TIMELAPSE, hotkeyTime))
        {
            ToggleTimelapse();
        }

        ImGui::Render();
        g_Context->OMSetRenderTargets(1, &g_RTV, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    return oPresent(swapChain, sync, flags);
}

// ----------------------------
DWORD WINAPI InitHook(LPVOID)
{
    constexpr DXGI_SWAP_CHAIN_DESC sd = {
        .BufferDesc = {.Format = DXGI_FORMAT_R8G8B8A8_UNORM },
        .SampleDesc = {.Count = 1 },
        .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
        .BufferCount = 1,
        .OutputWindow = nullptr,
        .Windowed = TRUE
    };

    DXGI_SWAP_CHAIN_DESC sdCopy = sd;
    sdCopy.OutputWindow = GetForegroundWindow();

    IDXGISwapChain* swapChain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &sdCopy, &swapChain, &device, nullptr, &context);

    if (SUCCEEDED(hr))
    {
        void** vtable = *reinterpret_cast<void***>(swapChain);

        if (MH_Initialize() == MH_OK)
        {
            MH_CreateHook(vtable[8], &hkPresent, reinterpret_cast<void**>(&oPresent));
            MH_EnableHook(vtable[8]);
        }

        context->Release();
        device->Release();
        swapChain->Release();
    }
    return 0;
}

// ----------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, InitHook, nullptr, 0, nullptr);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        // Stop timelapse thread if running
        StopTimelapse();

        // Cleanup
        if (g_ImGuiInitialized.load())
        {
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
        }

        if (g_RTV) g_RTV->Release();
        if (g_Context) g_Context->Release();
        if (g_Device) g_Device->Release();

        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
    }
    return TRUE;
}
