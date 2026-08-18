#define DIRECTINPUT_VERSION 0x0800
#define IDC_FFB_UP 2001
#define IDC_FFB_DOWN 2002
#define IDC_FFB_LEFT 2003
#define IDC_FFB_RIGHT 2004
#define IDC_FFB_STOP 2005
#define IDC_FFB_CENTER 2006
#include <windows.h>
#include <dinput.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <atomic>
#include <chrono>
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
    constexpr UINT WM_APP_LOG = WM_APP + 1;
// 65458 == 0xFFB2 == "FFB2"
    constexpr int UDP_PORT = 65458;
    constexpr DWORD COMMAND_TIMEOUT_MS = 250;
    constexpr DWORD SOCKET_TIMEOUT_MS = 25;
    HWND g_mainWindow = nullptr;
    HWND g_statusWindow = nullptr;
    HWND g_logWindow = nullptr;
    IDirectInput8W* g_directInput = nullptr;
    IDirectInputDevice8W* g_ffbDevice = nullptr;
    IDirectInputEffect* g_springEffect = nullptr;
    IDirectInputEffect* g_testConstantForceEffect = nullptr;
    IDirectInputEffect* g_testConstantEffect = nullptr;
    SOCKET g_socket = INVALID_SOCKET;
    std::thread g_networkThread;
    std::thread g_ffbWatchdogThread;
    std::atomic<bool> g_running{ false };
    std::mutex g_stateMutex;
    // Prevent overlapping DirectInput re-acquisition attempts.
    std::atomic<bool> g_reacquiring{false};
// -------------------------------------------------------------------------
// Device state
// -------------------------------------------------------------------------
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
    DeviceState g_state;
// -------------------------------------------------------------------------
// Device candidate
// -------------------------------------------------------------------------
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
        DWORD springCoordinates = 0;
        std::vector<DWORD> ffbActuatorOffsets;
    };
    std::vector<DeviceCandidate> g_candidates;
// -------------------------------------------------------------------------
// Logging
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
    void Log(const std::string& text)
    {
        auto* message =
        new std::wstring(
            Utf8ToWide(text.c_str()));
        if (g_mainWindow != nullptr)
        {
            PostMessageW(
                g_mainWindow,
                WM_APP_LOG,
                0,
                reinterpret_cast<LPARAM>(
                    message));
        }
        else
        {
            delete message;
        }
    }
template <typename... Args>
    void Logf(
        const char* format,
        Args... args)
    {
        char buffer[2048]{};
        sprintf_s(
            buffer,
            sizeof(buffer),
            format,
            args...);
        Log(buffer);
    }
    void SendUdpCommand(
        const std::string& command)
    {
        if (g_socket == INVALID_SOCKET)
        {
            Log(
                "TX failed: UDP socket is not available.");
            return;
        }
        sockaddr_in destination{};
        destination.sin_family =
        AF_INET;
        destination.sin_port =
        htons(
            static_cast<u_short>(
                UDP_PORT));
        inet_pton(
            AF_INET,
            "127.0.0.1",
            &destination.sin_addr);
        const int result =
        sendto(
            g_socket,
            command.c_str(),
            static_cast<int>(
                command.size()),
            0,
            reinterpret_cast<
            const sockaddr*>(
                &destination),
            sizeof(destination));
        if (result == SOCKET_ERROR)
        {
            Logf(
                "TX failed: %d",
                WSAGetLastError());
            return;
        }
        Logf(
            "TX: %s",
            command.c_str());
    }
// -------------------------------------------------------------------------
// Status
// -------------------------------------------------------------------------
    void UpdateStatus()
    {
        if (g_statusWindow == nullptr)
            return;
        DeviceState state;
        {
            std::lock_guard<std::mutex> lock(
                g_stateMutex);
            state = g_state;
        }
        wchar_t text[4096]{};
        swprintf_s(
            text,
            L"Selected device: %ls\r\n"
            L"Axes: %lu\r\n"
            L"Force Feedback: %s\r\n"
            L"Spring effect: %s\r\n"
            L"Acquired: %s\r\n"
            L"Logical X axis: 0x%lX\r\n"
            L"Logical Y axis: 0x%lX\r\n"
            L"Spring strength: %.3f\r\n"
            L"UDP: 127.0.0.1:%d\r\n"
            L"FFB safety timeout: %lu ms",
            state.name.c_str(),
            state.axisCount,
            state.forceFeedback ? L"Yes" : L"No",
            state.springSupported ? L"Yes" : L"No",
            state.acquired ? L"Yes" : L"No",
            static_cast<unsigned long>(
                state.xAxisOffset),
            static_cast<unsigned long>(
                state.yAxisOffset),
            state.springStrength,
            UDP_PORT,
            COMMAND_TIMEOUT_MS);
        SetWindowTextW(
            g_statusWindow,
            text);
    }
// -----------------------------------------------------------------------------
// DirectInput object enumeration
// -----------------------------------------------------------------------------
    struct AxisEnumerationContext
    {
        DWORD axisCount = 0;
        bool hasXAxis = false;
        bool hasYAxis = false;
        struct Axis
        {
            DWORD offset = 0;
            DWORD type = 0;
            DWORD flags = 0;
            bool isXAxis = false;
            bool isYAxis = false;
            bool isFFBActuator = false;
        };
        std::vector<Axis> axes;
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
        AxisEnumerationContext::Axis axis;
        axis.offset =
        object->dwOfs;
        axis.type =
        object->dwType;
        axis.flags =
        object->dwFlags;
        axis.isXAxis =
        object->dwOfs == DIJOFS_X;
        axis.isYAxis =
        object->dwOfs == DIJOFS_Y;
/*
* DIDFT_FFACTUATOR identifies an object that can be used
* as a force-feedback actuator.
*
* Do not assume that every X/Y axis is necessarily an
* FFB actuator.
*/
        axis.isFFBActuator =
        (object->dwType & DIDFT_FFACTUATOR) != 0;
        if (axis.isXAxis)
            context->hasXAxis = true;
        if (axis.isYAxis)
            context->hasYAxis = true;
        ++context->axisCount;
        context->axes.push_back(axis);
        return DIENUM_CONTINUE;
    }
