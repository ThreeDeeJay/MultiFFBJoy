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

} // namespace MultiFFBJoy

using namespace MultiFFBJoy;

int APIENTRY wWinMain(
    HINSTANCE instance,
    HINSTANCE,
    LPWSTR,
    int showCommand)
{
    Log("MultiFFBJoy starting.");

    if (!CreateMainWindow(instance, showCommand))
    {
        return 1;
    }

    if (!InitializeDirectInput(instance))
    {
        Log("DirectInput initialization failed.");
    }
    else
    {
        if (!SelectFirstSuitableDevice())
        {
            Log(
                "No suitable FFB joystick available at startup.");
        }
        else
        {
            Log(
                "FFB joystick initialized successfully.");
        }
    }

    // Start the watchdog exactly once, outside device selection.
    // The previous implementation started it from inside
    // SelectFirstSuitableDevice(), which could create multiple
    // watchdog threads during re-acquisition.
    g_running = true;
    StartFFBWatchdog();

    if (!StartUdpServer())
    {
        Log(
            "UDP server could not be started.");
    }

    RunMessageLoop();

    // Shutdown order is deliberate:
    //
    // 1. Stop accepting network work.
    // 2. Stop watchdog.
    // 3. Stop/release FFB.
    // 4. Release DirectInput.
    //
    // This prevents worker threads from touching COM/DirectInput
    // objects while main is destroying them.
    StopUdpServer();

    g_running = false;
    StopFFBWatchdog();

    ReleaseFFBDevice();
    ShutdownDirectInput();

    DestroyMainWindow();

    return 0;
}
