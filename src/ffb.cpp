#include "common.h"
namespace MultiFFBJoy
{
    namespace
    {
        // The SideWinder FFB2 exposes the two condition slots in the
        // opposite order from the logical X/Y coordinates we read from
        // DIJOYSTATE2.  The old code put logical Y into condition[1],
        // which is why a vertical PRND offset produced a horizontal pull.
        // Keep the logical FFShifter coordinates as X/Y and translate them
        // to the FFB2 condition slots here.
        constexpr bool SWAP_FFB2_CONDITION_AXES = true;
        void BuildSpringConditions(
            LONG logicalCenterX,
            LONG logicalCenterY,
            LONG coefficientX,
            LONG coefficientY,
            DICONDITION (&conditions)[2])
        {
            const LONG logicalCoefficientX =
                std::clamp<LONG>(
                    std::abs(coefficientX),
                    0,
                    DI_FFNOMINALMAX);
            const LONG logicalCoefficientY =
                std::clamp<LONG>(
                    std::abs(coefficientY),
                    0,
                    DI_FFNOMINALMAX);
            for (auto& condition : conditions)
            {
                condition = DICONDITION{};
                condition.dwPositiveSaturation = DI_FFNOMINALMAX;
                condition.dwNegativeSaturation = DI_FFNOMINALMAX;
                condition.lDeadBand = 0;
            }
            if (SWAP_FFB2_CONDITION_AXES)
            {
                conditions[0].lOffset = logicalCenterY;
                conditions[0].lPositiveCoefficient = logicalCoefficientY;
                conditions[0].lNegativeCoefficient = logicalCoefficientY;
                conditions[1].lOffset = logicalCenterX;
                conditions[1].lPositiveCoefficient = logicalCoefficientX;
                conditions[1].lNegativeCoefficient = logicalCoefficientX;
            }
            else
            {
                conditions[0].lOffset = logicalCenterX;
                conditions[0].lPositiveCoefficient = logicalCoefficientX;
                conditions[0].lNegativeCoefficient = logicalCoefficientX;
                conditions[1].lOffset = logicalCenterY;
                conditions[1].lPositiveCoefficient = logicalCoefficientY;
                conditions[1].lNegativeCoefficient = logicalCoefficientY;
            }
        }
    }
    bool CreateSpringEffect()
    {
        if (g_ffbDevice == nullptr)
            return false;
        DWORD axes[2] = { DIJOFS_X, DIJOFS_Y };
        LONG directions[2] = { 1, 0 };
        DICONDITION conditions[2]{};
        for (auto& condition : conditions)
        {
            condition.lOffset = 0;
            condition.lPositiveCoefficient = 0;
            condition.lNegativeCoefficient = 0;
            condition.dwPositiveSaturation = DI_FFNOMINALMAX;
            condition.dwNegativeSaturation = DI_FFNOMINALMAX;
            condition.lDeadBand = 0;
        }
        DIEFFECT effect{};
        effect.dwSize = sizeof(DIEFFECT);
        effect.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
        effect.dwDuration = INFINITE;
        effect.dwSamplePeriod = 0;
        effect.dwGain = DI_FFNOMINALMAX;
        effect.dwTriggerButton = DIEB_NOTRIGGER;
        effect.dwTriggerRepeatInterval = 0;
        effect.cAxes = 2;
        effect.rgdwAxes = axes;
        effect.rglDirection = directions;
        effect.cbTypeSpecificParams = sizeof(conditions);
        effect.lpvTypeSpecificParams = conditions;
        HRESULT hr = g_ffbDevice->CreateEffect(
            GUID_Spring,
            &effect,
            &g_springEffect,
            nullptr);
        if (FAILED(hr))
        {
            Logf(
                "CreateEffect(GUID_Spring) failed: HRESULT=0x%08lX",
                static_cast<unsigned long>(hr));
            g_springEffect = nullptr;
            return false;
        }
        Log("Spring effect created successfully.");
        return true;
    }
    bool CreateTestConstantForceEffect()
    {
        if (g_ffbDevice == nullptr)
            return false;
        DWORD axes[2] = { DIJOFS_X, DIJOFS_Y };
        LONG directions[2] = { 0, 0 };
        DICONSTANTFORCE constantForce{};
        DIEFFECT effect{};
        effect.dwSize = sizeof(DIEFFECT);
        effect.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
        effect.dwDuration = INFINITE;
        effect.dwSamplePeriod = 0;
        effect.dwGain = DI_FFNOMINALMAX;
        effect.dwTriggerButton = DIEB_NOTRIGGER;
        effect.dwTriggerRepeatInterval = 0;
        effect.cAxes = 2;
        effect.rgdwAxes = axes;
        effect.rglDirection = directions;
        effect.cbTypeSpecificParams = sizeof(DICONSTANTFORCE);
        effect.lpvTypeSpecificParams = &constantForce;
        HRESULT hr = g_ffbDevice->CreateEffect(
            GUID_ConstantForce,
            &effect,
            &g_testConstantEffect,
            nullptr);
        if (FAILED(hr))
        {
            Logf(
                "CreateEffect(GUID_ConstantForce) failed: 0x%08lX",
                static_cast<unsigned long>(hr));
            g_testConstantEffect = nullptr;
            return false;
        }
        Log("Constant-force test effect created.");
        return true;
    }
    bool IsFFBDeviceUsable()
    {
        if (g_ffbDevice == nullptr)
            return false;
        DIJOYSTATE2 state{};
        const HRESULT hr = g_ffbDevice->GetDeviceState(
            sizeof(DIJOYSTATE2),
            &state);
        if (SUCCEEDED(hr))
            return true;
        if (hr == DIERR_INPUTLOST ||
            hr == DIERR_NOTACQUIRED ||
            hr == DIERR_NOTEXCLUSIVEACQUIRED)
        {
            return false;
        }
        Logf(
            "FFB device health check failed unexpectedly: 0x%08lX",
            static_cast<unsigned long>(hr));
        return false;
    }
    void StopSpringForRelease()
    {
        if (g_springEffect == nullptr)
            return;
        const HRESULT hr = g_springEffect->Stop();
        if (FAILED(hr) &&
            hr != DIERR_INPUTLOST &&
            hr != DIERR_NOTACQUIRED &&
            hr != DIERR_NOTEXCLUSIVEACQUIRED &&
            hr != static_cast<HRESULT>(0x80040203L))
        {
            Logf(
                "Spring Stop failed: 0x%08lX",
                static_cast<unsigned long>(hr));
        }
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_state.springStrength = 0.0f;
        }
        UpdateStatus();
    }
    void StopSpring()
    {
        if (g_springEffect != nullptr)
        {
            const HRESULT hr = g_springEffect->Stop();
            if (FAILED(hr) &&
                hr != DIERR_INPUTLOST &&
                hr != DIERR_NOTACQUIRED &&
                hr != DIERR_NOTEXCLUSIVEACQUIRED &&
                hr != DIERR_OBJECTNOTFOUND &&
                hr != static_cast<HRESULT>(0x80040203L))
            {
                Logf(
                    "Spring Stop failed: 0x%08lX",
                    static_cast<unsigned long>(hr));
            }
        }
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_state.springStrength = 0.0f;
            g_state.springPersistent = false;
        }
        UpdateStatus();
    }
    void StopTestConstantForce()
    {
        if (g_testConstantEffect == nullptr)
            return;
        const HRESULT hr = g_testConstantEffect->Stop();
        if (FAILED(hr) &&
            hr != DIERR_INPUTLOST &&
            hr != DIERR_NOTACQUIRED &&
            hr != DIERR_NOTEXCLUSIVEACQUIRED)
        {
            Logf(
                "ConstantForce Stop failed: 0x%08lX",
                static_cast<unsigned long>(hr));
        }
    }
    bool SetSpringStrength(float strength)
    {
        if (g_ffbDevice == nullptr || g_springEffect == nullptr)
        {
            Log("SetSpringStrength: FFB device/effect unavailable.");
            return false;
        }
        strength = std::clamp(strength, 0.0f, 1.0f);
        const LONG coefficient = static_cast<LONG>(
            std::lround(
                strength * static_cast<float>(DI_FFNOMINALMAX)));
        DWORD axes[2] = { DIJOFS_X, DIJOFS_Y };
        LONG directions[2] = { 1, 0 };
        DICONDITION conditions[2]{};
        BuildSpringConditions(
            0,
            0,
            coefficient,
            coefficient,
            conditions);
        DIEFFECT effect{};
        effect.dwSize = sizeof(DIEFFECT);
        effect.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
        effect.dwDuration = INFINITE;
        effect.dwSamplePeriod = 0;
        effect.dwGain = DI_FFNOMINALMAX;
        effect.dwTriggerButton = DIEB_NOTRIGGER;
        effect.dwTriggerRepeatInterval = 0;
        effect.cAxes = 2;
        effect.rgdwAxes = axes;
        effect.rglDirection = directions;
        effect.cbTypeSpecificParams = sizeof(conditions);
        effect.lpvTypeSpecificParams = conditions;
        HRESULT hr = g_springEffect->SetParameters(
            &effect,
            DIEP_TYPESPECIFICPARAMS | DIEP_DIRECTION);
        if (FAILED(hr))
        {
            Logf(
                "SetParameters(Spring) failed: 0x%08lX",
                static_cast<unsigned long>(hr));
            return false;
        }
        hr = g_springEffect->Start(1, 0);
        if (FAILED(hr))
        {
            Logf(
                "Spring Start failed: 0x%08lX",
                static_cast<unsigned long>(hr));
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_state.springStrength = strength;
            g_state.springPersistent = (strength > 0.0f);
        }
        UpdateStatus();
        Logf(
            "Spring strength set to %.3f and effect started.",
            strength);
        return true;
    }
    bool SetSpringForceField(const ForceField& forceField)
    {
        if (g_ffbDevice == nullptr || g_springEffect == nullptr)
        {
            Log("SetSpringForceField: FFB device/effect unavailable.");
            return false;
        }
        const LONG centerX = std::clamp<LONG>(
            forceField.centerX,
            -DI_FFNOMINALMAX,
            DI_FFNOMINALMAX);
        const LONG centerY = std::clamp<LONG>(
            forceField.centerY,
            -DI_FFNOMINALMAX,
            DI_FFNOMINALMAX);
        const LONG coefficientX = std::clamp<LONG>(
            std::abs(forceField.powerX),
            0,
            DI_FFNOMINALMAX);
        const LONG coefficientY = std::clamp<LONG>(
            std::abs(forceField.powerY),
            0,
            DI_FFNOMINALMAX);
        Logf(
            "SetSpringForceField ENTER: \"%s\" logical equilibrium=(%ld,%ld) power=(%ld,%ld)",
            forceField.name.c_str(),
            centerX,
            centerY,
            forceField.powerX,
            forceField.powerY);
        Logf(
            "Spring mapping \"%s\": FFB2 condition[0]=logical Y, condition[1]=logical X; coeff=(%ld,%ld)",
            forceField.name.c_str(),
            coefficientX,
            coefficientY);
        DWORD axes[2] = { DIJOFS_X, DIJOFS_Y };
        LONG directions[2] = { 1, 0 };
        DICONDITION conditions[2]{};
        BuildSpringConditions(
            centerX,
            centerY,
            coefficientX,
            coefficientY,
            conditions);
        Logf(
            "  condition[0]: offset=%ld +coeff=%ld -coeff=%ld",
            conditions[0].lOffset,
            conditions[0].lPositiveCoefficient,
            conditions[0].lNegativeCoefficient);
        Logf(
            "  condition[1]: offset=%ld +coeff=%ld -coeff=%ld",
            conditions[1].lOffset,
            conditions[1].lPositiveCoefficient,
            conditions[1].lNegativeCoefficient);
        DIEFFECT effect{};
        effect.dwSize = sizeof(DIEFFECT);
        effect.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
        effect.dwDuration = INFINITE;
        effect.dwSamplePeriod = 0;
        effect.dwGain = DI_FFNOMINALMAX;
        effect.dwTriggerButton = DIEB_NOTRIGGER;
        effect.dwTriggerRepeatInterval = 0;
        effect.cAxes = 2;
        effect.rgdwAxes = axes;
        effect.rglDirection = directions;
        effect.cbTypeSpecificParams = sizeof(conditions);
        effect.lpvTypeSpecificParams = conditions;
        HRESULT hr = g_springEffect->SetParameters(
            &effect,
            DIEP_TYPESPECIFICPARAMS | DIEP_DIRECTION);
        Logf(
            "SetSpringForceField SetParameters: HRESULT=0x%08lX",
            static_cast<unsigned long>(hr));
        if (FAILED(hr))
            return false;
        hr = g_springEffect->Start(1, 0);
        Logf(
            "SetSpringForceField Start: HRESULT=0x%08lX",
            static_cast<unsigned long>(hr));
        if (FAILED(hr))
            return false;
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_state.springStrength = 1.0f;
            g_state.springPersistent = true;
        }
        UpdateStatus();
        return true;
    }
    bool SetTestConstantForce(LONG x, LONG y)
    {
        if (g_ffbDevice == nullptr || g_testConstantEffect == nullptr)
        {
            Log("SetTestConstantForce: FFB device/effect unavailable.");
            return false;
        }
        x = std::clamp<LONG>(x, -DI_FFNOMINALMAX, DI_FFNOMINALMAX);
        y = std::clamp<LONG>(y, -DI_FFNOMINALMAX, DI_FFNOMINALMAX);
        if (x == 0 && y == 0)
        {
            StopTestConstantForce();
            return true;
        }
        DWORD axes[2] = { DIJOFS_X, DIJOFS_Y };
        LONG direction[2] = { x, y };
        const LONG magnitude = static_cast<LONG>(
            std::min(
                static_cast<long long>(DI_FFNOMINALMAX),
                static_cast<long long>(
                    std::sqrt(
                        static_cast<double>(x) * x +
                        static_cast<double>(y) * y))));
        DICONSTANTFORCE constantForce{};
        constantForce.lMagnitude = magnitude;
        DIEFFECT effect{};
        effect.dwSize = sizeof(DIEFFECT);
        effect.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
        effect.dwDuration = INFINITE;
        effect.dwSamplePeriod = 0;
        effect.dwGain = DI_FFNOMINALMAX;
        effect.dwTriggerButton = DIEB_NOTRIGGER;
        effect.dwTriggerRepeatInterval = 0;
        effect.cAxes = 2;
        effect.rgdwAxes = axes;
        effect.rglDirection = direction;
        effect.cbTypeSpecificParams = sizeof(DICONSTANTFORCE);
        effect.lpvTypeSpecificParams = &constantForce;
        HRESULT hr = g_testConstantEffect->SetParameters(
            &effect,
            DIEP_DIRECTION | DIEP_TYPESPECIFICPARAMS);
        if (FAILED(hr))
        {
            Logf(
                "SetParameters(ConstantForce) failed: 0x%08lX",
                static_cast<unsigned long>(hr));
            return false;
        }
        hr = g_testConstantEffect->Start(1, 0);
        if (FAILED(hr))
        {
            Logf(
                "ConstantForce Start failed: 0x%08lX",
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
    void StartFFBWatchdog()
    {
        if (g_ffbWatchdogThread.joinable())
            return;
        g_ffbWatchdogThread = std::thread([]()
        {
            Log("FFB watchdog thread started.");
            constexpr int REQUIRED_FAILURES = 5;
            constexpr DWORD CHECK_INTERVAL_MS = 100;
            constexpr DWORD FAILURE_RETRY_INTERVAL_MS = 100;
            int consecutiveFailures = 0;
            while (g_running)
            {
                if (g_reacquiring.load(std::memory_order_acquire))
                {
                    consecutiveFailures = 0;
                    Sleep(CHECK_INTERVAL_MS);
                    continue;
                }
                if (g_ffbDevice == nullptr)
                {
                    consecutiveFailures = 0;
                    Sleep(CHECK_INTERVAL_MS);
                    continue;
                }
                if (IsFFBDeviceUsable())
                {
                    consecutiveFailures = 0;
                }
                else
                {
                    ++consecutiveFailures;
                    Logf(
                        "FFB watchdog health check failed "
                        "(%d/%d).",
                        consecutiveFailures,
                        REQUIRED_FAILURES);
                    if (consecutiveFailures >= REQUIRED_FAILURES)
                    {
                        if (g_reacquiring.load(std::memory_order_acquire))
                        {
                            consecutiveFailures = 0;
                            Sleep(FAILURE_RETRY_INTERVAL_MS);
                            continue;
                        }
                        Log(
                            "FFB watchdog detected sustained "
                            "device loss; starting re-acquisition.");
                        if (ReacquireFFBDevice())
                        {
                            Log(
                                "FFB watchdog re-acquisition "
                                "completed successfully.");
                        }
                        else if (g_running)
                        {
                            Log(
                                "FFB watchdog re-acquisition failed.");
                        }
                        consecutiveFailures = 0;
                        Sleep(FAILURE_RETRY_INTERVAL_MS);
                        continue;
                    }
                }
                Sleep(CHECK_INTERVAL_MS);
            }
            Log("FFB watchdog thread stopped.");
        });
    }
    void StopFFBWatchdog()
    {
        if (g_ffbWatchdogThread.joinable())
        {
            g_ffbWatchdogThread.join();
        }
    }
    bool ReacquireFFBDevice()
    {
        bool expected = false;
        if (!g_reacquiring.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acquire,
            std::memory_order_relaxed))
        {
            Log(
                "REACQUIRE ignored: another re-acquisition "
                "is already in progress.");
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
        } guard;
        Log("Re-acquiring FFB device...");
        ReleaseFFBDevice();
        Sleep(100);
        constexpr int MAX_ATTEMPTS = 30;
        constexpr DWORD RETRY_DELAY_MS = 100;
        for (int attempt = 1;
            attempt <= MAX_ATTEMPTS && g_running;
            ++attempt)
        {
            Logf(
                "FFB acquisition attempt %d/%d.",
                attempt,
                MAX_ATTEMPTS);
            if (!SelectFirstSuitableDevice())
            {
                Sleep(RETRY_DELAY_MS);
                continue;
            }
            if (g_ffbDevice == nullptr)
            {
                Log(
                    "Device selection reported success but "
                    "FFB device is null.");
                Sleep(RETRY_DELAY_MS);
                continue;
            }
            bool usable = false;
            for (int waitAttempt = 0;
                waitAttempt < 10 && g_running;
                ++waitAttempt)
            {
                if (IsFFBDeviceUsable())
                {
                    usable = true;
                    break;
                }
                Sleep(50);
            }
            if (!usable)
            {
                Log(
                    "FFB device did not become usable after "
                    "successful selection.");
                ReleaseFFBDevice();
                Sleep(RETRY_DELAY_MS);
                continue;
            }
            Log(
                "FFB device is exclusively acquired and usable.");
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                g_state.acquired = true;
            }
            UpdateStatus();
            Log(
                "FFB device successfully reinitialized.");
            Log(
                "FFB device is exclusively acquired and usable.");
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                g_state.acquired = true;
            }
            UpdateStatus();
            Log("FFB device successfully reinitialized.");
// Do not automatically apply a center spring here.
// Startup/reacquisition should leave the joystick limp unless
// an active forcefield needs to be restored.
            if (g_springEffect != nullptr)
            {
                HRESULT stopHr = g_springEffect->Stop();
                if (FAILED(stopHr) &&
                    stopHr != DIERR_INPUTLOST &&
                    stopHr != DIERR_NOTACQUIRED &&
                    stopHr != DIERR_NOTEXCLUSIVEACQUIRED &&
                    stopHr != DIERR_OBJECTNOTFOUND &&
                    stopHr != static_cast<HRESULT>(0x80040203L))
                {
                    Logf(
                        "Spring Stop after acquisition failed: 0x%08lX",
                        static_cast<unsigned long>(stopHr));
                }
            }
            if (g_testConstantEffect != nullptr)
            {
                HRESULT stopHr = g_testConstantEffect->Stop();
                if (FAILED(stopHr) &&
                    stopHr != DIERR_INPUTLOST &&
                    stopHr != DIERR_NOTACQUIRED &&
                    stopHr != DIERR_NOTEXCLUSIVEACQUIRED &&
                    stopHr != DIERR_OBJECTNOTFOUND)
                {
                    Logf(
                        "Constant-force Stop after acquisition failed: 0x%08lX",
                        static_cast<unsigned long>(stopHr));
                }
            }
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                g_state.springStrength = 0.0f;
                g_state.springPersistent = false;
            }
            UpdateStatus();
            return true;
        }
        Log(
            "FFB re-acquisition failed after all attempts.");
        return false;
    }
    bool EnsureFFBDeviceReady()
    {
        if (g_reacquiring.load(std::memory_order_acquire))
        {
            Log(
                "EnsureFFBDeviceReady: re-acquisition already in progress.");
            return false;
        }
        if (g_ffbDevice == nullptr)
        {
            Log(
                "FFB device is not initialized; attempting reacquisition.");
            return ReacquireFFBDevice();
        }
        if (IsFFBDeviceUsable())
        {
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                g_state.acquired = true;
            }
            return true;
        }