// -------------------------------------------------------------------------
// FFB effect enumeration
// -------------------------------------------------------------------------
    struct EffectEnumerationContext
    {
        bool springSupported = false;
        DWORD springEffType = 0;
        DWORD springStaticParams = 0;
        DWORD springDynamicParams = 0;
    };
    BOOL CALLBACK EnumerateEffectsCallback(
        const DIEFFECTINFO* effectInfo,
        VOID* contextPointer)
    {
        if (effectInfo == nullptr ||
            contextPointer == nullptr)
        {
            return DIENUM_CONTINUE;
        }
        auto* context =
        static_cast<EffectEnumerationContext*>(
            contextPointer);
        if (IsEqualGUID(
            effectInfo->guid,
            GUID_Spring))
        {
            context->springSupported = true;
            context->springEffType =
            effectInfo->dwEffType;
            context->springStaticParams =
            effectInfo->dwStaticParams;
            context->springDynamicParams =
            effectInfo->dwDynamicParams;
            Logf(
                " GUID_Spring found: "
                "effType=0x%08lX "
                "static=0x%08lX dynamic=0x%08lX",
                static_cast<unsigned long>(
                    effectInfo->dwEffType),
                static_cast<unsigned long>(
                    effectInfo->dwStaticParams),
                static_cast<unsigned long>(
                    effectInfo->dwDynamicParams));
            return DIENUM_STOP;
        }
        return DIENUM_CONTINUE;
    }
    bool QuerySpringSupport(
        IDirectInputDevice8W* device,
        EffectEnumerationContext& result)
    {
        result = EffectEnumerationContext{};
        const HRESULT hr =
        device->EnumEffects(
            EnumerateEffectsCallback,
            &result,
            DIEFT_CONDITION);
        if (FAILED(hr))
        {
            Logf(
                "EnumEffects(DIEFT_CONDITION) failed: "
                "0x%08lX",
                static_cast<unsigned long>(hr));
            return false;
        }
        return result.springSupported;
    }
// -------------------------------------------------------------------------
// Device enumeration
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
        IDirectInputDevice8W* device =
        nullptr;
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
            std::vector<DWORD> ffbActuatorOffsets;
            for (const auto& axis : axes.axes)
            {
                if (axis.isFFBActuator)
                {
                    ffbActuatorOffsets.push_back(
                        axis.offset);
                }
            }
            Logf(
                " %ls: %lu axis object(s)",
                instance->tszProductName,
                axes.axisCount);
            for (size_t axisIndex = 0;
                axisIndex < axes.axes.size();
                ++axisIndex)
            {
                const auto& axis =
                axes.axes[axisIndex];
                Logf(
                    " axis[%zu]: offset=0x%lX type=0x%08lX "
                    "flags=0x%08lX X=%s Y=%s FFBActuator=%s",
                    axisIndex,
                    static_cast<unsigned long>(
                        axis.offset),
                    static_cast<unsigned long>(
                        axis.type),
                    static_cast<unsigned long>(
                        axis.flags),
                    axis.isXAxis ? "yes" : "no",
                    axis.isYAxis ? "yes" : "no",
                    axis.isFFBActuator ? "YES" : "no");
            }
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
            if (candidate.forceFeedback)
            {
                EffectEnumerationContext effects;
                if (QuerySpringSupport(
                    device,
                    effects))
                {
                    candidate.springSupported =
                    true;
                    candidate.springEffType =
                    effects.springEffType;
                    candidate.springStaticParams =
                    effects.springStaticParams;
                    candidate.springDynamicParams =
                    effects.springDynamicParams;
                }
            }
            candidate.ffbActuatorOffsets =
            ffbActuatorOffsets;
            g_candidates.push_back(
                candidate);
        }
        device->Release();
        return DIENUM_CONTINUE;
    }
// -------------------------------------------------------------------------
// Stop spring
// -------------------------------------------------------------------------
    void StopSpring()
    {
        if (g_springEffect != nullptr)
        {
            g_springEffect->Stop();
        }
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_state.springStrength = 0.0f;
            g_state.springPersistent = false;
        }
        UpdateStatus();
    }
    void StopSpringForRelease()
    {
        if (g_springEffect != nullptr)
        {
            HRESULT hr = g_springEffect->Stop();
            if (FAILED(hr) &&
                hr != DIERR_INPUTLOST &&
                hr != DIERR_NOTACQUIRED &&
                hr != DIERR_NOTEXCLUSIVEACQUIRED)
            {
                Logf(
                    "Spring Stop failed: 0x%08lX",
                    static_cast<unsigned long>(hr));
            }
        }
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_state.springStrength = 0.0f;
        // Intentionally preserve g_state.springPersistent.
        }
        UpdateStatus();
    }
    void StopTestConstantForce()
    {
        if (g_testConstantEffect == nullptr)
            return;
        const HRESULT hr =
        g_testConstantEffect->Stop();
        if (FAILED(hr))
        {
            Logf(
                "ConstantForce Stop failed: "
                "0x%08lX",
                static_cast<unsigned long>(hr));
            return;
        }
        Log(
            "Constant force stopped.");
    }
