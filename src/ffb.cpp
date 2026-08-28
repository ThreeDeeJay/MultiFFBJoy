#include "common.h"

namespace MultiFFBJoy
{
namespace
{
constexpr DWORD WATCHDOG_INTERVAL_MS = 100;
constexpr int WATCHDOG_FAILURES_BEFORE_REACQUIRE = 5;
constexpr DWORD REACQUIRE_RETRY_MS = 100;
constexpr int REACQUIRE_ATTEMPTS = 30;
constexpr DWORD DEVICE_SETTLE_MS = 100;
constexpr DWORD HEALTH_RETRY_MS = 50;
constexpr int HEALTH_RETRIES = 3;

bool IsExpectedDeviceError(HRESULT hr)
{
    return hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED ||
           hr == DIERR_NOTEXCLUSIVEACQUIRED || hr == DIERR_OBJECTNOTFOUND ||
           hr == static_cast<HRESULT>(0x80040203L);
}

LONG ClampFFB(LONG value)
{
    return std::clamp(value, FFB_COORD_MIN, FFB_COORD_MAX);
}

void StopEffect(IDirectInputEffect* effect, const char* label)
{
    if (!effect)
        return;
    const HRESULT hr = effect->Stop();
    if (FAILED(hr) && !IsExpectedDeviceError(hr))
        Logf("%s Stop failed: 0x%08lX", label, static_cast<unsigned long>(hr));
}

void SetSpringState(float strength, bool persistent)
{
    std::lock_guard<std::mutex> stateLock(g_stateMutex);
    g_state.springStrength = strength;
    g_state.springPersistent = persistent;
}

bool ApplySpringParameters(LONG centerX, LONG centerY,
                           LONG coefficientX, LONG coefficientY,
                           bool start)
{
    DWORD axes[2] = {DIJOFS_X, DIJOFS_Y};
    LONG directions[2] = {1, 0};
    DICONDITION conditions[2]{};

    // FFB2 quirk established experimentally:
    // condition[0] maps to logical Y and condition[1] to logical X.
    conditions[0].lOffset = centerY;
    conditions[0].lPositiveCoefficient = coefficientY;
    conditions[0].lNegativeCoefficient = coefficientY;
    conditions[1].lOffset = centerX;
    conditions[1].lPositiveCoefficient = coefficientX;
    conditions[1].lNegativeCoefficient = coefficientX;
    for (auto& condition : conditions)
    {
        condition.dwPositiveSaturation = DI_FFNOMINALMAX;
        condition.dwNegativeSaturation = DI_FFNOMINALMAX;
        condition.lDeadBand = 0;
    }

    DIEFFECT effect{};
    effect.dwSize = sizeof(effect);
    effect.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    effect.dwDuration = INFINITE;
    effect.dwGain = DI_FFNOMINALMAX;
    effect.dwTriggerButton = DIEB_NOTRIGGER;
    effect.cAxes = 2;
    effect.rgdwAxes = axes;
    effect.rglDirection = directions;
    effect.cbTypeSpecificParams = sizeof(conditions);
    effect.lpvTypeSpecificParams = conditions;

    HRESULT hr = g_springEffect->SetParameters(
        &effect, DIEP_TYPESPECIFICPARAMS | DIEP_DIRECTION);
    if (FAILED(hr))
    {
        Logf("SetParameters(Spring) failed: 0x%08lX",
             static_cast<unsigned long>(hr));
        return false;
    }

    if (start)
    {
        hr = g_springEffect->Start(1, 0);
        if (FAILED(hr))
        {
            Logf("Spring Start failed: 0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }
    }
    return true;
}

bool RestoreActiveSpring()
{
    ActiveSpringState active;
    {
        std::lock_guard<std::recursive_mutex> lock(g_ffbMutex);
        active = g_activeSpring;
    }
    if (!active.active)
        return true;
    if (active.forceField)
        return SetSpringForceField(active.field);
    return SetSpringStrength(active.strength);
}
} // namespace

bool CreateSpringEffect()
{
    std::lock_guard<std::recursive_mutex> lock(g_ffbMutex);
    if (!g_ffbDevice)
        return false;
    if (g_springEffect)
        return true;

    DWORD axes[2] = {DIJOFS_X, DIJOFS_Y};
    LONG directions[2] = {1, 0};
    DICONDITION conditions[2]{};
    for (auto& condition : conditions)
    {
        condition.dwPositiveSaturation = DI_FFNOMINALMAX;
        condition.dwNegativeSaturation = DI_FFNOMINALMAX;
    }

    DIEFFECT effect{};
    effect.dwSize = sizeof(effect);
    effect.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    effect.dwDuration = INFINITE;
    effect.dwGain = DI_FFNOMINALMAX;
    effect.dwTriggerButton = DIEB_NOTRIGGER;
    effect.cAxes = 2;
    effect.rgdwAxes = axes;
    effect.rglDirection = directions;
    effect.cbTypeSpecificParams = sizeof(conditions);
    effect.lpvTypeSpecificParams = conditions;

    const HRESULT hr = g_ffbDevice->CreateEffect(
        GUID_Spring, &effect, &g_springEffect, nullptr);
    if (FAILED(hr))
    {
        Logf("CreateEffect(GUID_Spring) failed: 0x%08lX",
             static_cast<unsigned long>(hr));
        g_springEffect = nullptr;
        return false;
    }
    Log("Spring effect created successfully.");
    return true;
}

bool CreateTestConstantForceEffect()
{
    std::lock_guard<std::recursive_mutex> lock(g_ffbMutex);
    if (!g_ffbDevice)
        return false;
    if (g_testConstantEffect)
        return true;

    DWORD axes[2] = {DIJOFS_X, DIJOFS_Y};
    LONG direction[2] = {0, 0};
    DICONSTANTFORCE force{};

    DIEFFECT effect{};
    effect.dwSize = sizeof(effect);
    effect.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    effect.dwDuration = INFINITE;
    effect.dwGain = DI_FFNOMINALMAX;
    effect.dwTriggerButton = DIEB_NOTRIGGER;
    effect.cAxes = 2;
    effect.rgdwAxes = axes;
    effect.rglDirection = direction;
    effect.cbTypeSpecificParams = sizeof(force);
    effect.lpvTypeSpecificParams = &force;

    const HRESULT hr = g_ffbDevice->CreateEffect(
        GUID_ConstantForce, &effect, &g_testConstantEffect, nullptr);
    if (FAILED(hr))
    {
        Logf("CreateEffect(GUID_ConstantForce) failed: 0x%08lX",
             static_cast<unsigned long>(hr));
        g_testConstantEffect = nullptr;
        return false;
    }
    Log("Constant-force test effect created.");
    return true;
}

bool IsFFBDeviceUsable()
{
    std::lock_guard<std::recursive_mutex> lock(g_ffbMutex);
    if (!g_ffbDevice)
        return false;
    DIJOYSTATE2 state{};
    const HRESULT hr = g_ffbDevice->GetDeviceState(sizeof(state), &state);
    if (SUCCEEDED(hr))
        return true;
    if (!IsExpectedDeviceError(hr))
        Logf("FFB device health check failed: 0x%08lX",
             static_cast<unsigned long>(hr));
    return false;
}

void StopSpringForRelease()
{
    std::lock_guard<std::recursive_mutex> lock(g_ffbMutex);
    StopEffect(g_springEffect, "Spring");
    SetSpringState(0.0f, g_activeSpring.active);
    // Do not clear g_activeSpring: it is the desired state to restore after
    // an unexpected device loss.
    UpdateStatus();
}

void StopSpring()
{
    std::lock_guard<std::recursive_mutex> lock(g_ffbMutex);
    StopEffect(g_springEffect, "Spring");
    SetSpringState(0.0f, false);
    g_activeSpring = ActiveSpringState{};
    UpdateStatus();
}

void StopTestConstantForce()
{
    std::lock_guard<std::recursive_mutex> lock(g_ffbMutex);
    StopEffect(g_testConstantEffect, "ConstantForce");
}

bool SetSpringStrength(float strength)
{
    std::lock_guard<std::recursive_mutex> lock(g_ffbMutex);
    if (!g_ffbDevice || !g_springEffect)
        return false;

    strength = std::clamp(strength, 0.0f, 1.0f);
    const LONG coefficient = static_cast<LONG>(
        std::lround(strength * static_cast<float>(DI_FFNOMINALMAX)));

    if (!ApplySpringParameters(0, 0, -coefficient, -coefficient, true))
        return false;

    g_activeSpring.active = strength > 0.0f;
    g_activeSpring.forceField = false;
    g_activeSpring.strength = strength;
    g_activeSpring.field = ForceField{};
    SetSpringState(strength, strength > 0.0f);
    StopTestConstantForce();
    UpdateStatus();
    Logf("Spring strength set to %.3f.", strength);
    return true;
}

bool SetSpringForceField(const ForceField& forceField)
{
    std::lock_guard<std::recursive_mutex> lock(g_ffbMutex);
    if (!g_ffbDevice || !g_springEffect)
        return false;

    LONG centerX = ClampFFB(forceField.centerX);
    LONG centerY = ClampFFB(forceField.centerY);

    // Straight PRND's Park and Drive are edge detents, not interior centers.
    // Keep X centered and put Y at the corresponding travel limit.
    if (_stricmp(forceField.name.c_str(), "Park") == 0)
    {
        centerX = 0;
        centerY = FFB_COORD_MIN;
    }
    else if (_stricmp(forceField.name.c_str(), "Drive") == 0)
    {
        centerX = 0;
        centerY = FFB_COORD_MAX;
    }

    const LONG coefficientX = std::clamp<LONG>(
        std::abs(forceField.powerX), 0, DI_FFNOMINALMAX);
    const LONG coefficientY = std::clamp<LONG>(
        std::abs(forceField.powerY), 0, DI_FFNOMINALMAX);

    Logf("SetSpringForceField: \"%s\" equilibrium=(%ld,%ld) coeff=(%ld,%ld)",
         forceField.name.c_str(), centerX, centerY, coefficientX, coefficientY);

    if (!ApplySpringParameters(centerX, centerY, coefficientX, coefficientY, true))
        return false;

    g_activeSpring.active = true;
    g_activeSpring.forceField = true;
    g_activeSpring.strength = 1.0f;
    g_activeSpring.field = forceField;
    SetSpringState(1.0f, true);
    StopTestConstantForce();
    UpdateStatus();
    return true;
}

bool SetTestConstantForce(LONG x, LONG y)
{
    std::lock_guard<std::recursive_mutex> lock(g_ffbMutex);
    if (!g_ffbDevice || !g_testConstantEffect)
        return false;

    x = ClampFFB(x);
    y = ClampFFB(y);
    if (x == 0 && y == 0)
    {
        StopTestConstantForce();
        return true;
    }

    // The constant-force test is an explicit manual test mode.
    StopSpring();

    DWORD axes[2] = {DIJOFS_X, DIJOFS_Y};
    LONG direction[2] = {x, y};
    const LONG magnitude = static_cast<LONG>(std::min<long long>(
        DI_FFNOMINALMAX,
        static_cast<long long>(std::sqrt(
            static_cast<double>(x) * x + static_cast<double>(y) * y))));

    DICONSTANTFORCE force{};
    force.lMagnitude = magnitude;
    DIEFFECT effect{};
    effect.dwSize = sizeof(effect);
    effect.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    effect.dwDuration = INFINITE;
    effect.dwGain = DI_FFNOMINALMAX;
    effect.dwTriggerButton = DIEB_NOTRIGGER;
    effect.cAxes = 2;
    effect.rgdwAxes = axes;
    effect.rglDirection = direction;
    effect.cbTypeSpecificParams = sizeof(force);
    effect.lpvTypeSpecificParams = &force;

    HRESULT hr = g_testConstantEffect->SetParameters(
        &effect, DIEP_DIRECTION | DIEP_TYPESPECIFICPARAMS);
    if (FAILED(hr))
    {
        Logf("SetParameters(ConstantForce) failed: 0x%08lX",
             static_cast<unsigned long>(hr));
        return false;
    }
    hr = g_testConstantEffect->Start(1, 0);
    if (FAILED(hr))
    {
        Logf("ConstantForce Start failed: 0x%08lX",
             static_cast<unsigned long>(hr));
        return false;
    }
    Logf("Constant force applied: X=%ld Y=%ld magnitude=%ld", x, y, magnitude);
    return true;
}

bool ReacquireFFBDevice()
{
    bool expected = false;
    if (!g_reacquiring.compare_exchange_strong(
            expected, true, std::memory_order_acquire, std::memory_order_relaxed))
    {
        Log("REACQUIRE ignored: another re-acquisition is already in progress.");
        return false;
    }

    struct Guard
    {
        ~Guard() { g_reacquiring.store(false, std::memory_order_release); }
    } guard;

    std::lock_guard<std::recursive_mutex> lock(g_ffbMutex);
    Log("Re-acquiring FFB device...");
    ReleaseFFBDevice();
    Sleep(DEVICE_SETTLE_MS);

    for (int attempt = 1; attempt <= REACQUIRE_ATTEMPTS && g_running; ++attempt)
    {
        Logf("FFB acquisition attempt %d/%d.", attempt, REACQUIRE_ATTEMPTS);
        if (!SelectFirstSuitableDevice())
        {
            Sleep(REACQUIRE_RETRY_MS);
            continue;
        }

        bool usable = false;
        for (int i = 0; i < 10 && g_running; ++i)
        {
            if (IsFFBDeviceUsable())
            {
                usable = true;
                break;
            }
            Sleep(HEALTH_RETRY_MS);
        }
        if (!usable)
        {
            Log("Selected FFB device did not become usable.");
            ReleaseFFBDevice();
            Sleep(REACQUIRE_RETRY_MS);
            continue;
        }

        StopEffect(g_springEffect, "Spring");
        StopEffect(g_testConstantEffect, "ConstantForce");
        SetSpringState(0.0f, g_activeSpring.active);
        UpdateStatus();

        Log("FFB device is exclusively acquired and usable.");
        if (!RestoreActiveSpring())
            Log("Warning: active spring could not be restored after acquisition.");
        else if (g_activeSpring.active)
            Log("Active spring restored after FFB re-acquisition.");

        return true;
    }

    Log("FFB re-acquisition failed after all attempts.");
    return false;
}

bool EnsureFFBDeviceReady()
{
    std::lock_guard<std::recursive_mutex> lock(g_ffbMutex);
    if (g_reacquiring.load(std::memory_order_acquire))
        return false;

    if (g_ffbDevice && IsFFBDeviceUsable())
    {
        std::lock_guard<std::mutex> stateLock(g_stateMutex);
        g_state.acquired = true;
        return true;
    }

    for (int attempt = 0; attempt < HEALTH_RETRIES; ++attempt)
    {
        if (g_reacquiring.load(std::memory_order_acquire))
            return false;
        Sleep(HEALTH_RETRY_MS);
        if (g_ffbDevice && IsFFBDeviceUsable())
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_state.acquired = true;
            return true;
        }
    }
    return ReacquireFFBDevice();
}

void StartFFBWatchdog()
{
    if (g_ffbWatchdogThread.joinable())
        return;

    try
    {
        g_ffbWatchdogThread = std::thread([]
        {
        Log("FFB watchdog thread started.");
        int failures = 0;
        while (g_running)
        {
            {
                std::lock_guard<std::recursive_mutex> lock(g_ffbMutex);
                if (g_reacquiring.load(std::memory_order_acquire) || !g_ffbDevice)
                {
                    failures = 0;
                    Sleep(WATCHDOG_INTERVAL_MS);
                    continue;
                }
            }

            if (IsFFBDeviceUsable())
            {
                failures = 0;
            }
            else if (++failures >= WATCHDOG_FAILURES_BEFORE_REACQUIRE)
            {
                Log("FFB watchdog detected sustained device loss; re-acquiring.");
                ReacquireFFBDevice();
                failures = 0;
            }
            Sleep(WATCHDOG_INTERVAL_MS);
        }
            Log("FFB watchdog thread stopped.");
        });
    }
    catch (const std::exception& error)
    {
        Logf("Failed to start FFB watchdog thread: %s", error.what());
    }
}

void StopFFBWatchdog()
{
    if (g_ffbWatchdogThread.joinable())
        g_ffbWatchdogThread.join();
}

bool ReadFFBJoystickPosition(LONG& x, LONG& y)
{
    std::lock_guard<std::recursive_mutex> lock(g_ffbMutex);
    x = y = 0;
    if (!g_ffbDevice)
        return false;

    DIJOYSTATE2 state{};
    const HRESULT hr = g_ffbDevice->GetDeviceState(sizeof(state), &state);
    if (FAILED(hr))
        return false;

    constexpr double RAW_MIN = 0.0;
    constexpr double RAW_MAX = 65535.0;
    const auto normalize = [](LONG raw) -> LONG
    {
        const double clamped = std::clamp<double>(raw, RAW_MIN, RAW_MAX);
        const double normalized = (clamped - RAW_MIN) / (RAW_MAX - RAW_MIN);
        return static_cast<LONG>(std::lround(
            FFB_COORD_MIN + normalized * (FFB_COORD_MAX - FFB_COORD_MIN)));
    };

    x = normalize(static_cast<LONG>(state.lX));
    y = normalize(static_cast<LONG>(state.lY));
    return true;
}
} // namespace MultiFFBJoy