/*
* A single failed health check is not enough to immediately
* destroy the current device. The watchdog is responsible for
* detecting sustained device loss.
*
* For an explicit CENTER request, however, we need a usable
* device immediately. Give DirectInput a brief opportunity to
* recover before performing the expensive full re-acquisition.
*/
        constexpr int RETRY_COUNT = 3;
        constexpr DWORD RETRY_DELAY_MS = 50;
        for (int attempt = 1;
            attempt <= RETRY_COUNT;
            ++attempt)
        {
            if (g_reacquiring.load(std::memory_order_acquire))
            {
                return false;
            }
            Sleep(RETRY_DELAY_MS);
            if (IsFFBDeviceUsable())
            {
                {
                    std::lock_guard<std::mutex> lock(g_stateMutex);
                    g_state.acquired = true;
                }
                Logf(
                    "FFB device became usable after health-check "
                    "retry %d/%d.",
                    attempt,
                    RETRY_COUNT);
                return true;
            }
        }
        Log(
            "Existing FFB device remains unusable; "
            "attempting reacquisition.");
        return ReacquireFFBDevice();
    }
    bool ReadFFBJoystickPosition(
        LONG& x,
        LONG& y)
    {
        x = 0;
        y = 0;
        if (g_ffbDevice == nullptr)
            return false;
        DIJOYSTATE2 state{};
        HRESULT hr =
        g_ffbDevice->GetDeviceState(
            sizeof(DIJOYSTATE2),
            &state);
        if (FAILED(hr))
        {
            if (hr == DIERR_INPUTLOST ||
                hr == DIERR_NOTACQUIRED ||
                hr == DIERR_NOTEXCLUSIVEACQUIRED)
            {
/*
* Do not perform a full device reacquisition
* from the preset-monitor thread.
*
* The watchdog owns device recovery.
*/
                return false;
            }
            return false;
        }
/*
* DirectInput joystick values are normally:
*
*     0 .. 65535
*
* Convert explicitly to the .fff coordinate system:
*
*     -10000 .. +10000
*
* This makes zone testing independent of the
* device's physical raw range.
*/
        const LONG rawX =
        static_cast<LONG>(state.lX);
        const LONG rawY =
        static_cast<LONG>(state.lY);
        x =
        static_cast<LONG>(
            std::lround(
                (static_cast<double>(rawX) -
                    32767.5) *
                20000.0 /
                65535.0));
        y =
        static_cast<LONG>(
            std::lround(
                (static_cast<double>(rawY) -
                    32767.5) *
                20000.0 /
                65535.0));
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
        return true;
    }
} // namespace MultiFFBJoy