// -------------------------------------------------------------------------
// Release FFB device
// -------------------------------------------------------------------------
    void ReleaseFFBDevice()
    {
        StopSpringForRelease();
        StopTestConstantForce();
        if (g_springEffect != nullptr)
        {
            g_springEffect->Release();
            g_springEffect = nullptr;
        }
        if (g_testConstantForceEffect != nullptr)
        {
            g_testConstantForceEffect->Release();
            g_testConstantForceEffect = nullptr;
        }
        if (g_ffbDevice != nullptr)
        {
            g_ffbDevice->Unacquire();
            g_ffbDevice->Release();
            g_ffbDevice = nullptr;
        }
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_state.acquired = false;
            g_state.springStrength = 0.0f;
        // Do NOT clear springPersistent here.
        }
    }
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
            conditions[i].lOffset =
            0;
            conditions[i].lPositiveCoefficient =
            0;
            conditions[i].lNegativeCoefficient =
            0;
            conditions[i].dwPositiveSaturation =
            DI_FFNOMINALMAX;
            conditions[i].dwNegativeSaturation =
            DI_FFNOMINALMAX;
            conditions[i].lDeadBand =
            0;
        }
        DIEFFECT effect{};
        effect.dwSize =
        sizeof(DIEFFECT);
        effect.dwFlags =
        DIEFF_CARTESIAN |
        DIEFF_OBJECTOFFSETS;
        effect.dwDuration =
        INFINITE;
        effect.dwSamplePeriod =
        0;
        effect.dwGain =
        DI_FFNOMINALMAX;
        effect.dwTriggerButton =
        DIEB_NOTRIGGER;
        effect.dwTriggerRepeatInterval =
        0;
        effect.cAxes =
        2;
        effect.rgdwAxes =
        axes;
        effect.rglDirection =
        directions;
        effect.lpEnvelope =
        nullptr;
        effect.cbTypeSpecificParams =
        sizeof(conditions);
        effect.lpvTypeSpecificParams =
        conditions;
        HRESULT hr =
        g_ffbDevice->CreateEffect(
            GUID_Spring,
            &effect,
            &g_springEffect,
            nullptr);
        if (FAILED(hr))
        {
            Logf(
                "CreateEffect(GUID_Spring) failed: "
                "0x%08lX",
                static_cast<unsigned long>(hr));
            g_springEffect =
            nullptr;
            return false;
        }
        Log(
            "Spring effect created.");
        return true;
    }
// -------------------------------------------------------------------------
// Create spring effect
// -------------------------------------------------------------------------
    bool CreateTestConstantForceEffect()
    {
        if (g_ffbDevice == nullptr)
            return false;
        DWORD axes[2] =
        {
            DIJOFS_X,
            DIJOFS_Y
        };
        LONG direction[2] =
        {
            0,
            0
        };
        DICONSTANTFORCE constantForce;
        ZeroMemory(
            &constantForce,
            sizeof(constantForce));
        constantForce.lMagnitude = 0;
        DIEFFECT effect;
        ZeroMemory(
            &effect,
            sizeof(effect));
        effect.dwSize =
        sizeof(DIEFFECT);
        effect.dwFlags =
        DIEFF_CARTESIAN |
        DIEFF_OBJECTOFFSETS;
        effect.dwDuration =
        INFINITE;
        effect.dwSamplePeriod =
        0;
        effect.dwGain =
        DI_FFNOMINALMAX;
        effect.dwTriggerButton =
        DIEB_NOTRIGGER;
        effect.dwTriggerRepeatInterval =
        0;
        effect.cAxes =
        2;
        effect.rgdwAxes =
        axes;
        effect.rglDirection =
        direction;
        effect.lpEnvelope =
        nullptr;
        effect.cbTypeSpecificParams =
        sizeof(DICONSTANTFORCE);
        effect.lpvTypeSpecificParams =
        &constantForce;
        HRESULT hr =
        g_ffbDevice->CreateEffect(
            GUID_ConstantForce,
            &effect,
            &g_testConstantEffect,
            nullptr);
        if (FAILED(hr))
        {
            Logf(
                "CreateEffect(GUID_ConstantForce) failed: "
                "0x%08lX",
                static_cast<unsigned long>(hr));
            g_testConstantEffect =
            nullptr;
            return false;
        }
        Log(
            "Constant-force test effect created.");
        return true;
    }
