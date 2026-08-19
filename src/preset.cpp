#include "common.h"
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
namespace MultiFFBJoy
{
    namespace
    {
        namespace fs = std::filesystem;
        constexpr LONG FFB_MIN = -10000;
        constexpr LONG FFB_MAX = 10000;
        const char* PRESET_DIRECTORY = "presets";
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
        std::string ToLower(
            std::string value)
        {
            std::transform(
                value.begin(),
                value.end(),
                value.begin(),
                [](unsigned char c)
                {
                    return static_cast<char>(
                        std::tolower(c));
                });
            return value;
        }
        bool StartsWith(
            const std::string& value,
            const char* prefix)
        {
            const size_t length =
            std::strlen(prefix);
            return value.size() >= length &&
            value.compare(
                0,
                length,
                prefix) == 0;
        }
        std::string ValueAfterEquals(
            const std::string& line)
        {
            const size_t equals =
            line.find('=');
            if (equals == std::string::npos)
                return {};
            return Trim(
                line.substr(equals + 1));
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
            const std::string& line,
            LONG& destination)
        {
            const std::string value =
            ValueAfterEquals(line);
            if (value.empty())
                return false;
            try
            {
                size_t consumed = 0;
                const long long parsed =
                std::stoll(
                    value,
                    &consumed,
                    10);
                if (consumed != value.size())
                    return false;
                if (parsed <
                    static_cast<long long>(
                        std::numeric_limits<LONG>::min()) ||
                    parsed >
                    static_cast<long long>(
                        std::numeric_limits<LONG>::max()))
                {
                    return false;
                }
                destination =
                static_cast<LONG>(parsed);
                return true;
            }
            catch (...)
            {
                return false;
            }
        }
        bool ParseInt(
            const std::string& line,
            int& destination)
        {
            const std::string value =
            ValueAfterEquals(line);
            if (value.empty())
                return false;
            try
            {
                size_t consumed = 0;
                const long parsed =
                std::stol(
                    value,
                    &consumed,
                    10);
                if (consumed != value.size())
                    return false;
                destination =
                static_cast<int>(parsed);
                return true;
            }
            catch (...)
            {
                return false;
            }
        }
        bool PointOnSegment(
            LONG x,
            LONG y,
            const POINT& a,
            const POINT& b)
        {
            const long long cross =
            static_cast<long long>(x - a.x) *
            static_cast<long long>(b.y - a.y) -
            static_cast<long long>(y - a.y) *
            static_cast<long long>(b.x - a.x);
            if (cross != 0)
                return false;
            return
            x >= std::min(a.x, b.x) &&
            x <= std::max(a.x, b.x) &&
            y >= std::min(a.y, b.y) &&
            y <= std::max(a.y, b.y);
        }
        bool PointInPolygon(
            LONG x,
            LONG y,
            const std::vector<POINT>& vertices)
        {
            if (vertices.size() < 3)
                return false;
/*
* Treat points exactly on a boundary as belonging to
* the forcefield. This makes adjacent FFShifter zones
* deterministic rather than leaving a one-coordinate
* gap between them.
*/
            for (size_t i = 0;
                i < vertices.size();
                ++i)
            {
                const POINT& a =
                vertices[i];
                const POINT& b =
                vertices[
                    (i + 1) %
                    vertices.size()];
                if (PointOnSegment(
                    x,
                    y,
                    a,
                    b))
                {
                    return true;
                }
            }
            bool inside = false;
            for (size_t i = 0,
                j = vertices.size() - 1;
                i < vertices.size();
                j = i++)
            {
                const LONG xi =
                vertices[i].x;
                const LONG yi =
                vertices[i].y;
                const LONG xj =
                vertices[j].x;
                const LONG yj =
                vertices[j].y;
                const bool crosses =
                ((yi > y) != (yj > y));
                if (!crosses)
                    continue;
                const double intersectionX =
                static_cast<double>(
                    xj - xi) *
                static_cast<double>(
                    y - yi) /
                static_cast<double>(
                    yj - yi) +
                static_cast<double>(xi);
                if (static_cast<double>(x) <
                    intersectionX)
                {
                    inside = !inside;
                }
            }
            return inside;
        }
        int EnsureVertex(
            ForceField& field,
            size_t index)
        {
            if (index >= field.vertices.size())
            {
                field.vertices.resize(
                    index + 1);
                field.vertexZ.resize(
                    index + 1,
                    0);
            }
            return static_cast<int>(index);
        }
    }
    bool LoadFFBPreset(
        const std::string& path)
    {
        std::ifstream file(path);
        if (!file.is_open())
        {
            Logf(
                "Could not open FFB preset: %s",
                path.c_str());
            return false;
        }
        FFBPreset preset;
        preset.path = path;
        try
        {
            preset.name =
            fs::path(path).stem().string();
        }
        catch (...)
        {
            preset.name = path;
        }
        ForceField currentField;
        bool haveCurrentField = false;
        int currentVertexIndex = -1;
        std::string line;
        while (std::getline(file, line))
        {
            line = Trim(line);
            if (line.empty())
                continue;
            if (line[0] == '*')
                continue;
            if (StartsWith(
                line,
                "FORCEFIELDS FILE VERSION="))
            {
                preset.fileVersion =
                Unquote(
                    ValueAfterEquals(line));
                continue;
            }
            if (StartsWith(
                line,
                "NO OF FORCEFIELDS="))
            {
/*
* This value is informational.
*
* We intentionally don't depend on it because the
* individual FORCEFIELD NAME records are authoritative.
*/
                continue;
            }
            if (StartsWith(
                line,
                "FORCEFIELD NAME="))
            {
                if (haveCurrentField)
                {
                    preset.forceFields.push_back(
                        std::move(currentField));
                }
                currentField = ForceField{};
                currentVertexIndex = -1;
                currentField.name =
                Unquote(
                    ValueAfterEquals(line));
                haveCurrentField = true;
                continue;
            }
            if (!haveCurrentField)
                continue;
            if (StartsWith(
                line,
                "FORCEFIELD TYPE="))
            {
                ParseInt(
                    line,
                    currentField.type);
                continue;
            }
            if (StartsWith(
                line,
                "FORCEFIELD SHAPE TYPE="))
            {
                ParseInt(
                    line,
                    currentField.shapeType);
                continue;
            }
            if (StartsWith(
                line,
                "FORCEFIELD CENTER X="))
            {
                ParseLong(
                    line,
                    currentField.centerX);
                continue;
            }
            if (StartsWith(
                line,
                "FORCEFIELD CENTER Y="))
            {
                ParseLong(
                    line,
                    currentField.centerY);
                continue;
            }
            if (StartsWith(
                line,
                "FORCEFIELD CENTER Z="))
            {
                ParseLong(
                    line,
                    currentField.centerZ);
                continue;
            }
            if (StartsWith(
                line,
                "FORCEFIELD NO OF VERTICES="))
            {
                int count = 0;
                if (ParseInt(line, count) &&
                    count > 0)
                {
                    currentField.vertices.resize(
                        static_cast<size_t>(count));
                    currentField.vertexZ.resize(
                        static_cast<size_t>(count),
                        0);
                }
                continue;
            }
            if (StartsWith(
                line,
                "FORCEFIELD VERTEX INDEX="))
            {
                int index = -1;
                if (ParseInt(line, index) &&
                    index >= 0)
                {
                    currentVertexIndex = index;
                    EnsureVertex(
                        currentField,
                        static_cast<size_t>(
                            index));
                }
                continue;
            }
            if (StartsWith(
                line,
                "FORCEFIELD VERTEX X="))
            {
                if (currentVertexIndex >= 0)
                {
                    LONG value = 0;
                    if (ParseLong(line, value))
                    {
                        EnsureVertex(
                            currentField,
                            static_cast<size_t>(
                                currentVertexIndex));
                        currentField.vertices[
                            currentVertexIndex].x =
                        value;
                    }
                }
                continue;
            }
            if (StartsWith(
                line,
                "FORCEFIELD VERTEX Y="))
            {
                if (currentVertexIndex >= 0)
                {
                    LONG value = 0;
                    if (ParseLong(line, value))
                    {
                        EnsureVertex(
                            currentField,
                            static_cast<size_t>(
                                currentVertexIndex));
                        currentField.vertices[
                            currentVertexIndex].y =
                        value;
                    }
                }
                continue;
            }
            if (StartsWith(
                line,
                "FORCEFIELD VERTEX Z="))
            {
                if (currentVertexIndex >= 0)
                {
                    LONG value = 0;
                    if (ParseLong(line, value))
                    {
                        EnsureVertex(
                            currentField,
                            static_cast<size_t>(
                                currentVertexIndex));
                        currentField.vertexZ[
                            currentVertexIndex] =
                        value;
                    }
                }
                continue;
            }
            if (StartsWith(
                line,
                "FORCE TYPE="))
            {
                ParseInt(
                    line,
                    currentField.forceType);
                continue;
            }
            if (StartsWith(
                line,
                "FORCE POWER X="))
            {
                ParseLong(
                    line,
                    currentField.powerX);
                continue;
            }
            if (StartsWith(
                line,
                "FORCE POWER Y="))
            {
                ParseLong(
                    line,
                    currentField.powerY);
                continue;
            }
            if (StartsWith(
                line,
                "FORCE OFFSET X="))
            {
                ParseLong(
                    line,
                    currentField.offsetX);
                continue;
            }
            if (StartsWith(
                line,
                "FORCE OFFSET Y="))
            {
                ParseLong(
                    line,
                    currentField.offsetY);
                continue;
            }
/*
* KEY*, FORCE PRIMARY KEY INDEX,
* FORCE SECONDARY KEY INDEX,
* SEQUENTIAL*, etc. are intentionally ignored.
*/
        }
        if (haveCurrentField)
        {
            preset.forceFields.push_back(
                std::move(currentField));
        }
        if (preset.forceFields.empty())
        {
            Logf(
                "FFB preset contains no forcefields: %s",
                path.c_str());
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(
                g_presetMutex);
            g_loadedPreset =
            std::move(preset);
            g_presetTestState =
            PresetTestState{};
        }
        Logf(
            "Loaded FFB preset: %s (%zu forcefield(s)).",
            path.c_str(),
            g_loadedPreset.forceFields.size());
        return true;
    }
    bool ReloadFFBPresets()
    {
        std::vector<PresetInfo> presets;
        std::error_code error;
        const fs::path directory =
        fs::path(PRESET_DIRECTORY);
        if (!fs::exists(
            directory,
            error))
        {
            if (!fs::create_directories(
                directory,
                error))
            {
                Logf(
                    "Could not create preset directory: %s",
                    directory.string().c_str());
                return false;
            }
        }
        for (const auto& entry :
            fs::directory_iterator(
                directory,
                error))
        {
            if (error)
                break;
            if (!entry.is_regular_file())
                continue;
            std::string extension =
            entry.path()
            .extension()
            .string();
            std::transform(
                extension.begin(),
                extension.end(),
                extension.begin(),
                [](unsigned char c)
                {
                    return static_cast<char>(
                        std::tolower(c));
                });
            if (extension != ".fff")
                continue;
            PresetInfo info;
            info.name =
            entry.path().stem().string();
            info.path =
            entry.path().string();
            presets.push_back(
                std::move(info));
        }
        std::sort(
            presets.begin(),
            presets.end(),
            [](const PresetInfo& a,
                const PresetInfo& b)
            {
                return ToLower(a.name) <
                ToLower(b.name);
            });
        {
            std::lock_guard<std::mutex> lock(
                g_presetMutex);
            g_availablePresets =
            std::move(presets);
        }
        Logf(
            "Found %zu FFB preset(s).",
            g_availablePresets.size());
        return true;
    }
    const std::vector<PresetInfo>&
    GetAvailablePresets()
    {
        return g_availablePresets;
    }
    std::string GetForceFieldAction(
        const ForceField& field)
    {
        const std::string name =
        ToLower(
            Trim(field.name));
/*
* These are semantic BeamNG/game actions, not keyboard keys.
*
* Additional aliases can be added here as we encounter
* FFShifter preset naming conventions.
*/
        if (name == "parking" ||
            name == "park")
        {
            return "parking";
        }
        if (name == "reverse" ||
            name == "rev")
        {
            return "reverse";
        }
        if (name == "neutral" ||
            name == "n")
        {
            return "neutral";
        }
        if (name == "drive" ||
            name == "d")
        {
            return "drive";
        }
        if (name == "low")
        {
            return "low";
        }
        if (name == "1" ||
            name == "1st" ||
            name == "first")
        {
            return "1";
        }
        if (name == "2" ||
            name == "2nd" ||
            name == "second")
        {
            return "2";
        }
        if (name == "3" ||
            name == "3rd" ||
            name == "third")
        {
            return "3";
        }
        if (name == "4" ||
            name == "4th" ||
            name == "fourth")
        {
            return "4";
        }
        if (name == "5" ||
            name == "5th" ||
            name == "fifth")
        {
            return "5";
        }
        if (name == "6" ||
            name == "6th" ||
            name == "sixth")
        {
            return "6";
        }
        if (name == "7" ||
            name == "7th" ||
            name == "seventh")
        {
            return "7";
        }
        if (name == "8" ||
            name == "8th" ||
            name == "eighth")
        {
            return "8";
        }
        return {};
    }
    int FindForceFieldAtPosition(
        const FFBPreset& preset,
        LONG x,
        LONG y)
    {
/*
* Test in file order.
*
* FFShifter presets normally define adjacent, non-overlapping
* zones. If two polygons overlap, the first one wins.
*/
        for (size_t i = 0;
            i < preset.forceFields.size();
            ++i)
        {
            const ForceField& field =
            preset.forceFields[i];
            if (PointInPolygon(
                x,
                y,
                field.vertices))
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    }
    bool TestFFBPreset(
        const std::string& path)
    {
        if (!LoadFFBPreset(path))
            return false;
        {
            std::lock_guard<std::mutex> lock(
                g_presetMutex);
            g_presetTestState.enabled = true;
            g_presetTestState.activeForceField = -1;
        }
        Logf(
            "FFB preset test enabled: %s",
            path.c_str());
        return true;
    }
    void StopFFBPresetTest()
    {
        {
            std::lock_guard<std::mutex> lock(
                g_presetMutex);
            g_presetTestState.enabled = false;
            g_presetTestState.activeForceField = -1;
            g_state.forceFieldName.clear();
            g_state.forceFieldAction.clear();
            g_state.presetName.clear();
        }
        StopSpring();
        Log("FFB preset test stopped.");
    }
    void UpdatePresetTest()
    {
        FFBPreset preset;
        bool enabled = false;
        int previousField = -1;
        {
            std::lock_guard<std::mutex> lock(
                g_presetMutex);
            enabled =
            g_presetTestState.enabled;
            if (!enabled)
                return;
            preset =
            g_loadedPreset;
            previousField =
            g_presetTestState.activeForceField;
        }
        if (preset.forceFields.empty())
            return;
        if (g_ffbDevice == nullptr)
            return;
        DIJOYSTATE2 joystick{};
        const HRESULT hr =
        g_ffbDevice->GetDeviceState(
            sizeof(joystick),
            &joystick);
        if (FAILED(hr))
            return;
        const LONG x =
        static_cast<LONG>(joystick.lX);
        const LONG y =
        static_cast<LONG>(joystick.lY);
        const int currentField =
        FindForceFieldAtPosition(
            preset,
            x,
            y);
        if (currentField == previousField)
            return;
        {
            std::lock_guard<std::mutex> lock(
                g_presetMutex);
            g_presetTestState.activeForceField =
            currentField;
        }
        if (currentField < 0)
        {
            Logf(
                "Preset position X=%ld Y=%ld: "
                "no forcefield.",
                x,
                y);
            {
                std::lock_guard<std::mutex> lock(
                    g_stateMutex);
                g_state.forceFieldName.clear();
                g_state.forceFieldAction.clear();
            }
            StopSpring();
            return;
        }
        const ForceField& field =
        preset.forceFields[
            static_cast<size_t>(
                currentField)];
        const std::string action =
        GetForceFieldAction(field);
        {
            std::lock_guard<std::mutex> lock(
                g_stateMutex);
            g_state.presetName =
            preset.name;
            g_state.forceFieldName =
            field.name;
            g_state.forceFieldAction =
            action;
        }
        Logf(
            "Preset position X=%ld Y=%ld: "
            "forcefield=\"%s\", action=\"%s\".",
            x,
            y,
            field.name.c_str(),
            action.empty()
            ? "(unmapped)"
            : action.c_str());
        if (field.type == 1 &&
            field.forceType == 1)
        {
            if (!SetSpringForceField(field))
            {
                Logf(
                    "Failed to apply spring forcefield: %s",
                    field.name.c_str());
            }
            return;
        }
        if (field.type == 0 &&
            field.forceType == 0)
        {
/*
* Stage 1 parser support is present, but constant-force
* interpretation will be implemented alongside the
* finalized FFShifter constant-force semantics.
*/
            Logf(
                "Constant forcefield selected: %s. "
                "Constant-force preset testing is not yet implemented.",
                field.name.c_str());
            StopSpring();
            return;
        }
        Logf(
            "Unsupported forcefield type: %s "
            "(field type=%d, force type=%d).",
            field.name.c_str(),
            field.type,
            field.forceType);
        StopSpring();
    }
}