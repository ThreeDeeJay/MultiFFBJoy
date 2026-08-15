#define DIRECTINPUT_VERSION 0x0800

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <dinput.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "ws2_32.lib")

namespace
{
    // -------------------------------------------------------------------------
    // Configuration
    // -------------------------------------------------------------------------

    constexpr UINT WM_APP_LOG = WM_APP + 1;

    constexpr int UDP_PORT = 47777;

    // If the client stops sending commands for this long, all FFB is stopped.
    constexpr DWORD COMMAND_TIMEOUT_MS = 250;

    // Network receive timeout. This allows the worker thread to periodically
    // check the safety timeout without requiring another packet.
    constexpr DWORD SOCKET_TIMEOUT_MS = 25;


    // -------------------------------------------------------------------------
    // GUI
    // -------------------------------------------------------------------------

    HWND g_mainWindow = nullptr;
    HWND g_statusWindow = nullptr;
    HWND g_logWindow = nullptr;


    // -------------------------------------------------------------------------
    // DirectInput
    // -------------------------------------------------------------------------

    IDirectInput8W* g_directInput = nullptr;
    IDirectInputDevice8W* g_ffbDevice = nullptr;
    IDirectInputEffect* g_springEffect = nullptr;


    // -------------------------------------------------------------------------
    // Networking
    // -------------------------------------------------------------------------

    SOCKET g_socket = INVALID_SOCKET;

    std::thread g_networkThread;
    std::atomic<bool> g_running{ false };


    // -------------------------------------------------------------------------
    // Shared state
    // -------------------------------------------------------------------------

    std::mutex g_stateMutex;

    struct DeviceState
    {
        std::wstring name = L"(none)";

        DWORD axisCount = 0;

        bool forceFeedback = false;
        bool acquired = false;

        // Physical DirectInput offsets selected for our logical X/Y axes.
        DWORD xAxisOffset = DIJOFS_X;
        DWORD yAxisOffset = DIJOFS_Y;

        float springX = 0.0f;
        float springY = 0.0f;
        float springStrength = 0.0f;

        std::chrono::steady_clock::time_point lastCommand =
            std::chrono::steady_clock::now();
    };

    DeviceState g_state;


    // -------------------------------------------------------------------------
    // Device enumeration
    // -------------------------------------------------------------------------

    struct DeviceCandidate
    {
        GUID guid{};

        std::wstring name;

        DWORD axisCount = 0;

        bool forceFeedback = false;

        bool hasXAxis = false;
        bool hasYAxis = false;
    };

    std::vector<DeviceCandidate> g_candidates;


    // -------------------------------------------------------------------------
    // Utility
    // -------------------------------------------------------------------------

    std::wstring Utf8ToWide(const char* text)
    {
        if (text == nullptr)
            return {};

        const int required =
            MultiByteToWideChar(
                CP_UTF8,
                0,
                text,
                -1,
                nullptr,
                0);

        if (required <= 0)
            return {};

        std::wstring result(
            static_cast<size_t>(required - 1),
            L'\0');

        MultiByteToWideChar(
            CP_UTF8,
            0,
            text,
            -1,
            result.data(),
            required);

        return result;
    }


    void Log(const char* format, ...)
    {
        char buffer[2048]{};

        va_list arguments;

        va_start(arguments, format);

        vsnprintf_s(
            buffer,
            sizeof(buffer),
            _TRUNCATE,
            format,
            arguments);

        va_end(arguments);

        auto* message =
            new std::wstring(
                Utf8ToWide(buffer));

        if (g_mainWindow != nullptr)
        {
            PostMessageW(
                g_mainWindow,
                WM_APP_LOG,
                0,
                reinterpret_cast<LPARAM>(message));
        }
        else
        {
            delete message;
        }
    }


    void UpdateStatus()
    {
        if (g_statusWindow == nullptr)
            return;

        DeviceState state;

        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            state = g_state;
        }

        wchar_t text[4096]{};