// -------------------------------------------------------------------------
// Select suitable FFB device
// -------------------------------------------------------------------------
    bool SelectFirstSuitableDevice()
    {
        ReleaseFFBDevice();
        g_candidates.clear();
        const HRESULT hr =
        g_directInput->EnumDevices(
            DI8DEVCLASS_GAMECTRL,
            EnumerateDevicesCallback,
            nullptr,
            DIEDFL_ATTACHEDONLY);
        if (FAILED(hr))
        {
            Logf(
                "DirectInput enumeration failed: "
                "0x%08lX",
                static_cast<unsigned long>(hr));
            return false;
        }
        Logf(
            "Found %zu attached game-controller device(s).",
            g_candidates.size());
        for (size_t i = 0;
            i < g_candidates.size();
            ++i)
        {
            const auto& candidate =
            g_candidates[i];
            Logf(
                "[%zu] %ls | axes=%lu | X=%s | Y=%s | "
                "FFB=%s | Spring=%s",
                i,
                candidate.name.c_str(),
                candidate.axisCount,
                candidate.hasXAxis ? "yes" : "no",
                candidate.hasYAxis ? "yes" : "no",
                candidate.forceFeedback ? "yes" : "no",
                candidate.springSupported ? "yes" : "no");
        }
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
/*
 * A DirectInput FFB device must actually expose
 * FFB actuator objects.
 *
 * This intentionally rejects vJoy in the current
 * configuration because its X/Y objects are not
 * DIDFT_FFACTUATOR objects.
 */
            if (candidate.ffbActuatorOffsets.size() < 2)
            {
                Logf(
                    "Skipping %ls: only %zu FFB actuator axis(es).",
                    candidate.name.c_str(),
                    candidate.ffbActuatorOffsets.size());
                continue;
            }
            if (!candidate.springSupported)
                continue;
            Logf(
                "Attempting FFB device: %ls",
                candidate.name.c_str());
            IDirectInputDevice8W* device =
            nullptr;
            HRESULT openResult =
            g_directInput->CreateDevice(
                candidate.guid,
                &device,
                nullptr);
            if (FAILED(openResult) ||
                device == nullptr)
            {
                Logf(
                    "Could not open %ls: 0x%08lX",
                    candidate.name.c_str(),
                    static_cast<unsigned long>(
                        openResult));
                continue;
            }
            openResult =
            device->SetDataFormat(
                &c_dfDIJoystick2);
            if (FAILED(openResult))
            {
                Logf(
                    "SetDataFormat failed for %ls: "
                    "0x%08lX",
                    candidate.name.c_str(),
                    static_cast<unsigned long>(
                        openResult));
                device->Release();
                continue;
            }
            openResult =
            device->SetCooperativeLevel(
                g_mainWindow,
                DISCL_BACKGROUND |
                DISCL_EXCLUSIVE);
            if (FAILED(openResult))
            {
                Logf(
                    "SetCooperativeLevel failed for %ls: "
                    "0x%08lX",
                    candidate.name.c_str(),
                    static_cast<unsigned long>(
                        openResult));
                device->Release();
                continue;
            }
/*
 * Disable hardware auto-center BEFORE acquiring.
 *
 * We want all centering behavior to come from our
 * software-controlled DirectInput spring effect.
 */
            DIPROPDWORD autoCenter{};
            autoCenter.diph.dwSize =
            sizeof(DIPROPDWORD);
            autoCenter.diph.dwHeaderSize =
            sizeof(DIPROPHEADER);
            autoCenter.diph.dwObj =
            0;
            autoCenter.diph.dwHow =
            DIPH_DEVICE;
            autoCenter.dwData =
            DIPROPAUTOCENTER_OFF;
            const HRESULT autoCenterResult =
            device->SetProperty(
                DIPROP_AUTOCENTER,
                &autoCenter.diph);
            if (FAILED(autoCenterResult))
            {
                Logf(
                    "Warning: could not disable hardware "
                    "auto-center before Acquire: 0x%08lX",
                    static_cast<unsigned long>(
                        autoCenterResult));
            }
            else
            {
                Log(
                    "Hardware auto-center disabled.");
            }
            openResult =
            device->Acquire();
            if (FAILED(openResult))
            {
                Logf(
                    "Acquire failed for %ls: "
                    "0x%08lX",
                    candidate.name.c_str(),
                    static_cast<unsigned long>(
                        openResult));
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
                g_state.springSupported =
                candidate.springSupported;
                g_state.acquired =
                true;
                g_state.xAxisOffset =
                DIJOFS_X;
                g_state.yAxisOffset =
                DIJOFS_Y;
            }
            Logf(
                "Selected FFB device: %ls",
                candidate.name.c_str());
            if (!CreateSpringEffect())
            {
                Log(
                    "Failed to create spring effect.");
                ReleaseFFBDevice();
                continue;
            }
            if (!CreateTestConstantForceEffect())
            {
                Log(
                    "Failed to create constant-force test effect.");
                ReleaseFFBDevice();
                continue;
            }
            UpdateStatus();
            if (!SelectFirstSuitableDevice())
            {
                Log("No suitable FFB joystick available at startup.");
            }
            else
            {
                Log("FFB joystick initialized successfully.");
            }
            g_ffbWatchdogThread = std::thread(FFBWatchdogThread);
            return true;
        }
        Log(
            "No suitable 2-axis FFB joystick found.");
        return false;
    }
    bool EnsureFFBDeviceReady()
    {
        if (g_ffbDevice == nullptr)
        {
            Log(
                "FFB device is not initialized; attempting reacquisition.");
            return SelectFirstSuitableDevice();
        }
/*
 * Querying the device state tells us whether DirectInput
 * still considers our interface acquired.
 */
        DIJOYSTATE2 joystickState{};
        HRESULT hr =
        g_ffbDevice->GetDeviceState(
            sizeof(DIJOYSTATE2),
            &joystickState);
        if (SUCCEEDED(hr))
        {
            {
                std::lock_guard<std::mutex> lock(
                    g_stateMutex);
                g_state.acquired =
                true;
            }
            return true;
        }
/*
 * BeamNG may have taken the exclusive DirectInput
 * interface away from us.
 *
 * Try the inexpensive recovery path first.
 */
        if (hr == DIERR_INPUTLOST ||
            hr == DIERR_NOTACQUIRED)
        {
            Logf(
                "FFB device acquisition lost "
                "(0x%08lX); attempting Acquire().",
                static_cast<unsigned long>(hr));
            hr =
            g_ffbDevice->Acquire();
            if (SUCCEEDED(hr))
            {
                {
                    std::lock_guard<std::mutex> lock(
                        g_stateMutex);
                    g_state.acquired =
                    true;
                }
                Log(
                    "FFB device reacquired.");
                return true;
            }
            Logf(
                "Acquire() failed: 0x%08lX",
                static_cast<unsigned long>(hr));
        }
        else
        {
            Logf(
                "GetDeviceState() failed: 0x%08lX",
                static_cast<unsigned long>(hr));
        }
/*
 * The existing DirectInput interface is no longer usable.
 *
 * Completely release it and enumerate again. This is
 * especially important when BeamNG has acquired the
 * device exclusively and our old DirectInput object
 * cannot simply be re-acquired.
 */
        Log(
            "Reinitializing FFB device.");
        if (!SelectFirstSuitableDevice())
        {
            Log(
                "FFB device reacquisition failed.");
            return false;
        }
        Log(
            "FFB device successfully reinitialized.");
        return true;
    }
