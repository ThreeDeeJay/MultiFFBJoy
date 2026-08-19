#include "common.h"
#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "ws2_32.lib")
namespace MultiFFBJoy
{
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
    DeviceState g_state;
    std::vector<DeviceCandidate> g_candidates;
    std::vector<PresetInfo> g_availablePresets;
    PresetTestState g_presetTestState;
}
using namespace MultiFFBJoy;
int APIENTRY wWinMain(
    HINSTANCE instance,
    HINSTANCE,
    LPWSTR,
    int showCommand)
{
    Log("MultiFFBJoy starting.");
    if (!CreateMainWindow(
        instance,
        showCommand))
    {
        return 1;
    }
    g_running = true;
    if (!InitializeDirectInput(instance))
    {
        Log(
            "DirectInput initialization failed.");
    }
    else
    {
        if (!SelectFirstSuitableDevice())
        {
            Log(
                "No suitable FFB joystick available at startup.");
        }
    }
    StartFFBWatchdog();
    if (!StartUdpServer())
    {
        Log(
            "UDP server could not be started.");
    }
    PopulatePresetList();
    RunMessageLoop();
    StopUdpServer();
    g_running = false;
    StopFFBWatchdog();
    ClearForceFieldPreset();
    ReleaseFFBDevice();
    ShutdownDirectInput();
    DestroyMainWindow();
    return 0;
}