        swprintf_s(
            text,
            L"Selected device: %ls\r\n"
            L"Axes: %lu\r\n"
            L"Force Feedback: %s\r\n"
            L"Acquired: %s\r\n"
            L"Logical X axis: 0x%lX\r\n"
            L"Logical Y axis: 0x%lX\r\n"
            L"Spring X: %.3f\r\n"
            L"Spring Y: %.3f\r\n"
            L"Spring strength: %.3f\r\n"
            L"UDP: 127.0.0.1:%d\r\n"
            L"FFB safety timeout: %lu ms",
            state.name.c_str(),
            state.axisCount,
            state.forceFeedback ? L"Yes" : L"No",
            state.acquired ? L"Yes" : L"No",
            static_cast<unsigned long>(state.xAxisOffset),
            static_cast<unsigned long>(state.yAxisOffset),
            state.springX,
            state.springY,
            state.springStrength,
            UDP_PORT,
            COMMAND_TIMEOUT_MS);

        SetWindowTextW(
            g_statusWindow,
            text);
    }


    // -------------------------------------------------------------------------
    // Axis enumeration
    // -------------------------------------------------------------------------

    struct AxisEnumerationContext
    {
        DWORD axisCount = 0;

        bool hasXAxis = false;
        bool hasYAxis = false;
    };


    BOOL CALLBACK EnumerateAxesCallback(
        const DIDEVICEOBJECTINSTANCEW* object,
        VOID* contextPointer)
    {
        if (object == nullptr ||
            contextPointer == nullptr)
        {
            return DIENUM_CONTINUE;
        }

        auto* context =
            static_cast<AxisEnumerationContext*>(
                contextPointer);

        if ((object->dwType & DIDFT_AXIS) == 0)
            return DIENUM_CONTINUE;

        ++context->axisCount;

        if (object->dwOfs == DIJOFS_X)
            context->hasXAxis = true;

        if (object->dwOfs == DIJOFS_Y)
            context->hasYAxis = true;

        return DIENUM_CONTINUE;
    }


    // -------------------------------------------------------------------------
    // DirectInput device enumeration
    // -------------------------------------------------------------------------

    BOOL CALLBACK EnumerateDevicesCallback(
        const DIDEVICEINSTANCEW* instance,
        VOID*)
    {
        if (g_directInput == nullptr ||
            instance == nullptr)
        {
            return DIENUM_STOP;
        }

        IDirectInputDevice8W* device = nullptr;

        HRESULT hr =
            g_directInput->CreateDevice(
                instance->guidInstance,
                &device,
                nullptr);

        if (FAILED(hr) ||
            device == nullptr)
        {
            return DIENUM_CONTINUE;
        }

        DIDEVCAPS capabilities{};

        capabilities.dwSize =
            sizeof(capabilities);

        hr =
            device->GetCapabilities(
                &capabilities);

        if (SUCCEEDED(hr))
        {
            AxisEnumerationContext axes;

            device->EnumObjects(
                EnumerateAxesCallback,
                &axes,
                DIDFT_AXIS);

            DeviceCandidate candidate;

            candidate.guid =
                instance->guidInstance;

            candidate.name =
                instance->tszProductName;

            candidate.axisCount =
                axes.axisCount;

            candidate.forceFeedback =
                (capabilities.dwFlags &
                 DIDC_FORCEFEEDBACK) != 0;

            candidate.hasXAxis =
                axes.hasXAxis;

            candidate.hasYAxis =
                axes.hasYAxis;

            g_candidates.push_back(
                candidate);
        }

        device->Release();

        return DIENUM_CONTINUE;
    }


    // -------------------------------------------------------------------------
    // FFB cleanup
    // -------------------------------------------------------------------------

    void StopSpring()
    {
        if (g_springEffect != nullptr)
        {
            g_springEffect->Stop();
        }

        {
            std::lock_guard<std::mutex> lock(
                g_stateMutex);

            g_state.springX = 0.0f;
            g_state.springY = 0.0f;
            g_state.springStrength = 0.0f;
        }

        UpdateStatus();
    }


    void ReleaseFFBDevice()
    {
        StopSpring();

        if (g_springEffect != nullptr)
        {
            g_springEffect->Release();
            g_springEffect = nullptr;
        }

        if (g_ffbDevice != nullptr)
        {
            g_ffbDevice->Unacquire();
            g_ffbDevice->Release();
            g_ffbDevice = nullptr;
        }

        {
            std::lock_guard<std::mutex> lock(
                g_stateMutex);

            g_state =
                DeviceState{};
        }

        UpdateStatus();
    }


    // -------------------------------------------------------------------------
    // Disable DirectInput's own automatic centering.
    //
    // We want MultiFFBJoy to control the spring ourselves.
    // -------------------------------------------------------------------------

    void DisableHardwareAutoCenter()
    {
        if (g_ffbDevice == nullptr)
            return;

        DIPROPDWORD property{};

        property.diph.dwSize =
            sizeof(property);

        property.diph.dwHeaderSize =
            sizeof(property.diph);

        property.diph.dwObj = 0;

        property.diph.dwHow =
            DIPH_DEVICE;

        property.dwData = DIPROPAUTOCENTER_OFF;

        HRESULT hr =
            g_ffbDevice->SetProperty(
                DIPROP_AUTOCENTER,
                &property.diph);

        if (FAILED(hr))
        {
            Log(
                "Could not disable device auto-center: 0x%08lX",
                static_cast<unsigned long>(hr));
        }
        else
        {
            Log("Hardware auto-center disabled.");
        }
    }


    // -------------------------------------------------------------------------
    // Create the spring effect.
    // -------------------------------------------------------------------------

    bool CreateSpringEffect()
    {
        if (g_ffbDevice == nullptr)
            return false;

        DWORD axes[2] =
        {
            DIJOFS_X,
            DIJOFS_Y
        };

        LONG directions[2] =
        {
            0,
            0
        };

        DICONDITION conditions[2]{};

        for (int i = 0; i < 2; ++i)
        {
            conditions[i].lOffset = 0;

            conditions[i].lPositiveCoefficient =
                10000;

            conditions[i].lNegativeCoefficient =
                10000;

            conditions[i].dwPositiveSaturation =
                10000;

            conditions[i].dwNegativeSaturation =
                10000;

            conditions[i].lDeadBand = 0;
        }

        DIEFFECT effect{};

        effect.dwSize =
            sizeof(effect);

        effect.dwFlags =
            DIEFF_CARTESIAN |
            DIEFF_OBJECTOFFSETS;

        effect.dwDuration =
            INFINITE;

        effect.dwGain =
            DI_FFNOMINALMAX;

        effect.cAxes =
            2;

        effect.rgdwAxes =
            axes;

        effect.rglDirection =
            directions;

        effect.cbTypeSpecificParams =
            sizeof(conditions);

        effect.lpvTypeSpecificParams =
            conditions;

        HRESULT hr =
            g_ffbDevice->CreateEffect(
                GUID_Condition,
                &effect,
                &g_springEffect,
                nullptr);

        if (FAILED(hr))
        {
            Log(
                "CreateEffect(GUID_Condition) failed: 0x%08lX",
                static_cast<unsigned long>(hr));

            g_springEffect = nullptr;

            return false;
        }

        Log(
            "Created 2-axis condition/spring effect.");

        return true;
    }


    // -------------------------------------------------------------------------
    // Select first generic suitable FFB controller.
    // -------------------------------------------------------------------------

    bool SelectFirstSuitableDevice()
    {
        ReleaseFFBDevice();

        g_candidates.clear();

        HRESULT hr =
            g_directInput->EnumDevices(
                DI8DEVCLASS_GAMECTRL,
                EnumerateDevicesCallback,
                nullptr,
                DIEDFL_ATTACHEDONLY);

        if (FAILED(hr))
        {
            Log(
                "DirectInput enumeration failed: 0x%08lX",
                static_cast<unsigned long>(hr));

            return false;
        }

        Log(
            "Found %zu attached game-controller device(s).",
            g_candidates.size());

        for (size_t i = 0;
             i < g_candidates.size();
             ++i)
        {
            const auto& candidate =
                g_candidates[i];

            Log(
                "[%zu] %ls | axes=%lu | X=%s | Y=%s | FFB=%s",
                i,
                candidate.name.c_str(),
                candidate.axisCount,
                candidate.hasXAxis ? "yes" : "no",
                candidate.hasYAxis ? "yes" : "no",
                candidate.forceFeedback ? "yes" : "no");
        }

        // Generic selection:
        //
        //   - DirectInput game controller
        //   - FFB capable
        //   - at least two axes
        //   - explicit X and Y axes
        //
        // No VID/PID/HWID is used.

        for (const auto& candidate :
             g_candidates)
        {
            if (!candidate.forceFeedback)
                continue;

            if (candidate.axisCount < 2)
                continue;

            if (!candidate.hasXAxis ||
                !candidate.hasYAxis)
            {
                continue;
            }

            IDirectInputDevice8W* device =
                nullptr;

            hr =
                g_directInput->CreateDevice(
                    candidate.guid,
                    &device,
                    nullptr);

            if (FAILED(hr) ||
                device == nullptr)
            {
                Log(
                    "Could not open %ls: 0x%08lX",
                    candidate.name.c_str(),
                    static_cast<unsigned long>(hr));

                continue;
            }

            hr =
                device->SetDataFormat(
                    &c_dfDIJoystick2);

            if (FAILED(hr))
            {
                Log(
                    "SetDataFormat failed for %ls: 0x%08lX",
                    candidate.name.c_str(),
                    static_cast<unsigned long>(hr));

                device->Release();

                continue;
            }

            hr =
                device->SetCooperativeLevel(
                    g_mainWindow,
                    DISCL_BACKGROUND |
                    DISCL_EXCLUSIVE);

            if (FAILED(hr))
            {
                Log(
                    "SetCooperativeLevel failed for %ls: 0x%08lX",
                    candidate.name.c_str(),
                    static_cast<unsigned long>(hr));

                device->Release();

                continue;
            }

            hr =
                device->Acquire();

            if (FAILED(hr))
            {
                Log(
                    "Acquire failed for %ls: 0x%08lX",
                    candidate.name.c_str(),
                    static_cast<unsigned long>(hr));

                device->Release();

                continue;
            }

            g_ffbDevice =
                device;

            {
                std::lock_guard<std::mutex> lock(
                    g_stateMutex);

                g_state.name =
                    candidate.name;

                g_state.axisCount =
                    candidate.axisCount;

                g_state.forceFeedback =
                    candidate.forceFeedback;

                g_state.acquired =
                    true;

                g_state.xAxisOffset =
                    DIJOFS_X;

                g_state.yAxisOffset =
                    DIJOFS_Y;
            }

            Log(
                "Selected FFB device: %ls",
                candidate.name.c_str());

            DisableHardwareAutoCenter();

            if (!CreateSpringEffect())
            {
                ReleaseFFBDevice();

                continue;
            }

            UpdateStatus();

            return true;
        }

        Log(
            "No suitable 2-axis FFB joystick found.");

        return false;
    }


    // -------------------------------------------------------------------------
    // Update spring.
    //
    // x/y describe the current normalized stick position.
    //
    // The condition effect generates a restoring force toward the center.
    // -------------------------------------------------------------------------

    void SetSpring(
        float x,
        float y,
        float strength)
    {
        x =
            std::clamp(
                x,
                -1.0f,
                1.0f);

        y =
            std::clamp(
                y,
                -1.0f,
                1.0f);

        strength =
            std::clamp(
                strength,
                0.0f,
                1.0f);

        if (g_springEffect == nullptr)
        {
            Log(
                "SPRING ignored: no active FFB effect.");

            return;
        }

        DWORD axes[2] =
        {
            DIJOFS_X,
            DIJOFS_Y
        };

        //
        // For a condition effect, direction defines the positive axis
        // orientation. The coefficients then determine the restoring force.
        //
        // Keep the logical X/Y coordinate system consistent for clients.
        //
        LONG directions[2] =
        {
            0,
            0
        };

        const LONG coefficient =
            static_cast<LONG>(
                strength * 10000.0f);

        DICONDITION conditions[2]{};

        for (int i = 0; i < 2; ++i)
        {
            conditions[i].lOffset = 0;

            conditions[i].lPositiveCoefficient =
                coefficient;

            conditions[i].lNegativeCoefficient =
                coefficient;

            conditions[i].dwPositiveSaturation =
                static_cast<DWORD>(
                    coefficient);

            conditions[i].dwNegativeSaturation =
                static_cast<DWORD>(
                    coefficient);

            conditions[i].lDeadBand = 0;
        }

        DIEFFECT effect{};

        effect.dwSize =
            sizeof(effect);

        effect.dwFlags =
            DIEFF_CARTESIAN |
            DIEFF_OBJECTOFFSETS;

        effect.dwDuration =
            INFINITE;

        effect.dwGain =
            static_cast<DWORD>(
                strength * 10000.0f);

        effect.cAxes =
            2;

        effect.rgdwAxes =
            axes;

        effect.rglDirection =
            directions;

        effect.cbTypeSpecificParams =
            sizeof(conditions);

        effect.lpvTypeSpecificParams =
            conditions;

        //
        // The current prototype uses the stick position to determine the
        // direction of the restoring force.
        //
        // Convert the normalized position into DirectInput direction units.
        //
        LONG springDirection[2] =
        {
            static_cast<LONG>(
                -x * 10000.0f),

            static_cast<LONG>(
                -y * 10000.0f)
        };

        effect.rglDirection =
            springDirection;

        HRESULT hr =
            g_springEffect->SetParameters(
                &effect,
                DIEP_DIRECTION |
                DIEP_GAIN |
                DIEP_TYPESPECIFICPARAMS |
                DIEP_START);

        if (FAILED(hr))
        {
            Log(
                "SetParameters failed: 0x%08lX",
                static_cast<unsigned long>(hr));

            return;
        }

        {
            std::lock_guard<std::mutex> lock(
                g_stateMutex);

            g_state.springX =
                x;

            g_state.springY =
                y;

            g_state.springStrength =
                strength;
        }

        UpdateStatus();
    }


    // -------------------------------------------------------------------------
    // UDP command parser
    // -------------------------------------------------------------------------

    void ProcessCommand(
        const std::string& command)
    {
        std::istringstream stream(
            command);

        std::string operation;

        stream >> operation;

        if (operation == "PING")
        {
            Log(
                "RX: PING");

            return;
        }

        if (operation == "STOP")
        {
            Log(
                "RX: STOP");

            StopSpring();

            return;
        }

        if (operation == "CENTER")
        {
            Log(
                "RX: CENTER");

            SetSpring(
                0.0f,
                0.0f,
                0.5f);

            return;
        }

        if (operation == "SPRING")
        {
            float x = 0.0f;
            float y = 0.0f;
            float strength = 0.0f;

            if (stream >>
                x >>
                y >>
                strength)
            {
                Log(
                    "RX: SPRING %.3f %.3f %.3f",
                    x,
                    y,
                    strength);

                SetSpring(
                    x,
                    y,
                    strength);

                return;
            }

            Log(
                "RX: malformed SPRING command.");

            return;
        }

        if (!operation.empty())
        {
            Log(
                "RX: unknown command: %s",
                operation.c_str());
        }
    }


    // -------------------------------------------------------------------------
    // UDP worker
    // -------------------------------------------------------------------------

    void NetworkThread()
    {
        while (g_running)
        {
            char buffer[1024]{};

            sockaddr_in sender{};

            int senderLength =
                sizeof(sender);

            const int received =
                recvfrom(
                    g_socket,
                    buffer,
                    sizeof(buffer) - 1,
                    0,
                    reinterpret_cast<sockaddr*>(
                        &sender),
                    &senderLength);

            if (received > 0)
            {
                buffer[received] =
                    '\0';

                {
                    std::lock_guard<std::mutex> lock(
                        g_stateMutex);

                    g_state.lastCommand =
                        std::chrono::steady_clock::now();
                }

                ProcessCommand(
                    std::string(buffer));
            }

            //
            // Ignore normal receive timeouts.
            //
            if (received == SOCKET_ERROR)
            {
                const int error =
                    WSAGetLastError();

                if (error != WSAETIMEDOUT &&
                    error != WSAEINTR)
                {
                    if (g_running)
                    {
                        Log(
                            "recvfrom() failed: %d",
                            error);
                    }
                }
            }

            //
            // FFB watchdog.
            //
            bool timedOut = false;

            {
                std::lock_guard<std::mutex> lock(
                    g_stateMutex);

                const auto now =
                    std::chrono::steady_clock::now();

                const auto elapsed =
                    std::chrono::duration_cast<
                        std::chrono::milliseconds>(
                            now -
                            g_state.lastCommand);

                timedOut =
                    elapsed.count() >
                    COMMAND_TIMEOUT_MS;
            }

            if (timedOut)
            {
                StopSpring();
            }
        }
    }


    // -------------------------------------------------------------------------
    // UDP startup/shutdown
    // -------------------------------------------------------------------------

    bool StartUdpServer()
    {
        WSADATA wsaData{};

        const int result =
            WSAStartup(
                MAKEWORD(2, 2),
                &wsaData);

        if (result != 0)
        {
            Log(
                "WSAStartup failed: %d",
                result);

            return false;
        }

        g_socket =
            socket(
                AF_INET,
                SOCK_DGRAM,
                IPPROTO_UDP);

        if (g_socket ==
            INVALID_SOCKET)
        {
            Log(
                "socket() failed: %d",
                WSAGetLastError());

            WSACleanup();

            return false;
        }

        DWORD receiveTimeout =
            SOCKET_TIMEOUT_MS;

        setsockopt(
            g_socket,
            SOL_SOCKET,
            SO_RCVTIMEO,
            reinterpret_cast<const char*>(
                &receiveTimeout),
            sizeof(receiveTimeout));

        sockaddr_in address{};

        address.sin_family =
            AF_INET;

        address.sin_port =
            htons(UDP_PORT);

        if (inet_pton(
                AF_INET,
                "127.0.0.1",
                &address.sin_addr) != 1)
        {
            Log(
                "inet_pton() failed.");

            closesocket(g_socket);
            g_socket =
                INVALID_SOCKET;

            WSACleanup();

            return false;
        }

        if (bind(
                g_socket,
                reinterpret_cast<sockaddr*>(
                    &address),
                sizeof(address)) != 0)
        {
            Log(
                "bind() failed: %d",
                WSAGetLastError());

            closesocket(g_socket);
            g_socket =
                INVALID_SOCKET;

            WSACleanup();

            return false;
        }

        Log(
            "UDP server listening on 127.0.0.1:%d.",
            UDP_PORT);

        g_running =
            true;

        g_networkThread =
            std::thread(
                NetworkThread);

        return true;
    }


    void StopUdpServer()
    {
        g_running =
            false;

        if (g_socket !=
            INVALID_SOCKET)
        {
            closesocket(
                g_socket);

            g_socket =
                INVALID_SOCKET;
        }

        if (g_networkThread.joinable())
        {
            g_networkThread.join();
        }

        WSACleanup();
    }


    // -------------------------------------------------------------------------
    // Win32 window procedure
    // -------------------------------------------------------------------------

    LRESULT CALLBACK WindowProcedure(
        HWND window,
        UINT message,
        WPARAM wParam,
        LPARAM lParam)
    {
        switch (message)
        {
        case WM_CREATE:
        {
            g_statusWindow =
                CreateWindowW(
                    L"STATIC",
                    L"",
                    WS_CHILD |
                    WS_VISIBLE,
                    12,
                    12,
                    700,
                    210,
                    window,
                    nullptr,
                    GetModuleHandleW(
                        nullptr),
                    nullptr);

            g_logWindow =
                CreateWindowExW(
                    WS_EX_CLIENTEDGE,
                    L"EDIT",
                    L"",
                    WS_CHILD |
                    WS_VISIBLE |
                    ES_MULTILINE |
                    ES_READONLY |
                    ES_AUTOVSCROLL |
                    WS_VSCROLL,
                    12,
                    230,
                    700,
                    280,
                    window,
                    nullptr,
                    GetModuleHandleW(
                        nullptr),
                    nullptr);

            UpdateStatus();

            return 0;
        }

        case WM_SIZE:
        {
            const int width =
                LOWORD(lParam);

            const int height =
                HIWORD(lParam);

            if (g_statusWindow != nullptr)
            {
                MoveWindow(
                    g_statusWindow,
                    12,
                    12,
                    std::max(
                        100,
                        width - 24),
                    210,
                    TRUE);
            }

            if (g_logWindow != nullptr)
            {
                MoveWindow(
                    g_logWindow,
                    12,
                    230,
                    std::max(
                        100,
                        width - 24),
                    std::max(
                        100,
                        height - 242),
                    TRUE);
            }

            return 0;
        }

        case WM_APP_LOG:
        {
            auto* message =
                reinterpret_cast<
                    std::wstring*>(
                        lParam);

            if (message != nullptr)
            {
                if (g_logWindow != nullptr)
                {
                    const int length =
                        GetWindowTextLengthW(
                            g_logWindow);

                    SendMessageW(
                        g_logWindow,
                        EM_SETSEL,
                        length,
                        length);

                    const std::wstring line =
                        *message +
                        L"\r\n";

                    SendMessageW(
                        g_logWindow,
                        EM_REPLACESEL,
                        FALSE,
                        reinterpret_cast<
                            LPARAM>(
                                line.c_str()));

                    //
                    // Keep the newest command visible.
                    //
                    SendMessageW(
                        g_logWindow,
                        EM_SCROLL,
                        SB_BOTTOM,
                        0);
                }

                delete message;
            }

            return 0;
        }

        case WM_DESTROY:
        {
            PostQuitMessage(0);

            return 0;
        }

        default:
            break;
        }

        return DefWindowProcW(
            window,
            message,
            wParam,
            lParam);
    }
}


