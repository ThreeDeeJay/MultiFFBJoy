#include "common.h"
#include <cctype>
#include <cstring>
#include <sstream>
namespace MultiFFBJoy
{
    std::mutex g_presetMutex;
    FFBPreset g_loadedPreset;
    namespace
    {
        std::atomic<bool> g_presetTestRunning{false};
        std::thread g_presetTestThread;
        std::string Trim(const std::string& value)
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
        std::string Unquote(const std::string& value)
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
            if (!ParseLong(value, temp))
                return false;
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
            return line.rfind(
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
                    current = ForceField{};
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
                        if (current.vertices.size() <=
                            static_cast<size_t>(
                                vertexIndex))
                        {
                            current.vertices.resize(
                                static_cast<size_t>(
                                    vertexIndex) + 1);
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
            LONG x,
            LONG y,
            const ForceField& field)
        {
            if (field.vertices.size() < 3)
                return false;
/*
* Standard 2D point-in-polygon test.
*
* The .fff coordinates are already in the same
* general [-10000,10000] coordinate space used by
* DirectInput, so no transformation is required here.
*/
            bool inside = false;
            const size_t count =
            field.vertices.size();
            for (size_t i = 0, j = count - 1;
                i < count;
                j = i++)
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
                if (!crosses)
                    continue;
                const double intersectionX =
                static_cast<double>(xj - xi) *
                static_cast<double>(y - yi) /
                static_cast<double>(yj - yi) +
                static_cast<double>(xi);
                if (static_cast<double>(x) <
                    intersectionX)
                {
                    inside = !inside;
                }
            }
            return inside;
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
* Later forcefields are allowed to overlap earlier
* ones. For this stage the first matching field wins,
* which matches the ordering used by the supplied
* Automatic/PRND preset.
*/
            for (size_t i = 0;
                i < g_loadedPreset.forceFields.size();
                ++i)
            {
                const ForceField& field =
                g_loadedPreset.forceFields[i];
/*
* This stage intentionally only supports spring
* forcefields.
*
* The supplied Automatic/PRND file uses:
*
*     FORCEFIELD TYPE=1
*     FORCE TYPE=1
*
* Constant-force support is intentionally deferred.
*/
                if (field.forceType != 1)
                    continue;
                if (PointInsideForceField(
                    x,
                    y,
                    field))
                {
                    return static_cast<int>(i);
                }
            }
            return -1;
        }
        bool ReadFFBJoystickPosition(
            LONG& x,
            LONG& y)
        {
            IDirectInputDevice8W* device = nullptr;
            DWORD xOffset = DIJOFS_X;
            DWORD yOffset = DIJOFS_Y;
            {
                std::lock_guard<std::mutex> lock(
                    g_stateMutex);
                device = g_ffbDevice;
                xOffset = g_state.xAxisOffset;
                yOffset = g_state.yAxisOffset;
            }
            if (device == nullptr)
                return false;
            DIJOYSTATE2 state{};
            HRESULT hr =
            device->GetDeviceState(
                sizeof(state),
                &state);
            if (FAILED(hr))
            {
/*
* The normal watchdog/reacquire mechanism owns
* acquisition recovery. We simply report that
* the current position could not be read.
*/
                return false;
            }
/*
* DirectInput's common X/Y offsets are DIJOFS_X/Y.
*
* The selected device currently uses those logical
* axis offsets. Keep the state lookup explicit rather
* than assuming every future device has identical
* physical axes.
*/
            if (xOffset == DIJOFS_X)
            {
                x = state.lX;
            }
            else
            {
                x = state.lX;
            }
            if (yOffset == DIJOFS_Y)
            {
                y = state.lY;
            }
            else
            {
                y = state.lY;
            }
            return true;
        }
        bool GetCurrentPresetForceField(
            int index,
            ForceField& result)
        {
            std::lock_guard<std::mutex> lock(
                g_presetMutex);
            if (index < 0 ||
                static_cast<size_t>(index) >=
                g_loadedPreset.forceFields.size())
            {
                return false;
            }
            result =
            g_loadedPreset.forceFields[
                static_cast<size_t>(index)];
            return true;
        }
        void PresetTestThreadMain()
        {
            int previousField = -1;
            LONG previousX = 0;
            LONG previousY = 0;
            bool havePreviousPosition = false;
            Log(
                "Preset zone tracking started.");
            while (g_presetTestRunning &&
                g_running)
            {
/*
* Make sure a preset is still loaded.
*/
                {
                    std::lock_guard<std::mutex> lock(
                        g_presetMutex);
                    if (g_loadedPreset.forceFields.empty())
                        break;
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
/*
* Avoid repeatedly logging identical positions.
* The position itself is not used to update the
* effect; only a zone transition is.
*/
                if (!havePreviousPosition ||
                    x != previousX ||
                    y != previousY)
                {
                    previousX = x;
                    previousY = y;
                    havePreviousPosition = true;
                }
                const int fieldIndex =
                FindForceFieldAtPosition(
                    x,
                    y);
                if (fieldIndex != previousField)
                {
                    if (fieldIndex < 0)
                    {
                        Logf(
                            "Preset zone: none "
                            "(stick X=%ld Y=%ld).",
                            x,
                            y);
/*
* Outside all zones, stop the spring
* rather than leaving the previous gear
* force active.
*/
                        StopSpring();
                    }
                    else
                    {
                        ForceField selected{};
                        if (GetCurrentPresetForceField(
                            fieldIndex,
                            selected))
                        {
                            Logf(
                                "Preset zone: \"%s\" "
                                "(index=%d, X=%ld Y=%ld).",
                                selected.name.c_str(),
                                fieldIndex,
                                x,
                                y);
                            if (!SetSpringForceField(
                                selected))
                            {
                                Logf(
                                    "Failed to apply "
                                    "spring zone \"%s\".",
                                    selected.name.c_str());
                            }
                        }
                    }
                    previousField = fieldIndex;
                }
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(5));
            }
/*
* Do not leave the last gear's spring active after
* the preset test ends.
*/
            StopSpring();
            Log(
                "Preset zone tracking stopped.");
        }
    }
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
                    "  [%zu] \"%s\": type=%d "
                    "forceType=%d vertices=%zu "
                    "power=(%ld,%ld) offset=(%ld,%ld)",
                    i,
                    field.name.c_str(),
                    field.type,
                    field.forceType,
                    field.vertices.size(),
                    field.powerX,
                    field.powerY,
                    field.offsetX,
                    field.offsetY);
            }
        }
        return true;
    }
    void ClearForceFieldPreset()
    {
        g_presetTestRunning = false;
        if (g_presetTestThread.joinable())
        {
            if (g_presetTestThread.get_id() !=
                std::this_thread::get_id())
            {
                g_presetTestThread.join();
            }
        }
        StopSpring();
        {
            std::lock_guard<std::mutex> lock(
                g_presetMutex);
            g_loadedPreset =
            FFBPreset{};
        }
        Log(
            "Forcefield preset cleared.");
    }
    bool IsForceFieldPresetLoaded()
    {
        std::lock_guard<std::mutex> lock(
            g_presetMutex);
        return
        !g_loadedPreset.forceFields.empty();
    }
    std::filesystem::path
    GetLoadedForceFieldPresetPath()
    {
        std::lock_guard<std::mutex> lock(
            g_presetMutex);
        return g_loadedPreset.path;
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
    void StopPresetTest()
    {
        g_presetTestRunning = false;
        if (g_presetTestThread.joinable())
        {
            if (g_presetTestThread.get_id() !=
                std::this_thread::get_id())
            {
                g_presetTestThread.join();
            }
        }
        StopSpring();
        {
            std::lock_guard<std::mutex> lock(
                g_presetMutex);
            g_presetTestState =
            PresetTestState{};
        }
        UpdateStatus();
        Log(
            "Preset test stopped.");
    }
    void UpdatePresetTest()
    {
/*
* Restart the zone tracker if it was already running.
* This is important when the user loads a different
* .fff while another preset is active.
*/
        g_presetTestRunning = false;
        if (g_presetTestThread.joinable())
        {
            if (g_presetTestThread.get_id() !=
                std::this_thread::get_id())
            {
                g_presetTestThread.join();
            }
        }
        {
            std::lock_guard<std::mutex> lock(
                g_presetMutex);
            if (g_loadedPreset.forceFields.empty())
            {
                Log(
                    "Preset test ignored: "
                    "no forcefield preset is loaded.");
                return;
            }
        }
        if (!EnsureFFBDeviceReady())
        {
            Log(
                "Preset test ignored: "
                "FFB device unavailable.");
            return;
        }
/*
* Start the continuous joystick-position monitor.
*/
        g_presetTestRunning = true;
        g_presetTestThread =
        std::thread(
            PresetTestThreadMain);
        Log(
            "Preset test enabled: "
            "spring forcefield zones are now position-aware.");
    }
}