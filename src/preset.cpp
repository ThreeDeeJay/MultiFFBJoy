#include "common.h"
#include <cctype>

#include <cstring>
#include <sstream>

namespace MultiFFBJoy
{
std::mutex g_presetMutex;
FFBPreset g_loadedPreset;
std::vector<PresetInfo> g_availablePresets;
PresetTestState g_presetTestState;
VehicleState g_vehicleState;
std::atomic<bool> g_vehicleStateValid{false};
std::atomic<bool> g_vehicleTransitioning{false};
std::atomic<bool> g_vehicleStartupMovePending{true};

namespace
{
std::atomic<bool> g_presetTestRunning{false};
std::thread g_presetTestThread;

std::string Trim(const std::string& value)
{
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string Unquote(std::string value)
{
    value = Trim(value);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
        return value.substr(1, value.size() - 2);
    return value;
}

bool ParseInteger(const std::string& text, LONG& value)
{
    try
    {
        const std::string trimmed = Trim(text);
        size_t used = 0;
        const long parsed = std::stol(trimmed, &used, 10);
        if (used != trimmed.size())
            return false;
        value = static_cast<LONG>(parsed);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool ParseField(const std::string& line, const char* key, std::string& value)
{
    const size_t n = std::strlen(key);
    if (line.size() <= n || line.compare(0, n, key) != 0 || line[n] != '=')
        return false;
    value = Unquote(line.substr(n + 1));
    return true;
}

bool ParseField(const std::string& line, const char* key, LONG& value)
{
    std::string text;
    return ParseField(line, key, text) && ParseInteger(text, value);
}

bool ParseField(const std::string& line, const char* key, int& value)
{
    LONG temp = 0;
    if (!ParseField(line, key, temp))
        return false;
    value = static_cast<int>(temp);
    return true;
}

bool IsSeparator(const std::string& line)
{
    if (line.empty())
        return false;
    return std::all_of(line.begin(), line.end(), [](char c) { return c == '*'; });
}

bool ParseForceFieldFile(const std::filesystem::path& path, FFBPreset& preset)
{
    std::ifstream file(path);
    if (!file)
    {
        Logf("Could not open forcefield preset: %s", path.string().c_str());
        return false;
    }

    ForceField current;
    bool haveCurrent = false;
    int currentVertex = -1;
    std::string line;

    auto finishCurrent = [&]
    {
        if (!haveCurrent)
            return;
        preset.forceFields.push_back(std::move(current));
        current = ForceField{};
        haveCurrent = false;
        currentVertex = -1;
    };

    while (std::getline(file, line))
    {
        line = Trim(line);
        if (line.empty() || IsSeparator(line))
            continue;

        std::string value;
        if (ParseField(line, "FORCEFIELDS FILE VERSION", value))
        {
            preset.fileVersion = value;
            continue;
        }

        if (line.rfind("FORCEFIELD NAME=", 0) == 0)
        {
            finishCurrent();
            haveCurrent = true;
            ParseField(line, "FORCEFIELD NAME", current.name);
            continue;
        }
        if (!haveCurrent)
            continue;

        if (ParseField(line, "FORCEFIELD TYPE", current.type)) continue;
        if (ParseField(line, "FORCEFIELD SHAPE TYPE", current.shapeType)) continue;
        if (ParseField(line, "FORCEFIELD CENTER X", current.centerX)) continue;
        if (ParseField(line, "FORCEFIELD CENTER Y", current.centerY)) continue;
        if (ParseField(line, "FORCEFIELD CENTER Z", current.centerZ)) continue;

        int vertexIndex = -1;
        if (ParseField(line, "FORCEFIELD VERTEX INDEX", vertexIndex))
        {
            if (vertexIndex >= 0)
            {
                currentVertex = vertexIndex;
                if (current.vertices.size() <= static_cast<size_t>(vertexIndex))
                    current.vertices.resize(static_cast<size_t>(vertexIndex) + 1);
            }
            continue;
        }

        if (currentVertex >= 0 && static_cast<size_t>(currentVertex) < current.vertices.size())
        {
            auto& vertex = current.vertices[static_cast<size_t>(currentVertex)];
            if (ParseField(line, "FORCEFIELD VERTEX X", vertex.x)) continue;
            if (ParseField(line, "FORCEFIELD VERTEX Y", vertex.y)) continue;
            if (ParseField(line, "FORCEFIELD VERTEX Z", vertex.z)) continue;
        }

        if (ParseField(line, "FORCE TYPE", current.forceType)) continue;
        if (ParseField(line, "FORCE PRIMARY KEY INDEX", current.primaryKeyIndex)) continue;
        if (ParseField(line, "FORCE SECONDARY KEY INDEX", current.secondaryKeyIndex)) continue;
        if (ParseField(line, "FORCE PRIMARY SEQUENTIAL GEAR VALUE", current.primarySequentialGearValue)) continue;
        if (ParseField(line, "FORCE SECONDARY SEQUENTIAL GEAR VALUE", current.secondarySequentialGearValue)) continue;
        if (ParseField(line, "FORCE POWER X", current.powerX)) continue;
        if (ParseField(line, "FORCE POWER Y", current.powerY)) continue;
        if (ParseField(line, "FORCE OFFSET X", current.offsetX)) continue;
        if (ParseField(line, "FORCE OFFSET Y", current.offsetY)) continue;
    }
    finishCurrent();

    if (preset.forceFields.empty())
    {
        Logf("Forcefield file contains no forcefields: %s", path.string().c_str());
        return false;
    }
    return true;
}

bool PointOnSegment(LONG x, LONG y, const ForceFieldVertex& a, const ForceFieldVertex& b)
{
    const long long cross =
        static_cast<long long>(x - a.x) * (b.y - a.y) -
        static_cast<long long>(y - a.y) * (b.x - a.x);
    if (cross != 0)
        return false;
    return x >= std::min(a.x, b.x) && x <= std::max(a.x, b.x) &&
           y >= std::min(a.y, b.y) && y <= std::max(a.y, b.y);
}

bool PointInside(const ForceField& field, LONG x, LONG y)
{
    if (field.vertices.size() < 3)
        return false;

    bool inside = false;
    size_t j = field.vertices.size() - 1;
    for (size_t i = 0; i < field.vertices.size(); ++i)
    {
        const auto& a = field.vertices[i];
        const auto& b = field.vertices[j];
        if (PointOnSegment(x, y, a, b))
            return true;

        const bool crosses = ((a.y > y) != (b.y > y));
        if (crosses)
        {
            const double intersection =
                static_cast<double>(b.x - a.x) * (y - a.y) /
                    static_cast<double>(b.y - a.y) + a.x;
            if (static_cast<double>(x) < intersection)
                inside = !inside;
        }
        j = i;
    }
    return inside;
}

void LogZoneSummary(const FFBPreset& preset)
{
    for (size_t i = 0; i < preset.forceFields.size(); ++i)
    {
        const auto& field = preset.forceFields[i];
        Logf("  Zone %zu: \"%s\" center=(%ld,%ld) vertices=%zu forceType=%d",
             i, field.name.c_str(), field.centerX, field.centerY,
             field.vertices.size(), field.forceType);
    }
}

FFBPreset BuildHardCodedPRND()
{
    FFBPreset preset;
    preset.fileVersion = "hard-coded-reference";
    auto make = [](const char* name, LONG centerY, LONG powerX, LONG powerY)
    {
        ForceField field;
        field.name = name;
        field.type = 1;
        field.shapeType = 1;
        field.centerX = 0;
        field.centerY = centerY;
        field.forceType = 1;
        field.powerX = powerX;
        field.powerY = powerY;
        // Full-width straight PRND zones, top to bottom.
        field.vertices = {
            {-10000, centerY - 2500, 0, 0, 0},
            { 10000, centerY - 2500, 0, 0, 0},
            { 10000, centerY + 2500, 0, 0, 0},
            {-10000, centerY + 2500, 0, 0, 0},
        };
        return field;
    };
    preset.forceFields.push_back(make("Park", -8500, -10000, 10000));
    preset.forceFields.push_back(make("Reverse", -3500, 10000, 10000));
    preset.forceFields.push_back(make("Neutral", 3500, 10000, 10000));
    preset.forceFields.push_back(make("Drive", 8500, -10000, 10000));
    return preset;
}

std::string NormalizeGearToken(const std::string& value)
{
    std::string result;
    for (unsigned char c : value)
        if (std::isalnum(c)) result.push_back(static_cast<char>(std::toupper(c)));
    return result;
}

int ExtractFirstInteger(const std::string& value)
{
    bool found = false;
    int number = 0;

    for (unsigned char c : value)
    {
        if (std::isdigit(c))
        {
            found = true;
            number = number * 10 + static_cast<int>(c - '0');
        }
        else if (found)
        {
            break;
        }
    }

    return found ? number : -1;
}

bool GearMatchesField(const ForceField& field, const VehicleState& state)
{
    const std::string gear = NormalizeGearToken(state.gear);
    const std::string name = NormalizeGearToken(field.name);
    std::string mode = NormalizeGearToken(state.gearboxMode);
    if (mode.empty())
        mode = NormalizeGearToken(state.defaultAutomaticMode);

    if (name.empty())
        return false;

    if (gear.empty())
    {
        if (mode == "PARK" && name == "PARK") return true;
        if (mode == "REVERSE" && name == "REVERSE") return true;
        if (mode == "NEUTRAL" && name == "NEUTRAL") return true;
        if (mode == "DRIVE" && name == "DRIVE") return true;
        if (mode == "LOW" && (name == "LOW" || name == "L")) return true;
    }

    if (gear == "P" && name == "PARK") return true;
    if (gear == "R" && name == "REVERSE") return true;
    if (gear == "N" && name == "NEUTRAL") return true;
    if (gear == "D" && name == "DRIVE") return true;
    if ((gear == "L" || gear == "LOW") && (name == "LOW" || name == "L")) return true;

    const int gearNumber = ExtractFirstInteger(gear);
    const int fieldNumber = ExtractFirstInteger(name);
    if (gearNumber >= 0 && fieldNumber >= 0 && gearNumber == fieldNumber)
        return true;

    if (state.gearIndex != 0 &&
        (field.primarySequentialGearValue == state.gearIndex ||
         field.secondarySequentialGearValue == state.gearIndex))
        return true;

    return !gear.empty() && gear == name;
}

int GetRequestedGearIndex(const ForceField& field, const VehicleState& state)
{
    const std::string name = NormalizeGearToken(field.name);
    const std::string gear = NormalizeGearToken(state.gear);

    // Automatic gearbox: automaticModes is the actual gear-lever layout.
    // Its character index is the index accepted by shiftToGearIndex().
    if (!state.automaticModes.empty())
    {
        const std::string modes = NormalizeGearToken(state.automaticModes);
        const int numericField = ExtractFirstInteger(field.name);
        for (size_t i = 0; i < modes.size(); ++i)
        {
            const char mode = modes[i];
            std::string token(1, mode);
            if ((token == "P" && name == "PARK") ||
                (token == "R" && name == "REVERSE") ||
                (token == "N" && name == "NEUTRAL") ||
                (token == "D" && name == "DRIVE") ||
                (token == "L" && (name == "LOW" || name == "L")) ||
                (numericField >= 0 && mode >= '0' && mode <= '9' &&
                 numericField == static_cast<int>(mode - '0')))
                return static_cast<int>(i) + 1;
        }
    }

    // Manual/sequential presets: use the number embedded in the zone name.
    const int numeric = ExtractFirstInteger(field.name);
    if (numeric >= 0)
        return numeric;

    if (name == "PARK") return 0;
    if (name == "REVERSE")
        return NormalizeGearToken(state.transmission) == "AUTOMATIC" ? 1 : -1;
    if (name == "NEUTRAL") return NormalizeGearToken(state.transmission) == "AUTOMATIC" ? 2 : 0;
    if (name == "DRIVE") return NormalizeGearToken(state.transmission) == "AUTOMATIC" ? 3 : 1;
    if (name == "LOW" || name == "L") return NormalizeGearToken(state.transmission) == "AUTOMATIC" ? 4 : 1;

    // If the preset zone corresponds to the current state, retain it.
    if (state.gearIndex != 0 && GearMatchesField(field, state))
        return state.gearIndex;

    (void)gear;
    return -99999;
}

void TriggerZoneGearSelection(const ForceField& field, const VehicleState& state)
{
    const int index = GetRequestedGearIndex(field, state);
    if (index == -99999)
        return;

    RequestVehicleGear(field.name, index);
}

void ApplyVehicleStateImpl(const VehicleState& state)
{
    ForceField selected;
    bool found = false;
    bool singleZone = false;
    int index = -1;

    {
        std::lock_guard<std::mutex> lock(g_presetMutex);
        if (g_loadedPreset.forceFields.empty())
            return;

        if (g_loadedPreset.forceFields.size() == 1)
        {
            singleZone = true;
            selected = g_loadedPreset.forceFields.front();
            index = 0;
            found = true;
        }
        else
        {
            for (size_t i = 0; i < g_loadedPreset.forceFields.size(); ++i)
            {
                if (GearMatchesField(g_loadedPreset.forceFields[i], state))
                {
                    selected = g_loadedPreset.forceFields[i];
                    index = static_cast<int>(i);
                    found = true;
                    break;
                }
            }
        }
    }

    if (!found)
    {
        Logf("Vehicle state has no matching forcefield: gear=\"%s\" gearIndex=%d; using basic centering.",
             state.gear.c_str(), state.gearIndex);
        {
            std::lock_guard<std::mutex> lock(g_presetMutex);
            g_vehicleState = state;
            g_vehicleStateValid = true;
        }
        SetSpringStrength(1.0f);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_presetMutex);
        g_vehicleState = state;
        g_vehicleStateValid = true;
    }

    if (singleZone)
        Logf("Using single-zone forcefield \"%s\".", selected.name.c_str());
    else
        Logf("Vehicle gear state -> zone \"%s\" (index=%d, gear=%s, gearIndex=%d).",
             selected.name.c_str(), index, state.gear.c_str(), state.gearIndex);

    // The first valid vehicle state after a profile load is the startup
    // position. Move there gradually before the physical zone monitor is
    // allowed to generate further gear changes.
    const bool startupMove = g_vehicleStartupMovePending.exchange(
        false, std::memory_order_acq_rel);

    if (startupMove)
        MoveStickToForceFieldCenterOverTime(selected, 1000);

    if (selected.forceType == 1)
    {
        SetSpringForceField(selected);
    }
    else if (selected.forceType == 0 || selected.forceType == 2)
    {
        SetConstantForceField(selected);
    }
    else
    {
        StopSpring();
    }

    TriggerZoneGearSelection(selected, state);
}

void ClearVehicleStateImpl()
{
    g_vehicleStateValid = false;
    g_vehicleStartupMovePending = true;
    std::lock_guard<std::mutex> lock(g_presetMutex);
    g_vehicleState = VehicleState{};
}

void PresetMonitorThread()
{
    Log("Preset monitor thread started.");
    while (g_presetTestRunning.load(std::memory_order_acquire))
    {
        UpdatePresetTest();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    Log("Preset monitor thread stopped.");
}
} // namespace

void ApplyVehicleState(const VehicleState& state)
{
    ApplyVehicleStateImpl(state);
}

void ClearVehicleState()
{
    ClearVehicleStateImpl();
}

bool LoadForceFieldPreset(const std::filesystem::path& path)
{
    // Loading a new profile is an atomic profile transition: stop the monitor
    // and current spring before replacing the preset it reads.
    StopPresetTest();
    FFBPreset parsed;
    parsed.path = path;
    if (!ParseForceFieldFile(path, parsed))
        return false;

    {
        std::lock_guard<std::mutex> lock(g_presetMutex);
        g_loadedPreset = std::move(parsed);
    }
    Logf("Loaded forcefield preset: %s", path.string().c_str());
    {
        std::lock_guard<std::mutex> lock(g_presetMutex);
        Logf("Forcefield preset contains %zu forcefield(s).",
             g_loadedPreset.forceFields.size());
        LogZoneSummary(g_loadedPreset);
    }
    return true;
}

bool IsForceFieldPresetLoaded()
{
    std::lock_guard<std::mutex> lock(g_presetMutex);
    return !g_loadedPreset.forceFields.empty();
}

std::filesystem::path GetLoadedForceFieldPresetPath()
{
    std::lock_guard<std::mutex> lock(g_presetMutex);
    return g_loadedPreset.path;
}

std::vector<std::filesystem::path> EnumerateForceFieldPresets()
{
    std::vector<std::filesystem::path> result;
    const auto directory = GetApplicationDirectory() / "forcefields";
    std::error_code ec;
    if (!std::filesystem::exists(directory, ec))
        return result;

    for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
    {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        if (_wcsicmp(entry.path().extension().c_str(), L".fff") == 0)
            result.push_back(entry.path());
    }
    std::sort(result.begin(), result.end());
    return result;
}

bool LoadHardCodedPRNDReference()
{
    StopPresetTest();
    auto preset = BuildHardCodedPRND();
    {
        std::lock_guard<std::mutex> lock(g_presetMutex);
        g_loadedPreset = std::move(preset);
        LogZoneSummary(g_loadedPreset);
    }
    Log("Loaded hard-coded PRND reference.");
    return true;
}

bool ApplyHardCodedPRNDZone(int zoneIndex)
{
    if (zoneIndex < 0 || zoneIndex >= 4)
        return false;
    if (!EnsureFFBDeviceReady())
        return false;

    if (!LoadHardCodedPRNDReference())
        return false;

    ForceField field;
    {
        std::lock_guard<std::mutex> lock(g_presetMutex);
        field = g_loadedPreset.forceFields[static_cast<size_t>(zoneIndex)];
    }
    Logf("Hard-coded PRND zone: %s (index=%d).", field.name.c_str(), zoneIndex);
    return SetSpringForceField(field);
}

void StartHardCodedPRNDTest()
{
    if (LoadHardCodedPRNDReference())
        StartPresetTest();
}

void StopHardCodedPRNDTest()
{
    StopPresetTest();
}

void UpdatePresetTest()
{
    bool enabled = false;
    {
        std::lock_guard<std::mutex> lock(g_presetMutex);
        enabled = g_presetTestState.enabled;
    }
    if (!enabled || !IsForceFieldPresetLoaded() || !EnsureFFBDeviceReady())
        return;
    if (g_vehicleTransitioning.load(std::memory_order_acquire))
        return;

    LONG x = 0, y = 0;
    if (!ReadFFBJoystickPosition(x, y))
        return;

    const int index = FindForceFieldAtPosition(x, y);
    ForceField selected;
    bool haveField = false;
    bool changed = false;
    VehicleState state;
    bool haveState = false;

    {
        std::lock_guard<std::mutex> lock(g_presetMutex);
        if (index >= 0 && static_cast<size_t>(index) < g_loadedPreset.forceFields.size())
        {
            selected = g_loadedPreset.forceFields[static_cast<size_t>(index)];
            haveField = true;
        }
        if (g_presetTestState.activeForceField != index)
        {
            g_presetTestState.activeForceField = index;
            g_presetTestState.normalizedX = static_cast<float>(x);
            g_presetTestState.normalizedY = static_cast<float>(y);
            changed = true;
        }
        state = g_vehicleState;
        haveState = g_vehicleStateValid.load(std::memory_order_acquire);
    }

    if (!changed)
        return;

    if (!haveField)
    {
        Logf("Preset zone: none (stick X=%ld Y=%ld).", x, y);
        StopSpring();
        return;
    }

    if (selected.forceType == 1)
        SetSpringForceField(selected);
    else if (selected.forceType == 0 || selected.forceType == 2)
        SetConstantForceField(selected);
    else
        StopSpring();

    if (haveState)
        TriggerZoneGearSelection(selected, state);
}

void StartPresetTest()
{
    ClearVehicleStateImpl();
    if (!IsForceFieldPresetLoaded() || !EnsureFFBDeviceReady())
        return;
    {
        std::lock_guard<std::mutex> lock(g_presetMutex);
        g_presetTestState.enabled = true;
        g_presetTestState.activeForceField = -1;
        g_presetTestState.normalizedX = 0.0f;
        g_presetTestState.normalizedY = 0.0f;
    }
    if (!g_presetTestRunning.exchange(true, std::memory_order_acq_rel))
        g_presetTestThread = std::thread(PresetMonitorThread);
}

void StopPresetTest()
{
    ClearVehicleStateImpl();
    {
        std::lock_guard<std::mutex> lock(g_presetMutex);
        g_presetTestState = PresetTestState{};
    }
    if (g_presetTestRunning.exchange(false, std::memory_order_acq_rel) &&
        g_presetTestThread.joinable())
        g_presetTestThread.join();
    StopSpring();
}

void ClearForceFieldPreset()
{
    StopPresetTest();
    {
        std::lock_guard<std::mutex> lock(g_presetMutex);
        g_loadedPreset = FFBPreset{};
    }
    StopTestConstantForce();
    Log("Forcefield preset cleared.");
}

int FindForceFieldAtPosition(LONG x, LONG y)
{
    std::lock_guard<std::mutex> lock(g_presetMutex);
    for (size_t i = 0; i < g_loadedPreset.forceFields.size(); ++i)
        if (PointInside(g_loadedPreset.forceFields[i], x, y))
            return static_cast<int>(i);
    return -1;
}
} // namespace MultiFFBJoy