// -----------------------------------------------------------------------------
// Application entry point
// -----------------------------------------------------------------------------

int APIENTRY wWinMain(
    HINSTANCE instance,
    HINSTANCE,
    LPWSTR,
    int showCommand)
{
    WNDCLASSW windowClass{};

    windowClass.hInstance =
        instance;

    windowClass.lpfnWndProc =
        WindowProcedure;

    windowClass.lpszClassName =
        L"MultiFFBJoyWindow";

    windowClass.hCursor =
        LoadCursorW(
            nullptr,
            IDC_ARROW);

    windowClass.hbrBackground =
        reinterpret_cast<HBRUSH>(
            COLOR_WINDOW + 1);

    if (!RegisterClassW(
            &windowClass))
    {
        return 1;
    }

    g_mainWindow =
        CreateWindowW(
            windowClass.lpszClassName,
            L"MultiFFBJoy - DirectInput FFB Bridge",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            760,
            560,
            nullptr,
            nullptr,
            instance,
            nullptr);

    if (g_mainWindow == nullptr)
    {
        return 1;
    }

    ShowWindow(
        g_mainWindow,
        showCommand);

    UpdateWindow(
        g_mainWindow);

    Log(
        "MultiFFBJoy starting.");

    //
    // Initialize DirectInput.
    //
    HRESULT hr =
        DirectInput8Create(
            instance,
            DIRECTINPUT_VERSION,
            IID_IDirectInput8W,
            reinterpret_cast<void**>(
                &g_directInput),
            nullptr);

    if (FAILED(hr))
    {
        Log(
            "DirectInput8Create failed: 0x%08lX",
            static_cast<unsigned long>(hr));
    }
    else
    {
        SelectFirstSuitableDevice();
    }

    //
    // Start UDP command server.
    //
    if (!StartUdpServer())
    {
        Log(
            "UDP server could not be started.");
    }

    //
    // Win32 message loop.
    //
    MSG message{};

    while (GetMessageW(
        &message,
        nullptr,
        0,
        0) > 0)
    {
        TranslateMessage(
            &message);

        DispatchMessageW(
            &message);
    }

    //
    // Shutdown order:
    //
    // 1. Stop receiving commands.
    // 2. Stop all FFB.
    // 3. Release DirectInput effect/device.
    // 4. Release DirectInput itself.
    //
    StopUdpServer();

    StopSpring();

    ReleaseFFBDevice();

    if (g_directInput != nullptr)
    {
        g_directInput->Release();

        g_directInput =
            nullptr;
    }

    return 0;
}