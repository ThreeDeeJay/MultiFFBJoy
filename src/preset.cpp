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
        void PresetTestThreadProc()
        {
            Log(
                "Preset monitor thread started.");
            int lastZone = -2;
            while (
                g_presetTestRunning.load(
                    std::memory_order_acquire))
            {
                bool enabled = false;
                {
                    std::lock_guard<std::mutex> lock(
                        g_presetMutex);
                    enabled =
                    g_presetTestState.enabled;
                }
                if (!enabled)
                {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(10));
                    continue;
                }
                LONG x = 0;
                LONG y = 0;
                if (!ReadFFBJoystickPosition(
                    x,
                    y))
                {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(10));
                    continue;
                }
                const int zone =
                FindForceFieldAtPosition(
                    x,
                    y);
                if (zone != lastZone)
                {
                    if (zone >= 0)
                    {
                        std::string zoneName;
                        ForceField field;
                        {
                            std::lock_guard<std::mutex> lock(
                                g_presetMutex);
                            if (zone <
                                static_cast<int>(
                                    g_loadedPreset.forceFields.size()))
                            {
                                field =
                                g_loadedPreset.forceFields[
                                    static_cast<size_t>(zone)];
                                zoneName =
                                field.name;
                            }
                        }
                        Logf(
                            "Preset zone: \"%s\" "
                            "(index=%d, X=%ld Y=%ld).",
                            zoneName.c_str(),
                            zone,
                            x,
                            y);
                        if (zoneName.empty())
                        {
                            lastZone = zone;
                            std::this_thread::sleep_for(
                                std::chrono::milliseconds(10));
                            continue;
                        }
                        if (field.forceType == 1)
                        {
                            if (SetSpringForceField(
                                field))
                            {
                                Logf(
                                    "Applied spring forcefield "
                                    "\"%s\": center=(%ld,%ld), "
                                    "power=(%ld,%ld), "
                                    "offset=(%ld,%ld).",
                                    field.name.c_str(),
                                    field.centerX,
                                    field.centerY,
                                    field.powerX,
                                    field.powerY,
                                    field.offsetX,
                                    field.offsetY);
                            }
                            else
                            {
                                Logf(
                                    "Failed to apply spring "
                                    "forcefield \"%s\".",
                                    field.name.c_str());
                            }
                        }
                        else
                        {
                            Logf(
                                "Preset zone \"%s\" has "
                                "unsupported force type %d; "
                                "constant force will be "
                                "implemented later.",
                                field.name.c_str(),
                                field.forceType);
                        }
                    }
                    else
                    {
                        Logf(
                            "Preset zone: none "
                            "(stick X=%ld Y=%ld).",
                            x,
                            y);
                        StopSpring();
                    }
                    lastZone = zone;
                }
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(10));
            }
            StopSpring();
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
    {
        std::lock_guard<std::mutex> lock(
            g_presetMutex);
        g_loadedPreset =
        FFBPreset{};
        g_presetTestState =
        PresetTestState{};
    }
    StopSpring();
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
void UpdatePresetTest()
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
            "Preset test ignored: "
            "FFB device unavailable.");
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
    bool expected =
    false;
    if (g_presetTestRunning.compare_exchange_strong(
        expected,
        true,
        std::memory_order_acq_rel))
    {
        if (g_presetTestThread.joinable())
        {
            g_presetTestThread.join();
        }
        g_presetTestThread =
        std::thread(
            PresetTestThreadProc);
    }
    Log(
        "Preset test enabled: spring forcefield "
        "zones are now position-aware.");
    Log(
        "Preset zone tracking started.");
}
int FindForceFieldAtPosition(
    LONG x,
    LONG y)
{
    std::lock_guard<std::mutex> lock(
        g_presetMutex);
    if (g_loadedPreset.forceFields.empty())
        return -1;
    for (size_t i = 0;
        i < g_loadedPreset.forceFields.size();
        ++i)
    {
        const ForceField& field =
        g_loadedPreset.forceFields[i];
        if (field.vertices.empty())
            continue;
        LONG minX = field.vertices[0].x;
        LONG maxX = field.vertices[0].x;
        LONG minY = field.vertices[0].y;
        LONG maxY = field.vertices[0].y;
        for (const auto& vertex :
            field.vertices)
        {
            minX =
            std::min(minX, vertex.x);
            maxX =
            std::max(maxX, vertex.x);
            minY =
            std::min(minY, vertex.y);
            maxY =
            std::max(maxY, vertex.y);
        }
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
    g_presetTestRunning.store(
        false,
        std::memory_order_release);
    if (g_presetTestThread.joinable())
    {
        g_presetTestThread.join();
    }
    StopSpring();
    Log(
        "Preset zone tracking stopped.");
}
} // namespace MultiFFBJoy