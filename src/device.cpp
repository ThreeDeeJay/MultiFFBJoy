#include "common.h"

namespace MultiFFBJoy
{

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

static BOOL CALLBACK EnumerateAxesCallback(
    const DIDEVICEOBJECTINSTANCEW* object,
    VOID* contextPointer)
{
    if (object == nullptr || contextPointer == nullptr)
        return DIENUM_CONTINUE;

    auto* context =
        static_cast<AxisEnumerationContext*>(contextPointer);

    if ((object->dwType & DIDFT_AXIS) == 0)
        return DIENUM_CONTINUE;

    AxisEnumerationContext::Axis axis;
    axis.offset = object->dwOfs;
    axis.type = object->dwType;
    axis.flags = object->dwFlags;
    axis.isXAxis = object->dwOfs == DIJOFS_X;
    axis.isYAxis = object->dwOfs == DIJOFS_Y;
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

struct EffectEnumerationContext
{
    bool springSupported = false;
    DWORD springEffType = 0;
    DWORD springStaticParams = 0;
    DWORD springDynamicParams = 0;
};

static BOOL CALLBACK EnumerateEffectsCallback(
    const DIEFFECTINFO* effectInfo,
    VOID* contextPointer)
{
    if (effectInfo == nullptr || contextPointer == nullptr)
        return DIENUM_CONTINUE;

    auto* context =
        static_cast<EffectEnumerationContext*>(contextPointer);

    if (IsEqualGUID(effectInfo->guid, GUID_Spring))
    {
        context->springSupported = true;
        context->springEffType = effectInfo->dwEffType;
        context->springStaticParams = effectInfo->dwStaticParams;
        context->springDynamicParams = effectInfo->dwDynamicParams;

        Logf(
            " GUID_Spring found: effType=0x%08lX "
            "static=0x%08lX dynamic=0x%08lX",
            static_cast<unsigned long>(effectInfo->dwEffType),
            static_cast<unsigned long>(effectInfo->dwStaticParams),
            static_cast<unsigned long>(effectInfo->dwDynamicParams));

        return DIENUM_STOP;
    }

    return DIENUM_CONTINUE;
}

static bool QuerySpringSupport(
    IDirectInputDevice8W* device,
    EffectEnumerationContext& result)
{
    result = EffectEnumerationContext{};

    const HRESULT hr = device->EnumEffects(
        EnumerateEffectsCallback,
        &result,
        DIEFT_CONDITION);

    if (FAILED(hr))
    {
        Logf(
            "EnumEffects(DIEFT_CONDITION) failed: 0x%08lX",
            static_cast<unsigned long>(hr));
        return false;
    }

    return result.springSupported;
}

static BOOL CALLBACK EnumerateDevicesCallback(
    const DIDEVICEINSTANCEW* instance,
    VOID*)
{
    if (g_directInput == nullptr || instance == nullptr)
        return DIENUM_STOP;

    IDirectInputDevice8W* device = nullptr;

    HRESULT hr = g_directInput->CreateDevice(
        instance->guidInstance,
        &device,
        nullptr);

    if (FAILED(hr) || device == nullptr)
        return DIENUM_CONTINUE;

    DIDEVCAPS capabilities{};
    capabilities.dwSize = sizeof(capabilities);

    hr = device->GetCapabilities(&capabilities);

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
                ffbActuatorOffsets.push_back(axis.offset);
        }

        Logf(
            " %ls: %lu axis object(s)",
            instance->tszProductName,
            axes.axisCount);

        for (size_t axisIndex = 0;
             axisIndex < axes.axes.size();
             ++axisIndex)
        {
            const auto& axis = axes.axes[axisIndex];

            Logf(
                " axis[%zu]: offset=0x%lX type=0x%08lX "
                "flags=0x%08lX X=%s Y=%s FFBActuator=%s",
                axisIndex,
                static_cast<unsigned long>(axis.offset),
                static_cast<unsigned long>(axis.type),
                static_cast<unsigned long>(axis.flags),
                axis.isXAxis ? "yes" : "no",
                axis.isYAxis ? "yes" : "no",
                axis.isFFBActuator ? "YES" : "no");
        }

        DeviceCandidate candidate;
        candidate.guid = instance->guidInstance;
        candidate.name = instance->tszProductName;
        candidate.axisCount = axes.axisCount;
        candidate.forceFeedback =
            (capabilities.dwFlags & DIDC_FORCEFEEDBACK) != 0;
        candidate.hasXAxis = axes.hasXAxis;
        candidate.hasYAxis = axes.hasYAxis;
        candidate.ffbActuatorOffsets = ffbActuatorOffsets;

        if (candidate.forceFeedback)
        {
            EffectEnumerationContext effects;

            if (QuerySpringSupport(device, effects))
            {
                candidate.springSupported = true;
                candidate.springEffType = effects.springEffType;
                candidate.springStaticParams =
                    effects.springStaticParams;
                candidate.springDynamicParams =
                    effects.springDynamicParams;
            }
        }

        g_candidates.push_back(candidate);
    }

    device->Release();
    return DIENUM_CONTINUE;
}

bool InitializeDirectInput(HINSTANCE instance)
{
    HRESULT hr = DirectInput8Create(
        instance,
        DIRECTINPUT_VERSION,
        IID_IDirectInput8W,
        reinterpret_cast<void**>(&g_directInput),
        nullptr);

    if (FAILED(hr))
    {
        Logf(
            "DirectInput8Create failed: 0x%08lX",
            static_cast<unsigned long>(hr));
        return false;
    }

    Log("DirectInput initialized.");
    return true;
}

