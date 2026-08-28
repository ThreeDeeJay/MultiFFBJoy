#include "common.h"

namespace MultiFFBJoy
{
namespace
{
struct AxisInfo
{
    DWORD offset = 0;
    DWORD type = 0;
    DWORD flags = 0;
    bool x = false;
    bool y = false;
    bool actuator = false;
};

struct AxisContext
{
    DWORD count = 0;
    bool x = false;
    bool y = false;
    std::vector<AxisInfo> axes;
};

struct EffectContext
{
    bool spring = false;
    DWORD effType = 0;
    DWORD staticParams = 0;
    DWORD dynamicParams = 0;
};

BOOL CALLBACK EnumAxes(const DIDEVICEOBJECTINSTANCEW* object, VOID* ctx)
{
    if (!object || !ctx || !(object->dwType & DIDFT_AXIS))
        return DIENUM_CONTINUE;

    auto& c = *static_cast<AxisContext*>(ctx);
    AxisInfo axis;
    axis.offset = object->dwOfs;
    axis.type = object->dwType;
    axis.flags = object->dwFlags;
    axis.x = object->dwOfs == DIJOFS_X;
    axis.y = object->dwOfs == DIJOFS_Y;
    axis.actuator = (object->dwType & DIDFT_FFACTUATOR) != 0;
    c.x |= axis.x;
    c.y |= axis.y;
    ++c.count;
    c.axes.push_back(axis);
    return DIENUM_CONTINUE;
}

BOOL CALLBACK EnumEffects(const DIEFFECTINFO* info, VOID* ctx)
{
    if (!info || !ctx)
        return DIENUM_CONTINUE;
    auto& c = *static_cast<EffectContext*>(ctx);
    if (!IsEqualGUID(info->guid, GUID_Spring))
        return DIENUM_CONTINUE;
    c.spring = true;
    c.effType = info->dwEffType;
    c.staticParams = info->dwStaticParams;
    c.dynamicParams = info->dwDynamicParams;
    return DIENUM_STOP;
}

bool QuerySpringSupport(IDirectInputDevice8W* device, EffectContext& result)
{
    result = {};
    const HRESULT hr = device->EnumEffects(EnumEffects, &result, DIEFT_CONDITION);
    if (FAILED(hr))
    {
        Logf("EnumEffects(DIEFT_CONDITION) failed: 0x%08lX",
             static_cast<unsigned long>(hr));
        return false;
    }
    return result.spring;
}

BOOL CALLBACK EnumDevices(const DIDEVICEINSTANCEW* instance, VOID*)
{
    if (!g_directInput || !instance)
        return DIENUM_STOP;

    IDirectInputDevice8W* device = nullptr;
    const HRESULT createHr = g_directInput->CreateDevice(
        instance->guidInstance, &device, nullptr);
    if (FAILED(createHr) || !device)
        return DIENUM_CONTINUE;

    DeviceCandidate candidate;
    candidate.guid = instance->guidInstance;
    candidate.name = instance->tszProductName;

    DIDEVCAPS caps{};
    caps.dwSize = sizeof(caps);
    if (SUCCEEDED(device->GetCapabilities(&caps)))
    {
        AxisContext axes;
        device->EnumObjects(EnumAxes, &axes, DIDFT_AXIS);
        candidate.axisCount = axes.count;
        candidate.hasXAxis = axes.x;
        candidate.hasYAxis = axes.y;
        candidate.forceFeedback = (caps.dwFlags & DIDC_FORCEFEEDBACK) != 0;

        for (const auto& axis : axes.axes)
            if (axis.actuator)
                candidate.ffbActuatorOffsets.push_back(axis.offset);

        if (candidate.forceFeedback)
        {
            EffectContext effects;
            if (QuerySpringSupport(device, effects))
            {
                candidate.springSupported = true;
                candidate.springEffType = effects.effType;
                candidate.springStaticParams = effects.staticParams;
                candidate.springDynamicParams = effects.dynamicParams;
            }
        }

        Logf(" %ls: axes=%lu X=%s Y=%s FFB=%s Spring=%s actuators=%zu",
             candidate.name.c_str(),
             static_cast<unsigned long>(candidate.axisCount),
             candidate.hasXAxis ? "yes" : "no",
             candidate.hasYAxis ? "yes" : "no",
             candidate.forceFeedback ? "yes" : "no",
             candidate.springSupported ? "yes" : "no",
             candidate.ffbActuatorOffsets.size());
    }

    device->Release();
    g_candidates.push_back(std::move(candidate));
    return DIENUM_CONTINUE;
}

bool ConfigureAndAcquire(IDirectInputDevice8W* device,
                         const DeviceCandidate& candidate)
{
    HRESULT hr = device->SetDataFormat(&c_dfDIJoystick2);
    if (FAILED(hr))
    {
        Logf("SetDataFormat failed for %ls: 0x%08lX",
             candidate.name.c_str(), static_cast<unsigned long>(hr));
        return false;
    }

    hr = device->SetCooperativeLevel(
        g_mainWindow, DISCL_BACKGROUND | DISCL_EXCLUSIVE);
    if (FAILED(hr))
    {
        Logf("SetCooperativeLevel failed for %ls: 0x%08lX",
             candidate.name.c_str(), static_cast<unsigned long>(hr));
        return false;
    }

    DIPROPDWORD autoCenter{};
    autoCenter.diph.dwSize = sizeof(autoCenter);
    autoCenter.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    autoCenter.diph.dwObj = 0;
    autoCenter.diph.dwHow = DIPH_DEVICE;
    autoCenter.dwData = DIPROPAUTOCENTER_OFF;
    hr = device->SetProperty(DIPROP_AUTOCENTER, &autoCenter.diph);
    if (FAILED(hr))
        Logf("Warning: could not disable hardware auto-center: 0x%08lX",
             static_cast<unsigned long>(hr));
    else
        Log("Hardware auto-center disabled.");

    hr = device->Acquire();
    if (FAILED(hr))
    {
        Logf("Acquire failed for %ls: 0x%08lX",
             candidate.name.c_str(), static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}
} // namespace

bool InitializeDirectInput(HINSTANCE instance)
{
    std::lock_guard<std::recursive_mutex> lock(g_ffbMutex);
    if (g_directInput)
        return true;

    const HRESULT hr = DirectInput8Create(
        instance, DIRECTINPUT_VERSION, IID_IDirectInput8W,
        reinterpret_cast<void**>(&g_directInput), nullptr);
    if (FAILED(hr))
    {
        Logf("DirectInput8Create failed: 0x%08lX",
             static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

void ShutdownDirectInput()
{
    std::lock_guard<std::recursive_mutex> lock(g_ffbMutex);
    ReleaseFFBDevice();
    if (g_directInput)
    {
        g_directInput->Release();
        g_directInput = nullptr;
    }
}

void ReleaseFFBDevice()
{
    std::lock_guard<std::recursive_mutex> lock(g_ffbMutex);

    if (g_springEffect)
    {
        StopSpringForRelease();
        g_springEffect->Release();
        g_springEffect = nullptr;
    }
    if (g_testConstantEffect)
    {
        StopTestConstantForce();
        g_testConstantEffect->Release();
        g_testConstantEffect = nullptr;
    }
    if (g_ffbDevice)
    {
        g_ffbDevice->Unacquire();
        g_ffbDevice->Release();
        g_ffbDevice = nullptr;
    }

    {
        std::lock_guard<std::mutex> stateLock(g_stateMutex);
        g_state.acquired = false;
        g_state.springStrength = 0.0f;
        // g_activeSpring intentionally survives device loss so the watchdog
        // can restore the active effect after a successful re-acquisition.
    }
    UpdateStatus();
}

bool SelectFirstSuitableDevice()
{
    std::lock_guard<std::recursive_mutex> lock(g_ffbMutex);
    if (!g_directInput)
        return false;

    ReleaseFFBDevice();
    g_candidates.clear();

    const HRESULT hr = g_directInput->EnumDevices(
        DI8DEVCLASS_GAMECTRL, EnumDevices, nullptr, DIEDFL_ATTACHEDONLY);
    if (FAILED(hr))
    {
        Logf("DirectInput enumeration failed: 0x%08lX",
             static_cast<unsigned long>(hr));
        return false;
    }

    Logf("Found %zu attached game-controller device(s).", g_candidates.size());

    for (const auto& candidate : g_candidates)
    {
        if (!candidate.forceFeedback || !candidate.springSupported ||
            candidate.axisCount < 2 || !candidate.hasXAxis || !candidate.hasYAxis ||
            candidate.ffbActuatorOffsets.size() < 2)
            continue;

        Logf("Attempting FFB device: %ls", candidate.name.c_str());
        IDirectInputDevice8W* device = nullptr;
        const HRESULT createHr = g_directInput->CreateDevice(
            candidate.guid, &device, nullptr);
        if (FAILED(createHr) || !device)
        {
            Logf("Could not open %ls: 0x%08lX", candidate.name.c_str(),
                 static_cast<unsigned long>(createHr));
            continue;
        }

        if (!ConfigureAndAcquire(device, candidate))
        {
            device->Release();
            continue;
        }

        g_ffbDevice = device;
        {
            std::lock_guard<std::mutex> stateLock(g_stateMutex);
            g_state.name = candidate.name;
            g_state.axisCount = candidate.axisCount;
            g_state.forceFeedback = candidate.forceFeedback;
            g_state.springSupported = candidate.springSupported;
            g_state.acquired = true;
            g_state.xAxisOffset = DIJOFS_X;
            g_state.yAxisOffset = DIJOFS_Y;
        }

        if (!CreateSpringEffect() || !CreateTestConstantForceEffect())
        {
            ReleaseFFBDevice();
            continue;
        }

        UpdateStatus();
        Logf("Selected FFB device: %ls", candidate.name.c_str());
        return true;
    }

    Log("No suitable 2-axis FFB joystick found.");
    return false;
}
} // namespace MultiFFBJoy
