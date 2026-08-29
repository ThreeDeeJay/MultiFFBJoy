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
#include <cstdio>
#include <exception>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace MultiFFBJoy
{
inline constexpr UINT WM_APP_LOG = WM_APP + 1;
inline constexpr UINT WM_APP_STATUS = WM_APP + 2;
inline constexpr int UDP_PORT = 65458;
inline constexpr DWORD COMMAND_TIMEOUT_MS = 5000;
inline constexpr DWORD SOCKET_TIMEOUT_MS = 25;
inline constexpr int IDC_FFB_UP = 2001;
inline constexpr int IDC_FFB_DOWN = 2002;
inline constexpr int IDC_FFB_LEFT = 2003;
inline constexpr int IDC_FFB_RIGHT = 2004;
inline constexpr int IDC_FFB_STOP = 2005;
inline constexpr int IDC_FFB_CENTER = 2006;
inline constexpr int IDC_PRND_TEST = 2201;
inline constexpr int IDC_PRND_PARK = 2202;
inline constexpr int IDC_PRND_REVERSE = 2203;
inline constexpr int IDC_PRND_NEUTRAL = 2204;
inline constexpr int IDC_PRND_DRIVE = 2205;
inline constexpr int IDC_PRESET_LIST = 2101;
inline constexpr int IDC_PRESET_LOAD = 2102;
inline constexpr LONG FFB_COORD_MIN = -10000;
inline constexpr LONG FFB_COORD_MAX = 10000;

extern HANDLE g_singleInstanceMutex;
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
extern std::recursive_mutex g_ffbMutex;

struct ForceFieldVertex
{
    LONG x = 0;
    LONG y = 0;
    LONG z = 0;
    LONG offsetX = 0;
    LONG offsetY = 0;
};

struct ForceField
{
    std::string name;
    int type = 0;
    int shapeType = 0;
    LONG centerX = 0;
    LONG centerY = 0;
    LONG centerZ = 0;
    std::vector<ForceFieldVertex> vertices;
    int forceType = 0;
    int primaryKeyIndex = -1;
    int secondaryKeyIndex = -1;
    int primarySequentialGearValue = -1;
    int secondarySequentialGearValue = -1;
    LONG powerX = 0;
    LONG powerY = 0;
    LONG offsetX = 0;
    LONG offsetY = 0;
};

struct ActiveSpringState
{
    bool active = false;
    bool forceField = false;
    bool constantForce = false;
    float strength = 0.0f;
    ForceField field;
};

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

struct FFBPreset
{
    std::filesystem::path path;
    std::string fileVersion;
    std::vector<ForceField> forceFields;
};

struct PresetInfo
{
    std::filesystem::path path;
    std::wstring displayName;
};

struct PresetTestState
{
    bool enabled = false;
    int activeForceField = -1;
    float normalizedX = 0.0f;
    float normalizedY = 0.0f;
};

struct VehicleProfileRequest
{
    std::string game;
    std::string vehicleType;
    std::string vehicle;
    std::string configuration;
    std::string transmission;
    std::string gearLayout;
};

struct ResolvedProfile
{
    bool found = false;
    std::string presetName;
    std::filesystem::path presetPath;
    std::string sourcePath;
};

struct VehicleState
{
    int vehicleId = -1;
    std::string vehicle;
    std::string configuration;
    std::string transmission;
    std::string transmissionRaw;
    std::string gear;
    int gearIndex = 0;
    std::string gearboxMode;
    double gearPosition = 0.0;
    std::string automaticModes;
    std::string defaultAutomaticMode;
};

extern VehicleState g_vehicleState;
extern std::atomic<bool> g_vehicleStateValid;
extern std::atomic<bool> g_vehicleTransitioning;

extern DeviceState g_state;
extern ActiveSpringState g_activeSpring;
extern std::vector<DeviceCandidate> g_candidates;
extern std::mutex g_presetMutex;
extern FFBPreset g_loadedPreset;
extern std::vector<PresetInfo> g_availablePresets;
extern PresetTestState g_presetTestState;

// Logging / GUI.
void Log(const std::string& text);
template <typename... Args>
void Logf(const char* format, Args... args)
{
    char buffer[2048]{};
    sprintf_s(buffer, sizeof(buffer), format, args...);
    Log(buffer);
}
std::wstring Utf8ToWide(const char* text);
bool CreateMainWindow(HINSTANCE instance, int showCommand);
int RunMessageLoop();
void DestroyMainWindow();
void UpdateStatus();
void PopulatePresetList();

// Paths / configuration.
std::filesystem::path GetApplicationDirectory();
std::filesystem::path GetConfigurationFilePath();
bool LoadConfigurationFile();
bool ResolveVehicleProfile(const VehicleProfileRequest& request,
                          ResolvedProfile& result);
bool LoadResolvedVehicleProfile(const VehicleProfileRequest& request);

// UDP.
bool StartUdpServer();
void StopUdpServer();
void SendUdpCommand(const std::string& command);
void ApplyVehicleState(const VehicleState& state);
void ClearVehicleState();
void RequestVehicleGear(const std::string& zoneName, int gearIndex);

// DirectInput / FFB.
bool InitializeDirectInput(HINSTANCE instance);
void ShutdownDirectInput();
bool SelectFirstSuitableDevice();
void ReleaseFFBDevice();
bool CreateSpringEffect();
bool CreateTestConstantForceEffect();
bool IsFFBDeviceUsable();
void StopSpringForRelease();
void StopSpring();
void StopTestConstantForce();
bool SetSpringStrength(float strength);
bool SetSpringForceField(const ForceField& forceField);
bool SetConstantForceField(const ForceField& forceField);
bool SetTestConstantForce(LONG x, LONG y);
bool MoveStickToForceFieldCenterOverTime(const ForceField& forceField, DWORD durationMs = 1000);
bool EnsureFFBDeviceReady();
bool ReacquireFFBDevice();
void StartFFBWatchdog();
void StopFFBWatchdog();
bool ReadFFBJoystickPosition(LONG& x, LONG& y);

// Presets / PRND.
std::vector<std::filesystem::path> EnumerateForceFieldPresets();
bool LoadForceFieldPreset(const std::filesystem::path& path);
void ClearForceFieldPreset();
void UpdatePresetTest();
void StartPresetTest();
void StopPresetTest();
std::filesystem::path GetLoadedForceFieldPresetPath();
bool LoadHardCodedPRNDReference();
bool ApplyHardCodedPRNDZone(int zoneIndex);
void StartHardCodedPRNDTest();
void StopHardCodedPRNDTest();
int FindForceFieldAtPosition(LONG x, LONG y);
bool IsForceFieldPresetLoaded();
} // namespace MultiFFBJoy
