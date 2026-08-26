#pragma once
#include <string>
#include <filesystem>
namespace MultiFFBJoy
{
    struct VehicleProfileRequest
    {
        std::string game;
        std::string vehicleType;
        std::string vehicle;
        std::string configuration;
        std::string transmission;
    };
    struct ResolvedProfile
    {
        bool found = false;
        std::string presetName;
        std::filesystem::path presetPath;
        std::string sourcePath;
    };
    bool LoadConfigurationFile();
    bool ResolveVehicleProfile(
        const VehicleProfileRequest& request,
        ResolvedProfile& result);
    bool LoadResolvedVehicleProfile(
        const VehicleProfileRequest& request);
    std::filesystem::path GetConfigurationFilePath();
}