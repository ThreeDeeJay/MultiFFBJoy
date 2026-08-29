#include "configuration.h"

#include <cctype>
#include <sstream>

namespace MultiFFBJoy
{
namespace
{
struct ConfigurationNode
{
    std::string name;
    std::string preset;
    std::vector<ConfigurationNode> children;
};

std::mutex g_configurationMutex;
ConfigurationNode g_configurationRoot;
bool g_configurationLoaded = false;

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
        value = value.substr(1, value.size() - 2);
    return value;
}

bool EqualsIgnoreCase(const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
    {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb))
            return false;
    }
    return true;
}

int IndentDepth(const std::string& line)
{
    int depth = 0;
    size_t i = 0;
    while (i < line.size())
    {
        if (line[i] == '\t')
        {
            ++depth;
            ++i;
        }
        else if (line[i] == ' ')
        {
            ++i;
        }
        else
        {
            break;
        }
    }
    return depth;
}

bool ParseNode(const std::string& raw, std::string& name, std::string& preset)
{
    const std::string line = Trim(raw);
    if (line.empty() || line[0] == '#' || line.rfind("//", 0) == 0)
        return false;

    // Find the first '=' outside double quotes.
    bool quoted = false;
    size_t separator = std::string::npos;
    for (size_t i = 0; i < line.size(); ++i)
    {
        if (line[i] == '"')
            quoted = !quoted;
        else if (line[i] == '=' && !quoted)
        {
            separator = i;
            break;
        }
    }

    if (separator == std::string::npos)
    {
        name = Unquote(line);
        preset.clear();
    }
    else
    {
        name = Unquote(line.substr(0, separator));
        preset = Unquote(line.substr(separator + 1));
    }
    return !name.empty();
}

const ConfigurationNode* FindChild(const ConfigurationNode& parent,
                                   const std::string& name)
{
    if (name.empty())
        return nullptr;
    for (const auto& child : parent.children)
        if (EqualsIgnoreCase(child.name, name))
            return &child;
    return nullptr;
}

bool ParseFile(const std::filesystem::path& path, ConfigurationNode& root)
{
    std::ifstream file(path);
    if (!file)
    {
        Logf("Configuration file could not be opened: %s", path.string().c_str());
        return false;
    }

    root = ConfigurationNode{};
    root.name = "ROOT";
    std::vector<ConfigurationNode*> stack{&root};
    std::string line;
    size_t lineNumber = 0;

    while (std::getline(file, line))
    {
        ++lineNumber;
        if (Trim(line).empty())
            continue;

        const int depth = IndentDepth(line);
        if (depth > static_cast<int>(stack.size()))
        {
            Logf("Configuration.txt line %zu: indentation skips a level.", lineNumber);
            return false;
        }

        std::string name;
        std::string preset;
        if (!ParseNode(line, name, preset))
            continue;

        while (static_cast<int>(stack.size()) > depth + 1)
            stack.pop_back();

        ConfigurationNode* parent = stack.back();
        parent->children.push_back(ConfigurationNode{});
        ConfigurationNode& node = parent->children.back();
        node.name = std::move(name);
        node.preset = std::move(preset);
        stack.push_back(&node);
    }

    if (root.children.empty())
    {
        Log("Configuration.txt contains no entries.");
        return false;
    }
    return true;
}

void ApplyPreset(const ConfigurationNode* node, std::string& preset,
                 std::string& source, const std::string& path)
{
    if (!node)
        return;
    if (!node->preset.empty())
    {
        preset = node->preset;
        source = path;
    }
}

} // namespace

std::filesystem::path GetApplicationDirectory()
{
    wchar_t buffer[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, std::size(buffer));
    if (length == 0 || length >= std::size(buffer))
        return std::filesystem::current_path();
    return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path GetConfigurationFilePath()
{
    return GetApplicationDirectory() / "Configuration.txt";
}

bool LoadConfigurationFile()
{
    ConfigurationNode parsed;
    const auto path = GetConfigurationFilePath();

    // Log the exact file path and its raw contents. This makes it unambiguous
    // which Configuration.txt the helper is using, regardless of its current
    // working directory.
    {
        std::ifstream debugFile(path);
        if (debugFile)
        {
            Logf("Configuration.txt path: %s", path.string().c_str());
            Log("Configuration.txt contents begin:");
            std::string debugLine;
            while (std::getline(debugFile, debugLine))
                Log(debugLine);
            Log("Configuration.txt contents end.");
        }
        else
        {
            Logf("Configuration.txt not found/openable at: %s", path.string().c_str());
        }
    }

    if (!ParseFile(path, parsed))
    {
        std::lock_guard<std::mutex> lock(g_configurationMutex);
        g_configurationRoot = {};
        g_configurationLoaded = false;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_configurationMutex);
        g_configurationRoot = std::move(parsed);
        g_configurationLoaded = true;
    }
    Logf("Configuration loaded: %s", path.string().c_str());
    return true;
}

