#include "common.h"
#include <cctype>
#include <cstring>
#include <limits>
#include <sstream>
namespace MultiFFBJoy
{
    std::mutex g_presetMutex;
    FFBPreset g_loadedPreset;
    namespace
    {
        std::atomic<bool> g_presetTestRunning{ false };
        std::thread g_presetTestThread;
    }
    std::vector<PresetInfo> g_availablePresets;
    PresetTestState g_presetTestState;
    namespace
    {
        std::string Trim(
            const std::string& value)
        {
            const auto first =
            value.find_first_not_of(" \t\r\n");
            if (first == std::string::npos)
                return {};
            const auto last =
            value.find_last_not_of(" \t\r\n");
            return value.substr(
                first,
                last - first + 1);
        }
        std::string Unquote(
            const std::string& value)
        {
            std::string result = Trim(value);
            if (result.size() >= 2 &&
                result.front() == '"' &&
                result.back() == '"')
            {
                result =
                result.substr(
                    1,
                    result.size() - 2);
            }
            return result;
        }
        bool ParseLong(
            const std::string& value,
            LONG& result)
        {
            try
            {
                const std::string trimmed =
                Trim(value);
                size_t used = 0;
                const long parsed =
                std::stol(
                    trimmed,
                    &used,
                    10);
                if (used != trimmed.size())
                    return false;
                result =
                static_cast<LONG>(parsed);
                return true;
            }
            catch (...)
            {
                return false;
            }
        }
        bool ParseInt(
            const std::string& value,
            int& result)
        {
            LONG temp = 0;
            if (!ParseLong(
                value,
                temp))
            {
                return false;
            }
            result =
            static_cast<int>(temp);
            return true;
        }
        bool ParseString(
            const std::string& line,
            const char* key,
            std::string& value)
        {
            const size_t keyLength =
            std::strlen(key);
            if (line.compare(
                0,
                keyLength,
                key) != 0)
            {
                return false;
            }
            if (line.size() <= keyLength ||
                line[keyLength] != '=')
            {
                return false;
            }
            value =
            Unquote(
                line.substr(
                    keyLength + 1));
            return true;
        }
        bool ParseIntegerField(
            const std::string& line,
            const char* key,
            LONG& value)
        {
            std::string text;
            if (!ParseString(
                line,
                key,
                text))
            {
                return false;
            }
            return ParseLong(
                text,
                value);
        }
        bool ParseIntegerField(
            const std::string& line,
            const char* key,
            int& value)
        {
            std::string text;
            if (!ParseString(
                line,
                key,
                text))
            {
                return false;
            }
            return ParseInt(
                text,
                value);
        }
        bool IsForceFieldNameLine(
            const std::string& line)
        {
            return
            line.rfind(
                "FORCEFIELD NAME=",
                0) == 0;
        }
        bool IsSeparator(
            const std::string& line)
        {
            if (line.empty())
                return false;
            for (const char ch : line)
            {
                if (ch != '*')
                    return false;
            }
            return true;
        }
        bool ParseForceFieldFile(
            const std::filesystem::path& path,
            FFBPreset& preset)
        {
            std::ifstream file(
                path,
                std::ios::in);
            if (!file)
            {
                Logf(
                    "Could not open forcefield preset: %s",
                    path.string().c_str());
                return false;
            }
            ForceField current;
            bool haveCurrent = false;
            std::string line;
            while (std::getline(
                file,
                line))
            {
                line = Trim(line);
                if (line.empty())
                    continue;
                if (IsSeparator(line))
                    continue;
                std::string value;
                if (ParseString(
                    line,
                    "FORCEFIELDS FILE VERSION",
                    value))
                {
                    preset.fileVersion = value;
                    continue;
                }
                LONG ignoredCount = 0;
                if (ParseIntegerField(
                    line,
                    "NO OF FORCEFIELDS",
                    ignoredCount))
                {
                    continue;
                }
                if (IsForceFieldNameLine(line))
                {
                    if (haveCurrent)
                    {
                        preset.forceFields.push_back(
                            current);
                    }
                    current =
                    ForceField{};
                    haveCurrent = true;
                    ParseString(
                        line,
                        "FORCEFIELD NAME",
                        current.name);
                    continue;
                }
                if (!haveCurrent)
                    continue;
                ParseIntegerField(
                    line,
                    "FORCEFIELD TYPE",
                    current.type);
                ParseIntegerField(
                    line,
                    "FORCEFIELD SHAPE TYPE",
                    current.shapeType);
                ParseIntegerField(
                    line,
                    "FORCEFIELD CENTER X",
                    current.centerX);
                ParseIntegerField(
                    line,
                    "FORCEFIELD CENTER Y",
                    current.centerY);
                ParseIntegerField(
                    line,
                    "FORCEFIELD CENTER Z",
                    current.centerZ);
                int vertexIndex = -1;
                if (ParseIntegerField(
                    line,
                    "FORCEFIELD VERTEX INDEX",
                    vertexIndex))
                {
                    if (vertexIndex >= 0)
                    {
                        const size_t index =
                        static_cast<size_t>(
                            vertexIndex);
                        if (current.vertices.size() <= index)
                        {
                            current.vertices.resize(
                                index + 1);
                        }
                    }
                    continue;
                }
                if (!current.vertices.empty())
                {
                    auto& vertex =
                    current.vertices.back();
                    ParseIntegerField(
                        line,
                        "FORCEFIELD VERTEX X",
                        vertex.x);
                    ParseIntegerField(
                        line,
                        "FORCEFIELD VERTEX Y",
                        vertex.y);
                    ParseIntegerField(
                        line,
                        "FORCEFIELD VERTEX Z",
                        vertex.z);
                }
                ParseIntegerField(
                    line,
                    "FORCE TYPE",
                    current.forceType);
                ParseIntegerField(
                    line,
                    "FORCE PRIMARY KEY INDEX",
                    current.primaryKeyIndex);
                ParseIntegerField(
                    line,
                    "FORCE SECONDARY KEY INDEX",
                    current.secondaryKeyIndex);
                ParseIntegerField(
                    line,
                    "FORCE PRIMARY SEQUENTIAL GEAR VALUE",
                    current.primarySequentialGearValue);
                ParseIntegerField(
                    line,
                    "FORCE SECONDARY SEQUENTIAL GEAR VALUE",
                    current.secondarySequentialGearValue);
                ParseIntegerField(
                    line,
                    "FORCE POWER X",
                    current.powerX);
                ParseIntegerField(
                    line,
                    "FORCE POWER Y",
                    current.powerY);
                ParseIntegerField(
                    line,
                    "FORCE OFFSET X",
                    current.offsetX);
                ParseIntegerField(
                    line,
                    "FORCE OFFSET Y",
                    current.offsetY);
            }
            if (haveCurrent)
            {
                preset.forceFields.push_back(
                    current);
            }
            if (preset.forceFields.empty())
            {
                Logf(
                    "Forcefield file contains no forcefields: %s",
                    path.string().c_str());
                return false;
            }
            return true;
        }
        bool PointInsideForceField(
            const ForceField& field,
            LONG x,
            LONG y)
        {
            if (field.vertices.size() < 3)
                return false;
            bool inside = false;
            size_t j =
            field.vertices.size() - 1;
            for (size_t i = 0;
                i < field.vertices.size();
                ++i)
            {
                const LONG xi =
                field.vertices[i].x;
                const LONG yi =
                field.vertices[i].y;
                const LONG xj =
                field.vertices[j].x;
                const LONG yj =
                field.vertices[j].y;
                const bool crosses =
                ((yi > y) != (yj > y));
                if (crosses)
                {
                    const double intersection =
                    static_cast<double>(xj - xi) *
                    static_cast<double>(y - yi) /
                    static_cast<double>(yj - yi) +
                    static_cast<double>(xi);
                    if (static_cast<double>(x) < intersection)
                    {
                        inside = !inside;
                    }
                }
                j = i;
            }
            return inside;
        }
        LONG NormalizeDirectInputAxis(
            LONG value)
        {
// DirectInput joystick axes normally use
// 0..65535 after SetProperty(DIPROP_RANGE).
//
// Convert that to FFShifter's
// -10000..10000 coordinate system.
            constexpr LONG DIRECT_INPUT_MIN = 0;
            constexpr LONG DIRECT_INPUT_MAX = 65535;
            value =
            std::clamp(
                value,
                DIRECT_INPUT_MIN,
                DIRECT_INPUT_MAX);
            const double normalized =
            (static_cast<double>(value) -
                DIRECT_INPUT_MIN) /
            static_cast<double>(
                DIRECT_INPUT_MAX -
                DIRECT_INPUT_MIN);
            const double ff =
            -10000.0 +
            normalized * 20000.0;
            return static_cast<LONG>(
                std::lround(ff));
        }
        void LogZoneSummary(
            const FFBPreset& preset)
        {
            for (size_t i = 0;
                i < preset.forceFields.size();
                ++i)
            {
                const ForceField& field =
                preset.forceFields[i];
                Logf(
                    "  Zone %zu: \"%s\" center=(%ld,%ld) "
                    "vertices=%zu forceType=%d",
                    i,
                    field.name.c_str(),
                    field.centerX,
                    field.centerY,
                    field.vertices.size(),
                    field.forceType);
            }
        }
        void PresetTestMonitorThread()
        {
            Log(
                "Preset monitor thread started.");
            while (g_presetTestRunning.load())
            {
                UpdatePresetTest();
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(10));
            }
            Log(
                "Preset monitor thread stopped.");
        }
} // anonymous namespace
bool LoadForceFieldPreset(
    const std::filesystem::path& path)
{
    FFBPreset parsed;
    parsed.path = path;
    if (!ParseForceFieldFile(
        path,
        parsed))
    {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(
            g_presetMutex);
        g_loadedPreset =
        std::move(parsed);
    }
    Logf(
        "Loaded forcefield preset: %s",
        path.string().c_str());
    {
        std::lock_guard<std::mutex> lock(
            g_presetMutex);
        Logf(
            "Forcefield preset contains %zu forcefield(s).",
            g_loadedPreset.forceFields.size());
        for (size_t i = 0;
            i < g_loadedPreset.forceFields.size();
            ++i)
        {
            const auto& field =
            g_loadedPreset.forceFields[i];
            Logf(
                "  Zone %zu: \"%s\" center=(%ld,%ld) "
                "vertices=%zu forceType=%d",
                i,
                field.name.c_str(),
                field.centerX,
                field.centerY,
                field.vertices.size(),
                field.forceType);
        }
    }
    return true;
}
void ClearForceFieldPreset()
{
    StopPresetTestMonitor();
    {
        std::lock_guard<std::mutex> lock(
            g_presetMutex);
        g_loadedPreset =
        FFBPreset{};
        g_presetTestState =
        PresetTestState{};
    }
    StopSpring();
    StopTestConstantForce();
    Log(
        "Forcefield preset cleared.");
}
bool IsForceFieldPresetLoaded()
{
    std::lock_guard<std::mutex> lock(
        g_presetMutex);
    return !g_loadedPreset.forceFields.empty();
}
std::filesystem::path
GetLoadedForceFieldPresetPath()
{
    std::lock_guard<std::mutex> lock(
        g_presetMutex);
    return
    g_loadedPreset.path;
}
std::vector<std::filesystem::path>
EnumerateForceFieldPresets()
{
    std::vector<std::filesystem::path> result;
    const std::filesystem::path directory =
    std::filesystem::current_path() /
    "forcefields";
    std::error_code ec;
    if (!std::filesystem::exists(
        directory,
        ec))
    {
        return result;
    }
    for (const auto& entry :
        std::filesystem::directory_iterator(
            directory,
            ec))
    {
        if (ec)
            break;
        if (!entry.is_regular_file(ec))
            continue;
        const auto extension =
        entry.path().extension().wstring();
        if (_wcsicmp(
            extension.c_str(),
            L".fff") == 0)
        {
            result.push_back(
                entry.path());
        }
    }
    std::sort(
        result.begin(),
        result.end());
    return result;
}
namespace
{
    FFBPreset BuildHardCodedPRNDPreset()
    {
        FFBPreset preset;
        preset.fileVersion = "hard-coded-reference";
        auto makeField = [](const char* name,
                            LONG centerY,
                            LONG powerX,
                            LONG powerY)
        {
            ForceField field;
            field.name = name;
            field.type = 1;
            field.shapeType = 1;
            field.centerX = 0;
            field.centerY = centerY;
            field.centerZ = 0;
            field.forceType = 1;
            field.powerX = powerX;
            field.powerY = powerY;
            return field;
        };
        // Logical FFShifter order is top -> bottom:
        // Park, Reverse, Neutral, Drive.
        //
        // Park/Drive deliberately retain the profile's full spring
        // strength but use the physical travel edge as their equilibrium.
        // SetSpringForceField() performs the FFB2 condition-axis mapping.
        preset.forceFields.push_back(
            makeField("Park", -8500, -10000, 10000));
        preset.forceFields.push_back(
            makeField("Reverse", -3500, 10000, 10000));
        preset.forceFields.push_back(
            makeField("Neutral", 3500, 10000, 10000));
        preset.forceFields.push_back(
            makeField("Drive", 8500, -10000, 10000));
        return preset;
    }
}
bool LoadHardCodedPRNDReference()
{
    StopPresetTest();
    FFBPreset preset = BuildHardCodedPRNDPreset();
    {
        std::lock_guard<std::mutex> lock(g_presetMutex);
        g_loadedPreset = std::move(preset);
    }
    Log("Loaded hard-coded PRND reference (equivalent straight PRND profile).");
    LogZoneSummary(g_loadedPreset);
    return true;
}
bool ApplyHardCodedPRNDZone(int zoneIndex)
{
    if (zoneIndex < 0 || zoneIndex >= 4)
    {
        Logf("Invalid hard-coded PRND zone index: %d.", zoneIndex);
        return false;
    }
    if (!EnsureFFBDeviceReady())
    {
        Log("Hard-coded PRND test ignored: FFB device unavailable.");
        return false;
    }
    StopPresetTest();
    if (!LoadHardCodedPRNDReference())
    {
        return false;
    }
    ForceField field;
    {
        std::lock_guard<std::mutex> lock(g_presetMutex);
        if (static_cast<size_t>(zoneIndex) >= g_loadedPreset.forceFields.size())
            return false;
        field = g_loadedPreset.forceFields[static_cast<size_t>(zoneIndex)];
    }
    Logf(
        "Hard-coded PRND zone: %s (index=%d).",
        field.name.c_str(),
        zoneIndex);
    return SetSpringForceField(field);
}
void StartHardCodedPRNDTest()
{
    if (!LoadHardCodedPRNDReference())
        return;
    Log("Starting hard-coded PRND position-aware test.");
    StartPresetTest();
}
void StopHardCodedPRNDTest()
{
    StopPresetTest();
}
void UpdatePresetTest()
{
    if (!IsForceFieldPresetLoaded())
    {
        return;
    }
    if (!EnsureFFBDeviceReady())
    {
        return;
    }
    LONG x = 0;
    LONG y = 0;
    if (!ReadFFBJoystickPosition(x, y))
    {
        return;
    }
    const int forceFieldIndex =
    FindForceFieldAtPosition(x, y);
    ForceField selectedField;
    bool haveField = false;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(
            g_presetMutex);
        if (forceFieldIndex >= 0 &&
            static_cast<size_t>(forceFieldIndex) <
            g_loadedPreset.forceFields.size())
        {
            selectedField =
            g_loadedPreset.forceFields[
                static_cast<size_t>(forceFieldIndex)];
            haveField = true;
        }
        if (g_presetTestState.activeForceField !=
            forceFieldIndex)
        {
            g_presetTestState.activeForceField =
            forceFieldIndex;
            g_presetTestState.normalizedX =
            static_cast<float>(x);
            g_presetTestState.normalizedY =
            static_cast<float>(y);
            changed = true;
        }
    }
