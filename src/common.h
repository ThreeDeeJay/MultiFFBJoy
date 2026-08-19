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
#include <fstream>
#include <mutex>
#include <sstream>
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
    /*
     * Runtime state for the currently selected DirectInput device.
     */
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
        /*
         * Stage 1 preset/test information.
         *
         * These are informational/runtime values only.
         * The actual preset data is stored in g_loadedPreset.
         */
        std::string presetName;
        std::string forceFieldName;
        std::string forceFieldAction;
        std::chrono::steady_clock::time_point lastCommand =
        std::chrono::steady_clock::now();
    };
    extern DeviceState g_state;
    /*
     * Candidate DirectInput device discovered during enumeration.
     */
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
    /*
     * ----------------------------------------------------------------------
     * FFShifter FFB preset representation
     * ----------------------------------------------------------------------
     *
     * This intentionally represents the information MultiFFBJoy actually
     * needs from an FFShifter .fff file.
     *
     * KEY*, SEQUENTIAL*, and other keyboard-emulation metadata are not
     * represented here because MultiFFBJoy will eventually report the
     * semantic action directly to BeamNG rather than emulating a keyboard
     * key.
     */
    struct ForceField
    {
        /*
         * Original:
         *
         *     FORCEFIELD NAME="Parking"
         *
         * The name is retained exactly as supplied by the preset.
         */
        std::string name;
        /*
         * FORCEFIELD TYPE
         *
         * 0 = constant
         * 1 = spring
         */
        int type = 0;
        /*
         * FORCEFIELD SHAPE TYPE
         *
         * Currently retained for compatibility but Stage 1 uses the
         * polygon represented by the vertices.
         */
        int shapeType = 0;
        /*
         * FORCE TYPE
         *
         * 0 = constant
         * 1 = spring
         */
        int forceType = 0;
        /*
         * FORCEFIELD CENTER X/Y/Z
         */
        LONG centerX = 0;
        LONG centerY = 0;
        LONG centerZ = 0;
        /*
         * FORCE POWER X/Y
         */
        LONG powerX = 0;
        LONG powerY = 0;
        /*
         * FORCE OFFSET X/Y
         */
        LONG offsetX = 0;
        LONG offsetY = 0;
        /*
         * FORCEFIELD VERTEX X/Y/Z
         *
         * The FFB joystick operates in DirectInput's nominal
         * -10000 .. +10000 coordinate space, which matches the
         * coordinates used by the supplied FFShifter preset.
         *
         * Z is retained for completeness even though Stage 1's
         * forcefield geometry is 2D.
         */
        std::vector<POINT> vertices;
        std::vector<LONG> vertexZ;
    };
    /*
     * A complete FFShifter .fff preset.
     */
    struct FFBPreset
    {
        /*
         * FORCEFIELDS FILE VERSION="..."
         */
        std::string fileVersion;
        /*
         * Human-readable preset name.
         *
         * Stage 1 derives this from the filename unless a future
         * metadata format supplies another name.
         */
        std::string name;
        /*
         * Full path to the source .fff file.
         */
        std::string path;
        /*
         * Forcefields in file order.
         */
        std::vector<ForceField> forceFields;
    };
    /*
     * Information displayed by the preset browser.
     */
    struct PresetInfo
    {
        std::string name;
        std::string path;
    };
    /*
     * Runtime state for standalone preset testing.
     *
     * This is deliberately independent of BeamNG.
     */
    struct PresetTestState
    {
        bool enabled = false;
        /*
         * Index into g_loadedPreset.forceFields.
         *
         * -1 means no forcefield currently contains the joystick.
         */
        int activeForceField = -1;
    };
    /*
     * Global preset state.
     */
    extern std::mutex g_presetMutex;
    extern FFBPreset g_loadedPreset;
    extern std::vector<PresetInfo> g_availablePresets;
    extern PresetTestState g_presetTestState;
    // ---------------------------------------------------------------------
    // Logging
    // ---------------------------------------------------------------------
    // Logging is implemented by gui.cpp.
    void Log(const std::string& text);
    template <typename... Args>
    void Logf(const char* format, Args... args)
    {
        char buffer[2048]{};
        sprintf_s(
            buffer,
            sizeof(buffer),
            format,
            args...);
        Log(buffer);
    }
    std::wstring Utf8ToWide(const char* text);
    // ---------------------------------------------------------------------
    // GUI
    // ---------------------------------------------------------------------
    bool CreateMainWindow(
        HINSTANCE instance,
        int showCommand);
    int RunMessageLoop();
    void DestroyMainWindow();
    void UpdateStatus();
    // ---------------------------------------------------------------------
    // UDP
    // ---------------------------------------------------------------------
    bool StartUdpServer();
    void StopUdpServer();
    void SendUdpCommand(
        const std::string& command);
    // ---------------------------------------------------------------------
    // FFB
    // ---------------------------------------------------------------------
    bool CreateSpringEffect();
    bool CreateTestConstantForceEffect();
    bool IsFFBDeviceUsable();
    void StopSpring();
    void StopSpringForRelease();
    void StopTestConstantForce();
    bool SetSpringStrength(
        float strength);
    bool SetTestConstantForce(
        LONG x,
        LONG y);
    /*
     * Apply the force parameters from an FFShifter spring forcefield.
     *
     * This is separate from SetSpringStrength(), which remains the
     * generic CENTER operation.
     */
    bool SetSpringForceField(
        const ForceField& field);
    bool EnsureFFBDeviceReady();
    bool ReacquireFFBDevice();
    void StartFFBWatchdog();
    void StopFFBWatchdog();
    // ---------------------------------------------------------------------
    // Device
    // ---------------------------------------------------------------------
    bool InitializeDirectInput(
        HINSTANCE instance);
    void ShutdownDirectInput();
    bool SelectFirstSuitableDevice();
    void ReleaseFFBDevice();
    // ---------------------------------------------------------------------
    // FFShifter presets
    // ---------------------------------------------------------------------
    /*
     * Load one FFShifter .fff file into g_loadedPreset.
     */
    bool LoadFFBPreset(
        const std::string& path);
    /*
     * Scan the preset directory and rebuild g_availablePresets.
     */
    bool ReloadFFBPresets();
    /*
     * Returns the currently discovered preset list.
     *
     * The returned reference remains valid until the next
     * ReloadFFBPresets() call.
     */
    const std::vector<PresetInfo>& GetAvailablePresets();
    /*
     * Load a preset and enable standalone physical testing.
     *
     * This does not involve BeamNG or UDP.
     */
    bool TestFFBPreset(
        const std::string& path);
    /*
     * Stop standalone preset testing and stop the associated force.
     */
    void StopFFBPresetTest();
    /*
     * Called periodically while preset testing is active.
     *
     * Reads the physical joystick position, determines the active
     * forcefield, and applies its force.
     */
    void UpdatePresetTest();
    /*
     * Find the forcefield containing the supplied DirectInput-style
     * joystick coordinate.
     *
     * Returns:
     *
     *     >= 0 : index into preset.forceFields
     *     -1    : no forcefield contains the point
     */
    int FindForceFieldAtPosition(
        const FFBPreset& preset,
        LONG x,
        LONG y);
    /*
     * Resolve the FFShifter FORCEFIELD NAME into the semantic game
     * action that MultiFFBJoy will eventually send to BeamNG.
     *
     * Matching is case-insensitive.
     *
     * Examples:
     *
     *     "Parking" -> "parking"
     *     "Reverse" -> "reverse"
     *     "Neutral" -> "neutral"
     *     "Drive"   -> "drive"
     *
     * Unknown names return an empty string.
     */
    std::string GetForceFieldAction(
        const ForceField& field);
} // namespace MultiFFBJoy