bool ResolveVehicleProfile(const VehicleProfileRequest& request,
                           ResolvedProfile& result)
{
    result = {};
    std::lock_guard<std::mutex> lock(g_configurationMutex);
    if (!g_configurationLoaded)
        return false;

    const ConfigurationNode* profiles = FindChild(g_configurationRoot, "Profiles");
    const ConfigurationNode* game = profiles ? FindChild(*profiles, request.game) : nullptr;
    if (!game)
        return false;

    std::string preset;
    std::string source;
    auto apply = [&](const ConfigurationNode* node, const std::string& path)
    {
        ApplyPreset(node, preset, source, path);
    };

    apply(game, "Profiles > " + request.game);

    // Vehicle hierarchy establishes the general vehicle fallback.
    // Transmission-specific defaults are then useful for vehicles that do not
    // have a more specific override. Finally, the concrete vehicle/configuration
    // nodes are applied last so they always win.
    const ConfigurationNode* vehicleCategory = FindChild(*game, "Vehicle");
    const ConfigurationNode* vehicleType = nullptr;
    const ConfigurationNode* vehicle = nullptr;
    std::string vehiclePath;
    bool vehicleFromType = false;

    if (vehicleCategory)
    {
        apply(vehicleCategory, "Profiles > " + request.game + " > Vehicle");
        if (!request.vehicleType.empty())
        {
            vehicleType = FindChild(*vehicleCategory, request.vehicleType);
            if (vehicleType)
                apply(vehicleType, "Profiles > " + request.game + " > Vehicle > " + request.vehicleType);
            vehicle = vehicleType ? FindChild(*vehicleType, request.vehicle) : nullptr;
            vehicleFromType = vehicle != nullptr;
        }

        // Vehicle type is not always exposed by VLUA for every vehicle/mod.
        // When it is missing, search each type category for the concrete vehicle.
        if (!vehicle && !request.vehicle.empty())
        {
            for (const auto& candidateType : vehicleCategory->children)
            {
                if (EqualsIgnoreCase(candidateType.name, "Transmission"))
                    continue;
                if (const auto* candidateVehicle = FindChild(candidateType, request.vehicle))
                {
                    vehicleType = &candidateType;
                    apply(vehicleType, "Profiles > " + request.game + " > Vehicle > " + vehicleType->name);
                    vehicle = candidateVehicle;
                    vehicleFromType = true;
                    break;
                }
            }
        }

        // Also support a vehicle directly under Vehicle for future/legacy configs.
        if (!vehicle)
            vehicle = FindChild(*vehicleCategory, request.vehicle);

        if (vehicle)
        {
            if (vehicleFromType && vehicleType)
                vehiclePath = "Profiles > " + request.game + " > Vehicle > " + vehicleType->name + " > " + request.vehicle;
            else
                vehiclePath = "Profiles > " + request.game + " > Vehicle > " + request.vehicle;
        }
    }
    else
    {
        // Backward-compatible form: VehicleType directly under the game.
        vehicleType = FindChild(*game, request.vehicleType);
        if (vehicleType)
            apply(vehicleType, "Profiles > " + request.game + " > " + request.vehicleType);
        vehicle = vehicleType ? FindChild(*vehicleType, request.vehicle) : nullptr;
        if (vehicle)
            vehiclePath = "Profiles > " + request.game + " > " + request.vehicleType + " > " + request.vehicle;
    }

    // Transmission is a sibling category, but vehicle/configuration overrides
    // are deliberately more specific than transmission defaults.
    const ConfigurationNode* transmissionCategory = FindChild(*game, "Transmission");
    if (transmissionCategory)
    {
        // A category-level Transmission preset is a fallback. A concrete
        // transmission preset wins over it.
        apply(transmissionCategory,
              "Profiles > " + request.game + " > Transmission");
        if (!request.transmission.empty())
        {
            if (const auto* transmission =
                    FindChild(*transmissionCategory, request.transmission))
            {
                apply(transmission,
                      "Profiles > " + request.game + " > Transmission > " + request.transmission);
            }
        }
    }

    if (vehicle)
    {
        apply(vehicle, vehiclePath);
        if (!request.configuration.empty())
        {
            if (const auto* configuration = FindChild(*vehicle, request.configuration))
            {
                apply(configuration, vehiclePath + " > " + request.configuration);
            }
        }
    }

    if (preset.empty())
        return false;

    result.found = true;
    result.presetName = preset;
    result.sourcePath = source;
    result.presetPath = GetApplicationDirectory() / "forcefields" / preset;
    if (result.presetPath.extension().empty())
        result.presetPath += ".fff";
    return true;
}

bool LoadResolvedVehicleProfile(const VehicleProfileRequest& request)
{
    if (!LoadConfigurationFile())
        return false;

    ResolvedProfile profile;
    if (!ResolveVehicleProfile(request, profile))
    {
        Logf("No configured FFB profile for game=\"%s\" type=\"%s\" vehicle=\"%s\" configuration=\"%s\" transmission=\"%s\"; using basic centering.",
             request.game.c_str(), request.vehicleType.c_str(), request.vehicle.c_str(),
             request.configuration.c_str(), request.transmission.c_str());
        ClearForceFieldPreset();
        return SetSpringStrength(1.0f);
    }

    if (!std::filesystem::exists(profile.presetPath))
    {
        Logf("Resolved preset does not exist: %s; using basic centering.", profile.presetPath.string().c_str());
        ClearForceFieldPreset();
        return SetSpringStrength(1.0f);
    }

    Logf("Resolved FFB profile: %s", profile.presetName.c_str());
    Logf("Profile source: %s", profile.sourcePath.c_str());
    ClearForceFieldPreset();
    if (!LoadForceFieldPreset(profile.presetPath))
        return false;
    StartPresetTest();
    // The live STATE packet will establish the initial vehicle state.
    return true;
}
} // namespace MultiFFBJoy
