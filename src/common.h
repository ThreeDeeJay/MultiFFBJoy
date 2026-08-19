#pragma once
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
namespace MultiFFBJoy
{
    inline constexpr UINT WM_APP_LOG = WM_APP + 1;
    inline constexpr int UDP_PORT = 65458;
    inline constexpr DWORD COMMAND_TIMEOUT_MS = 250;
    inline constexpr DWORD SOCKET_TIMEOUT_MS = 25;
    inline constexpr int IDC_FFB_UP = 2001;
    inline constexpr int IDC_FFB_DOWN = 2002;
    inline constexpr int IDC_FFB_LEFT = 2003;
    inline constexpr int IDC_FFB_RIGHT = 2004;
    inline constexpr int IDC_FFB_STOP = 2005;
    inline constexpr int IDC_FFB_CENTER = 2006;
    // Preset GUI controls.
    inline constexpr int IDC_PRESET_LIST = 2100;
    inline constexpr int IDC_PRESET_LOAD = 2101;
    extern HWND g_mainWindow;
    extern HWND g_statusWindow;
    extern HWND g_logWindow;
    extern IDirectInput8W* g_directInput;
    extern IDirectInputDevice8W* g_ffbDevice;
    extern IDirectInputEffect* g_springEffect;
    extern IDirectInputEffect* g_testConstantEffect;
    extern SOCKET g_socket;
    extern std::thread g_networkThread;
    extern std::thread g_ffbWatchdogThread;
    extern std::atomic<bool> g_running;
    extern std::atomic<bool> g_networkRunning;
    extern std::atomic<bool> g_reacquiring;
    extern std::mutex g_stateMutex;
    struct DeviceState
    {
        std::wstring name = L"(none)";
        DWORD axisCount = 0;
        bool forceFeedback = false;
        bool acquired = false;
        bool springSupported = false;
        DWORD xAxisOffset = DIJOFS_X;
        DWORD yAxisOffset = DIJOFS_Y;
        float springStrength = 0.0f;
        bool springPersistent = false;
        std::chrono::steady_clock::time_point lastCommand =
        std::chrono::steady_clock::now();
    };
    extern DeviceState g_state;
    struct DeviceCandidate
    {
        GUID guid{};
        std::wstring name;
        DWORD axisCount = 0;
        bool forceFeedback = false;
        bool hasXAxis = false;
        bool hasYAxis = false;
        bool springSupported = false;
        DWORD springEffType = 0;
        DWORD springStaticParams = 0;
        DWORD springDynamicParams = 0;
        std::vector<DWORD> ffbActuatorOffsets;
    };
    extern std::vector<DeviceCandidate> g_candidates;
    // ---------------------------------------------------------------------
    // FFShifter-compatible forcefield representation.
    // ---------------------------------------------------------------------
    struct ForceFieldVertex
    {
        LONG x = 0;
        LONG y = 0;
        LONG z = 0;
    };
    struct ForceField
    {
        std::string name;
        // FORCEFIELD TYPE.
        // 0 = constant force
        // 1 = spring force
        int forceFieldType = 0;
        // FORCEFIELD SHAPE TYPE.
        // Currently retained for compatibility but not interpreted.
        int shapeType = 0;
        LONG centerX = 0;
        LONG centerY = 0;
        LONG centerZ = 0;
        std::vector<ForceFieldVertex> vertices;
        // FORCE TYPE.
        // 0 = constant force
        // 1 = spring force
        int forceType = 0;
        int primaryKeyIndex = -1;
        int secondaryKeyIndex = -1;
        int primarySequentialGearValue = -1;
        int secondarySequentialGearValue = -1;
        LONG powerX = 0;
        LONG powerY = 0;
        LONG offsetX = 0;
        LONG offsetY = 0;
    };
    struct ForceFieldPreset
    {
        std::filesystem::path path;
        std::string version;
        std::vector<ForceField> forceFields;
        bool loaded = false;
    };
    extern std::mutex g_presetMutex;
    extern ForceFieldPreset g_loadedPreset;
    // ---------------------------------------------------------------------
    // Logging.
    // ---------------------------------------------------------------------
    void Log(const std::string& text);
    template <typename... Args>
    void Logf(const char* format, Args... args)
    {
        char buffer[2048]{};
        sprintf_s(buffer, sizeof(buffer), format, args...);
        Log(buffer);
    }
    std::wstring Utf8ToWide(const char* text);
    // ---------------------------------------------------------------------
    // GUI.
    // ---------------------------------------------------------------------
    bool CreateMainWindow(
        HINSTANCE instance,
        int showCommand);
    int RunMessageLoop();
    void DestroyMainWindow();
    void UpdateStatus();
    // ---------------------------------------------------------------------
    // UDP.
    // ---------------------------------------------------------------------
    bool StartUdpServer();
    void StopUdpServer();
    void SendUdpCommand(
        const std::string& command);
    // ---------------------------------------------------------------------
    // FFB.
    // ---------------------------------------------------------------------
    bool CreateSpringEffect();
    bool CreateTestConstantForceEffect();
    bool IsFFBDeviceUsable();
    void StopSpring();
    void StopSpringForRelease();
    void StopTestConstantForce();
    bool SetSpringStrength(
        float strength);
    bool SetTestConstantForce(
        LONG x,
        LONG y);
    // Apply one FFShifter spring forcefield.
    bool SetSpringForceField(
        const ForceField& forceField);
    bool EnsureFFBDeviceReady();
    bool ReacquireFFBDevice();
    void StartFFBWatchdog();
    void StopFFBWatchdog();
    // ---------------------------------------------------------------------
    // Device.
    // ---------------------------------------------------------------------
    bool InitializeDirectInput(
        HINSTANCE instance);
    void ShutdownDirectInput();
    bool SelectFirstSuitableDevice();
    void ReleaseFFBDevice();
    // ---------------------------------------------------------------------
    // Presets.
    // ---------------------------------------------------------------------
    bool LoadForceFieldPreset(
        const std::filesystem::path& path);
    void ClearForceFieldPreset();
    bool IsForceFieldPresetLoaded();
    std::filesystem::path GetLoadedForceFieldPresetPath();
    std::vector<std::filesystem::path>
    EnumerateForceFieldPresets();
    // Applies the currently loaded preset as a test.
    void UpdatePresetTest();
    // Stop the current preset test and clear its active force.
    void StopPresetTest();
    // ---------------------------------------------------------------------
    // Preset GUI.
    // ---------------------------------------------------------------------
    void RefreshPresetList();
    bool LoadSelectedPresetFromGui();
}