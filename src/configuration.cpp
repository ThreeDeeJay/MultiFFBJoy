#include "common.h"
#include "configuration.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
namespace MultiFFBJoy
{
    namespace
    {
        struct ConfigurationNode
        {
            std::string name;
            std::string preset;
            int depth = 0;
            ConfigurationNode* parent = nullptr;
            std::vector<ConfigurationNode> children;
        };
        ConfigurationNode g_configurationRoot;
        std::mutex g_configurationMutex;
        bool g_configurationLoaded = false;
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
        bool EqualsIgnoreCase(
            const std::string& a,
            const std::string& b)
        {
            return ToLower(a) == ToLower(b);
        }
        bool ParseNodeLine(
            const std::string& line,
            std::string& name,
            std::string& preset)
        {
            std::string text = Trim(line);
            if (text.empty())
                return false;
            if (text[0] == '#')
                return false;
            if (text.rfind("//", 0) == 0)
                return false;
            const size_t equals =
            text.find('=');
            if (equals == std::string::npos)
            {
                name = Trim(text);
                preset.clear();
                return !name.empty();
            }
            name =
            Trim(
                text.substr(
                    0,
                    equals));
            preset =
            Trim(
                text.substr(
                    equals + 1));
            return !name.empty();
        }
        int GetTabDepth(
            const std::string& line)
        {
            int depth = 0;
            for (const char c : line)
            {
                if (c == '\t')
                {
                    ++depth;
                    continue;
                }
        /*
         * Spaces before a node are deliberately not
         * treated as indentation. Configuration.txt
         * uses literal TAB characters.
         */
                if (c == ' ')
                    continue;
                break;
            }
            return depth;
        }
        std::string StripIndent(
            const std::string& line)
        {
            size_t position = 0;
            while (
                position < line.size() &&
                (line[position] == '\t' ||
                   line[position] == ' '))
            {
                ++position;
            }
            return line.substr(position);
        }
        ConfigurationNode* FindChild(
            ConfigurationNode& parent,
            const std::string& name)
        {
            for (auto& child : parent.children)
            {
                if (EqualsIgnoreCase(
                    child.name,
                    name))
                {
                    return &child;
                }
            }
            return nullptr;
        }
        const ConfigurationNode* FindChild(
            const ConfigurationNode& parent,
            const std::string& name)
        {
            for (const auto& child : parent.children)
            {
                if (EqualsIgnoreCase(
                    child.name,
                    name))
                {
                    return &child;
                }
            }
            return nullptr;
        }
        bool ParseConfigurationFile(
            const std::filesystem::path& path,
            ConfigurationNode& root)
        {
            std::ifstream file(
                path,
                std::ios::in);
            if (!file)
            {
                Logf(
                    "Configuration file could not be opened: %s",
                    path.string().c_str());
                return false;
            }
            root = ConfigurationNode{};
            root.name = "ROOT";
            root.depth = -1;
            std::vector<ConfigurationNode*> stack;
            stack.push_back(&root);
            std::string line;
            size_t lineNumber = 0;
            while (std::getline(file, line))
            {
                ++lineNumber;
                if (line.empty())
                    continue;
                const int depth =
                GetTabDepth(line);
                const std::string text =
                StripIndent(line);
                std::string name;
                std::string preset;
                if (!ParseNodeLine(
                    text,
                    name,
                    preset))
                {
                    continue;
                }
        /*
         * The tree is expected to increase by at most
         * one level at a time.
         */
                if (depth > static_cast<int>(stack.size()))
                {
                    Logf(
                        "Configuration.txt line %zu: invalid indentation.",
                        lineNumber);
                    return false;
                }
                while (
                    static_cast<int>(stack.size()) >
                    depth + 1)
                {
                    stack.pop_back();
                }
                ConfigurationNode* parent =
                stack.back();
                parent->children.emplace_back();
                ConfigurationNode& node =
                parent->children.back();
                node.name = name;
                node.preset = preset;
                node.depth = depth;
                node.parent = parent;
                stack.push_back(&node);
            }
            if (root.children.empty())
            {
                Log(
                    "Configuration.txt contains no entries.");
                return false;
            }
            return true;
        }
        const ConfigurationNode* MatchLevel(
            const ConfigurationNode* parent,
            const std::string& name)
        {
            if (parent == nullptr ||
                name.empty())
            {
                return nullptr;
            }
            return FindChild(
                *parent,
                name);
        }
        std::string EffectivePreset(
            const std::string& inherited,
            const ConfigurationNode* node)
        {
            if (node == nullptr)
                return inherited;
            if (!node->preset.empty())
                return node->preset;
            return inherited;
        }
        bool ResolveProfileInternal(
            const VehicleProfileRequest& request,
            ResolvedProfile& result)
        {
            const ConfigurationNode* profiles =
            FindChild(
                g_configurationRoot,
                "Profiles");
            if (profiles == nullptr)
            {
                Log(
                    "Configuration.txt has no Profiles root.");
                return false;
            }
            const ConfigurationNode* game =
            MatchLevel(
                profiles,
                request.game);
            if (game == nullptr)
            {
                return false;
            }
            std::string preset =
            EffectivePreset(
                {},
                game);
            const ConfigurationNode* type =
            MatchLevel(
                game,
                request.vehicleType);
            if (type != nullptr)
            {
                preset =
                EffectivePreset(
                    preset,
                    type);
            }
            if (type == nullptr)
            {
        /*
         * Vehicle entries can also be placed directly
         * beneath the game node if desired.
         */
                type = game;
            }
            const ConfigurationNode* vehicle =
            MatchLevel(
                type,
                request.vehicle);
            if (vehicle != nullptr)
            {
                preset =
                EffectivePreset(
                    preset,
                    vehicle);
                const ConfigurationNode* configuration =
                MatchLevel(
                    vehicle,
                    request.configuration);
                if (configuration != nullptr)
                {
                    preset =
                    EffectivePreset(
                        preset,
                        configuration);
                    result.sourcePath =
                    "Profiles > " +
                    request.game +
                    " > " +
                    request.vehicleType +
                    " > " +
                    request.vehicle +
                    " > " +
                    request.configuration;
                }
                else
                {
                    result.sourcePath =
                    "Profiles > " +
                    request.game +
                    " > " +
                    request.vehicleType +
                    " > " +
                    request.vehicle;
                }
            }
            else
            {
                result.sourcePath =
                "Profiles > " +
                request.game +
                " > " +
                request.vehicleType;
            }
            if (preset.empty())
            {
                return false;
            }
            result.presetName = preset;
            std::filesystem::path presetPath =
            std::filesystem::current_path() /
            "forcefields" /
            preset;
            if (presetPath.extension().empty())
            {
                presetPath += ".fff";
            }
            result.presetPath =
            presetPath;
            result.found = true;
            return true;
        }
    }
    std::filesystem::path
    GetConfigurationFilePath()
    {
        return std::filesystem::current_path() /
        "Configuration.txt";
    }
    bool LoadConfigurationFile()
    {
        const auto path =
        GetConfigurationFilePath();
        ConfigurationNode parsed;
        if (!ParseConfigurationFile(
            path,
            parsed))
        {
            std::lock_guard<std::mutex> lock(
                g_configurationMutex);
            g_configurationRoot =
            ConfigurationNode{};
            g_configurationLoaded =
            false;
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(
                g_configurationMutex);
            g_configurationRoot =
            std::move(parsed);
            g_configurationLoaded =
            true;
        }
        Logf(
            "Configuration loaded: %s",
            path.string().c_str());
        return true;
    }
    bool ResolveVehicleProfile(
        const VehicleProfileRequest& request,
        ResolvedProfile& result)
    {
        result = ResolvedProfile{};
        std::lock_guard<std::mutex> lock(
            g_configurationMutex);
        if (!g_configurationLoaded)
            return false;
        return ResolveProfileInternal(
            request,
            result);
    }
    bool LoadResolvedVehicleProfile(
        const VehicleProfileRequest& request)
    {
    /*
     * Always reload. This is intentional:
     * Configuration.txt may have been edited while
     * BeamNG is running.
     */
        if (!LoadConfigurationFile())
        {
            Log(
                "Profile resolution aborted: "
                "Configuration.txt could not be loaded.");
            return false;
        }
        ResolvedProfile profile;
        if (!ResolveVehicleProfile(
            request,
            profile))
        {
            Logf(
                "No configured FFB profile for "
                "game=\"%s\" type=\"%s\" vehicle=\"%s\" "
                "configuration=\"%s\".",
                request.game.c_str(),
                request.vehicleType.c_str(),
                request.vehicle.c_str(),
                request.configuration.c_str());
            return false;
        }
        Logf(
            "Resolved FFB profile: %s",
            profile.presetName.c_str());
        Logf(
            "Profile source: %s",
            profile.sourcePath.c_str());
        if (!std::filesystem::exists(
            profile.presetPath))
        {
            Logf(
                "Resolved preset does not exist: %s",
                profile.presetPath.string().c_str());
            return false;
        }
    /*
     * Loading the .fff replaces the currently loaded
     * forcefield. The existing preset tracking system
     * remains responsible for applying its zones.
     */
        if (!LoadForceFieldPreset(
            profile.presetPath))
        {
            Logf(
                "Failed to load resolved preset: %s",
                profile.presetPath.string().c_str());
            return false;
        }
        Logf(
            "Vehicle profile active: %s",
            profile.presetName.c_str());
        return true;
    }
}