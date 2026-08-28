#include "common.h"

#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "ws2_32.lib")

namespace MultiFFBJoy
{
HANDLE g_singleInstanceMutex = nullptr;
HWND g_mainWindow = nullptr;
HWND g_statusWindow = nullptr;
HWND g_logWindow = nullptr;
IDirectInput8W* g_directInput = nullptr;
IDirectInputDevice8W* g_ffbDevice = nullptr;
IDirectInputEffect* g_springEffect = nullptr;
IDirectInputEffect* g_testConstantEffect = nullptr;
SOCKET g_socket = INVALID_SOCKET;
std::thread g_networkThread;
std::thread g_ffbWatchdogThread;
std::atomic<bool> g_running{false};
std::atomic<bool> g_networkRunning{false};
std::atomic<bool> g_reacquiring{false};
std::mutex g_stateMutex;
std::recursive_mutex g_ffbMutex;
DeviceState g_state;
ActiveSpringState g_activeSpring;
std::vector<DeviceCandidate> g_candidates;
}

using namespace MultiFFBJoy;

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int showCommand)
{
    Log("MultiFFBJoy starting.");

    g_singleInstanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\MultiFFBJoy.Singleton");
    if (!g_singleInstanceMutex)
    {
        Logf("CreateMutexW failed: %lu", GetLastError());
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        Log("Another MultiFFBJoy instance is already running. Exiting duplicate instance.");
        CloseHandle(g_singleInstanceMutex);
        g_singleInstanceMutex = nullptr;
        return 2;
    }

    if (!CreateMainWindow(instance, showCommand))
    {
        CloseHandle(g_singleInstanceMutex);
        g_singleInstanceMutex = nullptr;
        return 1;
    }

    g_running = true;

    if (!InitializeDirectInput(instance))
        Log("DirectInput initialization failed.");
    else
        Log("DirectInput initialized.");

    if (LoadConfigurationFile())
        Logf("Configuration ready: %s", GetConfigurationFilePath().string().c_str());
    else
        Logf("Configuration unavailable: %s", GetConfigurationFilePath().string().c_str());

    if (!ReacquireFFBDevice())
        Log("Initial FFB acquisition failed; watchdog will retry later.");

    StartFFBWatchdog();

    if (!StartUdpServer())
        Log("UDP server could not be started.");

    PopulatePresetList();
    UpdateStatus();
    const int exitCode = RunMessageLoop();

    // Stop producers before tearing down the objects they use.
    g_running = false;
    StopUdpServer();
    StopFFBWatchdog();
    ClearForceFieldPreset();
    ReleaseFFBDevice();
    ShutdownDirectInput();
    DestroyMainWindow();
    if (g_singleInstanceMutex)
    {
        CloseHandle(g_singleInstanceMutex);
        g_singleInstanceMutex = nullptr;
    }
    return exitCode;
}