// -------------------------------------------------------------------------
// Set spring strength
// -------------------------------------------------------------------------
    bool SetSpringStrength(float strength)
    {
        if (g_ffbDevice == nullptr || g_springEffect == nullptr)
        {
            Log("SetSpringStrength: FFB device/effect unavailable.");
            return false;
        }
        strength = std::clamp(strength, 0.0f, 1.0f);
        LONG magnitude = static_cast<LONG>(
            std::lround(strength * static_cast<float>(DI_FFNOMINALMAX)));
        DICONSTANTFORCE constantForce{};
        constantForce.lMagnitude = magnitude;
        DIEFFECT effect{};
        effect.dwSize = sizeof(DIEFFECT);
        effect.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
        effect.dwDuration = INFINITE;
        effect.dwSamplePeriod = 0;
        effect.dwGain = DI_FFNOMINALMAX;
        effect.dwTriggerButton = DIEB_NOTRIGGER;
        effect.dwTriggerRepeatInterval = INFINITE;
        effect.cAxes = 2;
        LONG axes[2] = { DIJOFS_X, DIJOFS_Y };
        LONG directions[2] = { 0, 0 };
        effect.rgdwAxes = axes;
        effect.rglDirection = directions;
    /*
     * The spring effect is already created with its spring-specific
     * parameters. We only need to update its magnitude here.
     *
     * IMPORTANT:
     * DIEP_START is deliberately included below. Merely calling
     * SetParameters() does not guarantee that the effect remains
     * actively running after DirectInput ownership has changed.
     */
        DICONDITION condition[2]{};
        condition[0].lOffset = 0;
        condition[0].lPositiveCoefficient = magnitude;
        condition[0].lNegativeCoefficient = magnitude;
        condition[0].dwPositiveSaturation = DI_FFNOMINALMAX;
        condition[0].dwNegativeSaturation = DI_FFNOMINALMAX;
        condition[0].lDeadBand = 0;
        condition[1] = condition[0];
        DIEFFECT springEffect{};
        springEffect.dwSize = sizeof(DIEFFECT);
        springEffect.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
        springEffect.dwDuration = INFINITE;
        springEffect.dwSamplePeriod = 0;
        springEffect.dwGain = DI_FFNOMINALMAX;
        springEffect.dwTriggerButton = DIEB_NOTRIGGER;
        springEffect.dwTriggerRepeatInterval = INFINITE;
        springEffect.cAxes = 2;
        springEffect.rgdwAxes = axes;
        springEffect.rglDirection = directions;
        springEffect.cbTypeSpecificParams = sizeof(condition);
        springEffect.lpvTypeSpecificParams = condition;
        HRESULT hr = g_springEffect->SetParameters(
            &springEffect,
            DIEP_TYPESPECIFICPARAMS |
            DIEP_DIRECTION |
            DIEP_START);
        if (FAILED(hr))
        {
            Logf(
                "SetParameters(Spring) failed: 0x%08lX",
                static_cast<unsigned long>(hr));
        /*
         * If the device was lost between the readiness check and
         * SetParameters(), don't silently leave the effect dead.
         */
            if (hr == DIERR_INPUTLOST ||
                hr == DIERR_NOTACQUIRED ||
                hr == DIERR_NOTEXCLUSIVEACQUIRED)
            {
                Log("Spring effect lost device access.");
                return false;
            }
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_state.springStrength = strength;
            g_state.springPersistent = (strength > 0.0f);
        }
        Logf(
            "Spring strength set to %.3f and effect started.",
            strength);
        return true;
    }
