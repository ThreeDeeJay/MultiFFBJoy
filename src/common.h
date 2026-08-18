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
#include <mutex>
#include <string>
#include <thread>
#include <vector>
namespace MultiFFBJoy
{
    constexpr UINT WM_APP_LOG = WM_APP + 1;
    constexpr int UDP_PORT = 65458;
    constexpr DWORD COMMAND_TIMEOUT_MS = 250;
    constexpr DWORD SOCKET_TIMEOUT_MS = 25;
    constexpr int IDC_FFB_UP = 2001;
    constexpr int IDC_FFB_DOWN = 2002;
    constexpr int IDC_FFB_LEFT = 2003;
    constexpr int IDC_FFB_RIGHT = 2004;
    constexpr int IDC_FFB_STOP = 2005;
    constexpr int IDC_FFB_CENTER = 2006;
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
// Logging is implemented by gui.cpp.
    void Log(const std::string& text);
template <typename... Args>
    void Logf(const char* format, Args... args)
    {
        char buffer[2048]{};
        sprintf_s(buffer, sizeof(buffer), format, args...);
        Log(buffer);
    }
    std::wstring Utf8ToWide(const char* text);
// GUI
    bool CreateMainWindow(HINSTANCE instance, int showCommand);
    int RunMessageLoop();
    void DestroyMainWindow();
    void UpdateStatus();
// UDP
    bool StartUdpServer();
    void StopUdpServer();
    void SendUdpCommand(const std::string& command);
// FFB
    bool CreateSpringEffect();
    bool CreateTestConstantForceEffect();
    bool IsFFBDeviceUsable();
    void StopSpring();
    void StopSpringForRelease();
    void StopTestConstantForce();
    bool SetSpringStrength(float strength);
    bool SetTestConstantForce(LONG x, LONG y);
    bool EnsureFFBDeviceReady();
    bool ReacquireFFBDevice();
    void StartFFBWatchdog();
    void StopFFBWatchdog();
// Device
    bool InitializeDirectInput(HINSTANCE instance);
    void ShutdownDirectInput();
    bool SelectFirstSuitableDevice();
    void ReleaseFFBDevice();
} // namespace MultiFFBJoy