/*
* Only change the physical FFB effect when the
* active zone changes.
*/
    if (!changed)
    {
        return;
    }
    if (!haveField)
    {
        Logf(
            "Preset zone: none (stick X=%ld Y=%ld).",
            x,
            y);
        StopSpring();
        return;
    }
    Logf(
        "Preset zone: \"%s\" (index=%d, X=%ld Y=%ld).",
        selectedField.name.c_str(),
        forceFieldIndex,
        x,
        y);
    if (selectedField.forceType != 1)
    {
        Logf(
            "Preset zone \"%s\" has unsupported "
            "forceType=%d; stopping spring.",
            selectedField.name.c_str(),
            selectedField.forceType);
        StopSpring();
        return;
    }
/*
* THIS is the important call that was missing.
*
* The selected field is a spring forcefield, so send
* its center and coefficients to DirectInput.
*/
    if (SetSpringForceField(selectedField))
    {
        Logf(
            "Applied spring forcefield \"%s\": "
            "center=(%ld,%ld), power=(%ld,%ld).",
            selectedField.name.c_str(),
            selectedField.centerX,
            selectedField.centerY,
            selectedField.powerX,
            selectedField.powerY);
    }
    else
    {
        Logf(
            "Failed to apply spring forcefield \"%s\".",
            selectedField.name.c_str());
    }
}
void StartPresetTestMonitor()
{
    if (g_presetTestRunning.exchange(true))
    {
        return;
    }
    g_presetTestThread =
    std::thread(PresetTestMonitorThread);
}
void StopPresetTestMonitor()
{
    if (!g_presetTestRunning.exchange(false))
    {
        return;
    }
    if (g_presetTestThread.joinable())
    {
        g_presetTestThread.join();
    }
}
void StartPresetTest()
{
    if (!IsForceFieldPresetLoaded())
    {
        Log(
            "Preset test ignored: no preset loaded.");
        return;
    }
    if (!EnsureFFBDeviceReady())
    {
        Log(
            "Preset test ignored: FFB device unavailable.");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(
            g_presetMutex);
        g_presetTestState.enabled = true;
        g_presetTestState.activeForceField = -1;
        g_presetTestState.normalizedX = 0.0f;
        g_presetTestState.normalizedY = 0.0f;
    }
    Log(
        "Preset test enabled: spring forcefield zones "
        "are now position-aware.");
    Log(
        "Preset zone tracking started.");
    if (!g_presetTestRunning.exchange(true))
    {
        g_presetTestThread =
        std::thread(
            PresetTestMonitorThread);
    }
}
int FindForceFieldAtPosition(
    LONG x,
    LONG y)
{
    std::lock_guard<std::mutex> lock(
        g_presetMutex);
    if (g_loadedPreset.forceFields.empty())
        return -1;
/*
* The .fff coordinates and the joystick coordinates
* are both now explicitly represented as
* -10000 .. +10000.
*/
    for (size_t i = 0;
        i < g_loadedPreset.forceFields.size();
        ++i)
    {
        const ForceField& field =
        g_loadedPreset.forceFields[i];
        if (field.vertices.empty())
            continue;
        LONG minX =
        field.vertices.front().x;
        LONG maxX =
        field.vertices.front().x;
        LONG minY =
        field.vertices.front().y;
        LONG maxY =
        field.vertices.front().y;
        for (const auto& vertex :
            field.vertices)
        {
            minX =
            std::min(
                minX,
                vertex.x);
            maxX =
            std::max(
                maxX,
                vertex.x);
            minY =
            std::min(
                minY,
                vertex.y);
            maxY =
            std::max(
                maxY,
                vertex.y);
        }
/*
* The current PRND presets use rectangular zones.
*
* Keep the implementation deliberately simple for
* now; polygon support can be added later.
*/
        if (x >= minX &&
            x <= maxX &&
            y >= minY &&
            y <= maxY)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}
void StopPresetTest()
{
    {
        std::lock_guard<std::mutex> lock(
            g_presetMutex);
        g_presetTestState =
        PresetTestState{};
    }
    StopPresetTestMonitor();
    StopSpring();
    Log(
        "Preset zone tracking stopped.");
}
} // namespace MultiFFBJoy