// -------------------------------------------------------------------------
// Force a complete FFB device re-acquisition.
//
// BeamNG can temporarily take the DirectInput device exclusively.  Merely
// calling Acquire() on our existing interface is not sufficient in that
// situation because the existing effect objects may also belong to the old
// acquisition state.
//
// This function therefore:
//   1. remembers the persistent spring state
//   2. stops/releases all effects
//   3. releases the DirectInput device
//   4. waits briefly
//   5. enumerates and opens the device again
//   6. waits for exclusive ownership to become usable
//   7. restores the persistent spring
// -------------------------------------------------------------------------
// -------------------------------------------------------------------------
// Re-acquire FFB device after BeamNG may have taken/released ownership
// -------------------------------------------------------------------------
    bool ReacquireFFBDevice()
    {
        bool expected = false;
        if (!g_reacquiring.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acquire,
            std::memory_order_relaxed))
        {
            Log("REACQUIRE ignored: another re-acquisition is already in progress.");
            return false;
        }
        struct ReacquireGuard
        {
            ~ReacquireGuard()
            {
                g_reacquiring.store(
                    false,
                    std::memory_order_release);
            }
        } reacquireGuard;
        float previousSpringStrength = 0.0f;
        bool restoreSpring = false;
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            previousSpringStrength =
            g_state.springStrength;
            restoreSpring =
            g_state.springPersistent;
        }
        Log("Re-acquiring FFB device...");
        ReleaseFFBDevice();
    // Give BeamNG / DirectInput a moment to release the old interface.
        Sleep(100);
        constexpr int MAX_ATTEMPTS = 30;
        constexpr DWORD RETRY_DELAY_MS = 100;
        ...
        bool IsFFBDeviceUsable()
        {
            if (g_ffbDevice == nullptr)
                return false;
            DIJOYSTATE2 state{};
            HRESULT hr = g_ffbDevice->GetDeviceState(
                sizeof(DIJOYSTATE2),
                &state);
            if (SUCCEEDED(hr))
                return true;
            Logf(
                "FFB device health check failed: 0x%08lX",
                static_cast<unsigned long>(hr));
            if (hr == DIERR_INPUTLOST ||
                hr == DIERR_NOTACQUIRED ||
                hr == DIERR_NOTEXCLUSIVEACQUIRED)
            {
                return false;
            }
            return false;
        }
    }
    void FFBWatchdogThread()
    {
        Log("FFB watchdog thread started.");
        while (g_running)
        {
            if (g_ffbDevice != nullptr)
            {
                if (!IsFFBDeviceUsable())
                {
                    Log(
                        "FFB watchdog detected lost device; "
                        "starting re-acquisition.");
                    if (ReacquireFFBDevice())
                    {
                        Log(
                            "FFB watchdog re-acquisition "
                            "completed successfully.");
                    }
                    else
                    {
                        Log(
                            "FFB watchdog re-acquisition failed.");
                    }
                }
            }
            Sleep(100);
        }
        Log("FFB watchdog thread stopped.");
    }
// -------------------------------------------------------------------------
// Hardware auto-center
// -------------------------------------------------------------------------
    void DisableHardwareAutoCenter()
    {
        if (g_ffbDevice == nullptr)
            return;
        DIPROPDWORD property{};
        property.diph.dwSize =
        sizeof(property);
        property.diph.dwHeaderSize =
        sizeof(DIPROPHEADER);
        property.diph.dwObj =
        0;
        property.diph.dwHow =
        DIPH_DEVICE;
        property.dwData =
        DIPROPAUTOCENTER_OFF;
        const HRESULT hr =
        g_ffbDevice->SetProperty(
            DIPROP_AUTOCENTER,
            &property.diph);
        if (FAILED(hr))
        {
            Logf(
                "Warning: could not disable hardware "
                "auto-center: 0x%08lX",
                static_cast<unsigned long>(hr));
        }
        else
        {
            Log(
                "Hardware auto-center disabled.");
        }
    }
    bool SetTestConstantForce(
        LONG x,
        LONG y)
    {
        if (g_testConstantEffect == nullptr)
            return false;
        DICONSTANTFORCE constantForce;
        ZeroMemory(
            &constantForce,
            sizeof(constantForce));
/*
* The magnitude is kept at nominal maximum and
* the vector is represented by the direction.
*
* x/y are expected in the range -10000..10000.
*/
        LONG direction[2] =
        {
            x,
            y
        };
        LONG magnitude =
        static_cast<LONG>(
            std::sqrt(
                static_cast<double>(x) * x +
                static_cast<double>(y) * y));
        if (magnitude > DI_FFNOMINALMAX)
            magnitude = DI_FFNOMINALMAX;
        if (magnitude < 0)
            magnitude = 0;
        constantForce.lMagnitude =
        magnitude;
        DIEFFECT effect;
        ZeroMemory(
            &effect,
            sizeof(effect));
        effect.dwSize =
        sizeof(DIEFFECT);
        effect.dwFlags =
        DIEFF_CARTESIAN |
        DIEFF_OBJECTOFFSETS;
        effect.cAxes =
        2;
        DWORD axes[2] =
        {
            DIJOFS_X,
            DIJOFS_Y
        };
        effect.rgdwAxes =
        axes;
        effect.rglDirection =
        direction;
        effect.cbTypeSpecificParams =
        sizeof(DICONSTANTFORCE);
        effect.lpvTypeSpecificParams =
        &constantForce;
        HRESULT hr =
        g_testConstantEffect->SetParameters(
            &effect,
            DIEP_DIRECTION |
            DIEP_TYPESPECIFICPARAMS);
        if (FAILED(hr))
        {
            Logf(
                "SetParameters(ConstantForce) failed: "
                "0x%08lX",
                static_cast<unsigned long>(hr));
            return false;
        }
        hr =
        g_testConstantEffect->Start(
            1,
            0);
        if (FAILED(hr))
        {
            Logf(
                "ConstantForce Start failed: "
                "0x%08lX",
                static_cast<unsigned long>(hr));
            return false;
        }
        Logf(
            "Constant force applied: X=%ld Y=%ld magnitude=%ld",
            x,
            y,
            magnitude);
        return true;
    }
