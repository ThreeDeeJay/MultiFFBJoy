#include "common.h"
#include <cctype>
#include <cstring>
#include <sstream>
namespace MultiFFBJoy
{
    namespace
    {
        std::string Trim(
            const std::string& value)
        {
            const auto first =
            value.find_first_not_of(" \t\r\n");
            if (first == std::string::npos)
            {
                return {};
            }
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
            const std::string trimmed =
            Trim(value);
            if (trimmed.empty())
            {
                return false;
            }
            try
            {
                size_t used = 0;
                const long parsed =
                std::stol(
                    trimmed,
                    &used,
                    10);
                if (used != trimmed.size())
                {
                    return false;
                }
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
            return line.rfind(
                "FORCEFIELD NAME=",
                0) == 0;
        }
        bool IsSeparator(
            const std::string& line)
        {
            if (line.empty())
            {
                return false;
            }
            for (const char ch : line)
            {
                if (ch != '*')
                {
                    return false;
                }
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
                    "Could not open forcefield file: %s",
                    path.string().c_str());
                return false;
            }
            ForceField current;
            bool haveCurrent = false;
            std::string line;
            while (std::getline(file, line))
            {
                line = Trim(line);
                if (line.empty())
                {
                    continue;
                }
                if (IsSeparator(line))
                {
                    continue;
                }
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
                {
                    continue;
                }
// FORCEFIELD TYPE
                ParseIntegerField(
                    line,
                    "FORCEFIELD TYPE",
                    current.type);
// FORCEFIELD SHAPE TYPE
                ParseIntegerField(
                    line,
                    "FORCEFIELD SHAPE TYPE",
                    current.shapeType);
// FORCEFIELD CENTER
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
// Vertex records
                int vertexIndex = -1;
                if (ParseIntegerField(
                    line,
                    "FORCEFIELD VERTEX INDEX",
                    vertexIndex))
                {
                    if (vertexIndex >= 0)
                    {
                        const size_t requiredSize =
                        static_cast<size_t>(
                            vertexIndex) +
                        1;
                        if (current.vertices.size() <
                            requiredSize)
                        {
                            current.vertices.resize(
                                requiredSize);
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
// Force parameters
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
        {
            std::lock_guard<std::mutex> lock(
                g_presetMutex);
            Logf(
                "Loaded forcefield preset: %s",
                g_loadedPreset.path.string().c_str());
            Logf(
                "Forcefield preset contains %zu forcefield(s).",
                g_loadedPreset.forceFields.size());
            if (!g_loadedPreset.fileVersion.empty())
            {
                Logf(
                    "Forcefields file version: %s",
                    g_loadedPreset.fileVersion.c_str());
            }
        }
        return true;
    }
    void ClearForceFieldPreset()
    {
        StopPresetTest();
        std::lock_guard<std::mutex> lock(
            g_presetMutex);
        g_loadedPreset =
        FFBPreset{};
        g_presetTestState =
        PresetTestState{};
        Log(
            "Forcefield preset cleared.");
    }
    bool IsForceFieldPresetLoaded()
    {
        std::lock_guard<std::mutex> lock(
            g_presetMutex);
        return
        !g_loadedPreset.path.empty() &&
        !g_loadedPreset.forceFields.empty();
    }
    std::filesystem::path
    GetLoadedForceFieldPresetPath()
    {
        std::lock_guard<std::mutex> lock(
            g_presetMutex);
        return g_loadedPreset.path;
    }
    std::vector<PresetInfo>
    EnumerateForceFieldPresets()
    {
        std::vector<PresetInfo> result;
        const std::filesystem::path directory =
        std::filesystem::current_path() /
        "forcefields";
        std::error_code ec;
        if (!std::filesystem::exists(
            directory,
            ec))
        {
            Logf(
                "Forcefields directory does not exist: %s",
                directory.string().c_str());
            return result;
        }
        for (const auto& entry :
            std::filesystem::directory_iterator(
                directory,
                ec))
        {
            if (ec)
            {
                break;
            }
            if (!entry.is_regular_file(ec))
            {
                continue;
            }
            const auto extension =
            entry.path().extension().wstring();
            if (_wcsicmp(
                extension.c_str(),
                L".fff") != 0)
            {
                continue;
            }
            PresetInfo info;
            info.path =
            entry.path();
            info.displayName =
            entry.path().filename().wstring();
            result.push_back(
                std::move(info));
        }
        std::sort(
            result.begin(),
            result.end(),
            [](const PresetInfo& a,
                const PresetInfo& b)
            {
                return _wcsicmp(
                    a.displayName.c_str(),
                    b.displayName.c_str()) < 0;
            });
        return result;
    }
    void StopPresetTest()
    {
        if (g_springEffect != nullptr)
        {
            StopSpring();
        }
        else
        {
            std::lock_guard<std::mutex> lock(
                g_stateMutex);
            g_state.springStrength = 0.0f;
            g_state.springPersistent = false;
        }
        {
            std::lock_guard<std::mutex> lock(
                g_presetMutex);
            g_presetTestState.enabled = false;
            g_presetTestState.activeForceField = -1;
            g_presetTestState.normalizedX = 0.0f;
            g_presetTestState.normalizedY = 0.0f;
        }
        UpdateStatus();
    }
    void UpdatePresetTest()
    {
        ForceField selectedField;
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
// Stage 1: test the first forcefield
// contained in the selected .fff file.
            selectedField =
            g_loadedPreset.forceFields.front();
            g_presetTestState.enabled = true;
            g_presetTestState.activeForceField = 0;
        }
        if (!EnsureFFBDeviceReady())
        {
            Log(
                "Preset test ignored: "
                "FFB device unavailable.");
            return;
        }
        if (!SetSpringForceField(
            selectedField))
        {
            Log(
                "Preset test failed.");
            return;
        }
        Logf(
            "Preset test active: \"%s\".",
            selectedField.name.c_str());
    }
}