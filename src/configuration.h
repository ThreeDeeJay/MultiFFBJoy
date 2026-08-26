#pragma once
#include <string>
#include <filesystem>
namespace MultiFFBJoy
{
    bool LoadConfigurationFile();
    bool ResolveVehicleProfile(
        const VehicleProfileRequest& request,
        ResolvedProfile& result);
    bool LoadResolvedVehicleProfile(
        const VehicleProfileRequest& request);
    std::filesystem::path GetConfigurationFilePath();
}