// --------------------------------------------------------------------
// Process UDP command
// -------------------------------------------------------------------------
    void ProcessCommand(
        const std::string& command)
    {
        std::istringstream stream(
            command);
        std::string operation;
        stream >>
        operation;
        if (operation == "PING")
        {
            Log(
                "RX: PING");
            return;
        }
        if (operation == "REACQUIRE")
        {
            Log("RX: REACQUIRE");
            if (ReacquireFFBDevice())
            {
                Log(
                    "REACQUIRE completed successfully.");
            }
            else
            {
                Log(
                    "REACQUIRE failed.");
            }
            return;
        }
        if (operation == "STOP")
        {
            Log(
                "RX: STOP");
            StopSpring();
            StopTestConstantForce();
            return;
        }
        if (operation == "CENTER")
        {
            Log("RX: CENTER");
            if (!EnsureFFBDeviceReady())
            {
                Log(
                    "CENTER: device unavailable; "
                    "attempting full re-acquisition.");
                if (!ReacquireFFBDevice())
                {
                    Log(
                        "CENTER ignored: FFB re-acquisition failed.");
                    return;
                }
            }
            if (SetSpringStrength(1.0f))
            {
                return;
            }
            Log(
                "CENTER: spring download failed; "
                "attempting full FFB re-acquisition.");
            if (!ReacquireFFBDevice())
            {
                Log(
                    "CENTER: FFB re-acquisition failed.");
                return;
            }
    // CENTER explicitly requests a live 100% spring.
    //
    // ReacquireFFBDevice() may restore an existing persistent
    // spring, but CENTER must also work if there was no
    // persistent spring before the command.
            if (!SetSpringStrength(1.0f))
            {
                Log(
                    "CENTER: spring still could not be started.");
                return;
            }
            return;
        }
        if (operation == "SPRING")
        {
            float strength = 0.0f;
            if (stream >>
                strength)
            {
                Logf(
                    "RX: SPRING %.3f",
                    strength);
                if (!EnsureFFBDeviceReady())
                {
                    Log(
                        "SPRING ignored: FFB device unavailable.");
                    return;
                }
                SetSpringStrength(
                    strength);
                return;
            }
            Log(
                "RX: malformed SPRING command.");
            return;
        }
/*
 * TEST_FFB X Y
 *
 * X and Y are DirectInput force directions in the
 * range -10000..10000.
 */
        if (operation == "TEST_FFB")
        {
            LONG x = 0;
            LONG y = 0;
            if (stream >>
                x >>
                y)
            {
                x =
                std::clamp<LONG>(
                    x,
                    -DI_FFNOMINALMAX,
                    DI_FFNOMINALMAX);
                y =
                std::clamp<LONG>(
                    y,
                    -DI_FFNOMINALMAX,
                    DI_FFNOMINALMAX);
                Logf(
                    "RX: TEST_FFB %ld %ld",
                    x,
                    y);
                if (x == 0 &&
                    y == 0)
                {
                    StopTestConstantForce();
                    return;
                }
                if (!EnsureFFBDeviceReady())
                {
                    Log(
                        "TEST_FFB ignored: FFB device unavailable.");
                    return;
                }
                SetTestConstantForce(
                    x,
                    y);
                return;
            }
            Logf(
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
            if (received == SOCKET_ERROR)
            {
                const int error =
                WSAGetLastError();
                if (error != WSAETIMEDOUT &&
                    error != WSAEINTR &&
                    g_running)
                {
                    Logf(
                        "recvfrom() failed: %d",
                        error);
                }
            }
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
                bool springPersistent = false;
                {
                    std::lock_guard<std::mutex> lock(
                        g_stateMutex);
                    springPersistent =
                    g_state.springPersistent;
                }
                if (!springPersistent)
                {
                    StopSpring();
                }
            }
        }
    }