void ShutdownDirectInput()
{
    ReleaseFFBDevice();

    if (g_directInput != nullptr)
    {
        g_directInput->Release();
        g_directInput = nullptr;
    }
}

void ReleaseFFBDevice()
{
    StopSpringForRelease();
    StopTestConstantForce();

    if (g_springEffect != nullptr)
    {
        g_springEffect->Release();
        g_springEffect = nullptr;
    }

    if (g_testConstantEffect != nullptr)
    {
        g_testConstantEffect->Release();
        g_testConstantEffect = nullptr;
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
        // ReacquireFFBDevice() needs it.
    }

    UpdateStatus();
}

bool SelectFirstSuitableDevice()
{
    if (g_directInput == nullptr)
        return false;

    // This function MUST NOT call itself.
    // It also MUST NOT start the watchdog.
    //
    // The previous version accidentally contained:
    //
    //     if (!SelectFirstSuitableDevice()) ...
    //
    // inside this function, which recursively re-entered device
    // selection forever and repeatedly created/stopped FFB effects.

    ReleaseFFBDevice();
    g_candidates.clear();

    const HRESULT hr = g_directInput->EnumDevices(
        DI8DEVCLASS_GAMECTRL,
        EnumerateDevicesCallback,
        nullptr,
        DIEDFL_ATTACHEDONLY);

    if (FAILED(hr))
    {
        Logf(
            "DirectInput enumeration failed: 0x%08lX",
            static_cast<unsigned long>(hr));
        return false;
    }

    Logf(
        "Found %zu attached game-controller device(s).",
        g_candidates.size());

    for (size_t i = 0; i < g_candidates.size(); ++i)
    {
        const auto& candidate = g_candidates[i];

        Logf(
            "[%zu] %ls | axes=%lu | X=%s | Y=%s | FFB=%s | Spring=%s",
            i,
            candidate.name.c_str(),
            candidate.axisCount,
            candidate.hasXAxis ? "yes" : "no",
            candidate.hasYAxis ? "yes" : "no",
            candidate.forceFeedback ? "yes" : "no",
            candidate.springSupported ? "yes" : "no");
    }

    for (const auto& candidate : g_candidates)
    {
        if (!candidate.forceFeedback)
            continue;

        if (candidate.axisCount < 2)
            continue;

        if (!candidate.hasXAxis || !candidate.hasYAxis)
            continue;

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

        IDirectInputDevice8W* device = nullptr;

        HRESULT openResult = g_directInput->CreateDevice(
            candidate.guid,
            &device,
            nullptr);

        if (FAILED(openResult) || device == nullptr)
        {
            Logf(
                "Could not open %ls: 0x%08lX",
                candidate.name.c_str(),
                static_cast<unsigned long>(openResult));
            continue;
        }

        openResult = device->SetDataFormat(&c_dfDIJoystick2);

        if (FAILED(openResult))
        {
            Logf(
                "SetDataFormat failed for %ls: 0x%08lX",
                candidate.name.c_str(),
                static_cast<unsigned long>(openResult));
            device->Release();
            continue;
        }

        openResult = device->SetCooperativeLevel(
            g_mainWindow,
            DISCL_BACKGROUND | DISCL_EXCLUSIVE);

        if (FAILED(openResult))
        {
            Logf(
                "SetCooperativeLevel failed for %ls: 0x%08lX",
                candidate.name.c_str(),
                static_cast<unsigned long>(openResult));
            device->Release();
            continue;
        }

        DIPROPDWORD autoCenter{};
        autoCenter.diph.dwSize = sizeof(DIPROPDWORD);
        autoCenter.diph.dwHeaderSize = sizeof(DIPROPHEADER);
        autoCenter.diph.dwObj = 0;
        autoCenter.diph.dwHow = DIPH_DEVICE;
        autoCenter.dwData = DIPROPAUTOCENTER_OFF;

        const HRESULT autoCenterResult = device->SetProperty(
            DIPROP_AUTOCENTER,
            &autoCenter.diph);

        if (FAILED(autoCenterResult))
        {
            Logf(
                "Warning: could not disable hardware auto-center "
                "before Acquire: 0x%08lX",
                static_cast<unsigned long>(autoCenterResult));
        }
        else
        {
            Log("Hardware auto-center disabled.");
        }

        openResult = device->Acquire();

        if (FAILED(openResult))
        {
            Logf(
                "Acquire failed for %ls: 0x%08lX",
                candidate.name.c_str(),
                static_cast<unsigned long>(openResult));
            device->Release();
            continue;
        }

        g_ffbDevice = device;

        {
            std::lock_guard<std::mutex> lock(g_stateMutex);

            g_state.name = candidate.name;
            g_state.axisCount = candidate.axisCount;
            g_state.forceFeedback = candidate.forceFeedback;
            g_state.springSupported = candidate.springSupported;
            g_state.acquired = true;
            g_state.xAxisOffset = DIJOFS_X;
            g_state.yAxisOffset = DIJOFS_Y;
        }

        Logf(
            "Selected FFB device: %ls",
            candidate.name.c_str());

        if (!CreateSpringEffect())
        {
            Log("Failed to create spring effect.");
            ReleaseFFBDevice();
            continue;
        }

        if (!CreateTestConstantForceEffect())
        {
            Log("Failed to create constant-force test effect.");
            ReleaseFFBDevice();
            continue;
        }

        UpdateStatus();

        Log("FFB joystick initialized successfully.");
        return true;
    }

    Log("No suitable 2-axis FFB joystick found.");
    return false;
}

} // namespace MultiFFBJoy