// -------------------------------------------------------------------------
// UDP startup
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
            Logf(
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
            Logf(
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
        htons(
            static_cast<u_short>(
                UDP_PORT));
        inet_pton(
            AF_INET,
            "127.0.0.1",
            &address.sin_addr);
        if (bind(
            g_socket,
            reinterpret_cast<sockaddr*>(
                &address),
            sizeof(address)) != 0)
        {
            Logf(
                "bind() failed: %d",
                WSAGetLastError());
            closesocket(
                g_socket);
            g_socket =
            INVALID_SOCKET;
            WSACleanup();
            return false;
        }
        Logf(
            "UDP server listening on 127.0.0.1:%d.",
            UDP_PORT);
        g_running =
        true;
        g_networkThread =
        std::thread(
            NetworkThread);
        return true;
    }
// -------------------------------------------------------------------------
// UDP shutdown
// -------------------------------------------------------------------------
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
// -----------------------------------------------------------------------------
// Send a UDP command to the local MultiFFBJoy command socket
// -----------------------------------------------------------------------------
    void SendUdpCommand(
        const char* command)
    {
        if (g_socket == INVALID_SOCKET ||
            command == nullptr)
        {
            return;
        }
        sockaddr_in destination{};
        destination.sin_family =
        AF_INET;
        destination.sin_port =
        htons(
            static_cast<u_short>(
                UDP_PORT));
        inet_pton(
            AF_INET,
            "127.0.0.1",
            &destination.sin_addr);
        const int length =
        static_cast<int>(
            strlen(command));
        const int sent =
        sendto(
            g_socket,
            command,
            length,
            0,
            reinterpret_cast<
            const sockaddr*>(
                &destination),
            sizeof(destination));
        if (sent == SOCKET_ERROR)
        {
            Logf(
                "TX failed: %d",
                WSAGetLastError());
        }
        else
        {
            Logf(
                "TX: %s",
                command);
        }
    }
// -------------------------------------------------------------------------
// Win32 window procedure
// -------------------------------------------------------------------------
    LRESULT CALLBACK WindowProcedure(
        HWND window,
        UINT messageId,
        WPARAM wParam,
        LPARAM lParam)
    {
        switch (messageId)
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
                    GetModuleHandleW(nullptr),
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
                    180,
                    window,
                    nullptr,
                    GetModuleHandleW(nullptr),
                    nullptr);
                CreateWindowExW(
                    0,
                    L"BUTTON",
                    L"UP",
                    WS_CHILD |
                    WS_VISIBLE |
                    BS_PUSHBUTTON,
                    120,
                    430,
                    80,
                    30,
                    window,
                    reinterpret_cast<HMENU>(
                        IDC_FFB_UP),
                    GetModuleHandleW(nullptr),
                    nullptr);
                CreateWindowExW(
                    0,
                    L"BUTTON",
                    L"LEFT",
                    WS_CHILD |
                    WS_VISIBLE |
                    BS_PUSHBUTTON,
                    30,
                    465,
                    80,
                    30,
                    window,
                    reinterpret_cast<HMENU>(
                        IDC_FFB_LEFT),
                    GetModuleHandleW(nullptr),
                    nullptr);
                CreateWindowExW(
                    0,
                    L"BUTTON",
                    L"STOP",
                    WS_CHILD |
                    WS_VISIBLE |
                    BS_PUSHBUTTON,
                    120,
                    465,
                    80,
                    30,
                    window,
                    reinterpret_cast<HMENU>(
                        IDC_FFB_STOP),
                    GetModuleHandleW(nullptr),
                    nullptr);
                CreateWindowExW(
                    0,
                    L"BUTTON",
                    L"CENTER",
                    WS_CHILD |
                    WS_VISIBLE |
                    BS_PUSHBUTTON,
                    300,
                    465,
                    90,
                    30,
                    window,
                    reinterpret_cast<HMENU>(
                        IDC_FFB_CENTER),
                    GetModuleHandleW(nullptr),
                    nullptr);
                CreateWindowExW(
                    0,
                    L"BUTTON",
                    L"RIGHT",
                    WS_CHILD |
                    WS_VISIBLE |
                    BS_PUSHBUTTON,
                    210,
                    465,
                    80,
                    30,
                    window,
                    reinterpret_cast<HMENU>(
                        IDC_FFB_RIGHT),
                    GetModuleHandleW(nullptr),
                    nullptr);
                CreateWindowExW(
                    0,
                    L"BUTTON",
                    L"DOWN",
                    WS_CHILD |
                    WS_VISIBLE |
                    BS_PUSHBUTTON,
                    120,
                    500,
                    80,
                    30,
                    window,
                    reinterpret_cast<HMENU>(
                        IDC_FFB_DOWN),
                    GetModuleHandleW(nullptr),
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
                            height - 360),
                        TRUE);
                }
                return 0;
            }
        case WM_COMMAND:
            {
                if (HIWORD(wParam) != BN_CLICKED)
                {
                    break;
                }
                const int controlId =
                LOWORD(wParam);
                switch (controlId)
                {
                case IDC_FFB_UP:
        /*
         * DirectInput Y direction is inverted relative
         * to the physical/UI convention we're using.
         */
                    Log(
                        "TX: TEST_FFB 0 10000");
                    SendUdpCommand(
                        "TEST_FFB 0 10000");
                    return 0;
                case IDC_FFB_DOWN:
                    Log(
                        "TX: TEST_FFB 0 -10000");
                    SendUdpCommand(
                        "TEST_FFB 0 -10000");
                    return 0;
                case IDC_FFB_LEFT:
                    Log(
                        "TX: TEST_FFB 10000 0");
                    SendUdpCommand(
                        "TEST_FFB 10000 0");
                    return 0;
                case IDC_FFB_RIGHT:
                    Log(
                        "TX: TEST_FFB -10000 0");
                    SendUdpCommand(
                        "TEST_FFB -10000 0");
                    return 0;
                case IDC_FFB_STOP:
                    Log(
                        "TX: TEST_FFB 0 0");
                    SendUdpCommand(
                        "TEST_FFB 0 0");
                    return 0;
                case IDC_FFB_CENTER:
                    Log(
                        "TX: CENTER");
                    SendUdpCommand(
                        "CENTER");
                    return 0;
                default:
                    break;
                }
                break;
            }
        case WM_APP_LOG:
            {
                auto* logMessage =
                reinterpret_cast<
                std::wstring*>(
                    lParam);
                if (logMessage != nullptr)
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
                        *logMessage +
                        L"\r\n";
                        SendMessageW(
                            g_logWindow,
                            EM_REPLACESEL,
                            FALSE,
                            reinterpret_cast<LPARAM>(
                                line.c_str()));
                        SendMessageW(
                            g_logWindow,
                            EM_SCROLL,
                            SB_BOTTOM,
                            0);
                    }
                    delete logMessage;
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
            messageId,
            wParam,
            lParam);
    }
}
// -----------------------------------------------------------------------------
// Entry point
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
        Logf(
            "DirectInput8Create failed: "
            "0x%08lX",
            static_cast<unsigned long>(hr));
    }
    else
    {
        SelectFirstSuitableDevice();
    }
    if (!StartUdpServer())
    {
        Log(
            "UDP server could not be started.");
    }
    MSG winMessage{};
    while (GetMessageW(
        &winMessage,
        nullptr,
        0,
        0) > 0)
    {
        TranslateMessage(
            &winMessage);
        DispatchMessageW(
            &winMessage);
    }
    StopUdpServer();
    g_running = false;
    if (g_ffbWatchdogThread.joinable())
    {
        g_ffbWatchdogThread.join();
    }
    ReleaseFFBDevice();
    if (g_directInput != nullptr)
    {
        g_directInput->Release();
        g_directInput =
        nullptr;
    }
    return 0;
}