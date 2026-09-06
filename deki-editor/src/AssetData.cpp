#include <deki-editor/AssetData.h>
#include <deki-editor/FileIO.h>
#include <deki/Guid.h>
#include "AuthoredFormat.h"
#include <deki/LogSystem.h>
#include <fstream>
#include <filesystem>
#include <random>
#include <sstream>
#include <iomanip>

namespace fs = std::filesystem;

namespace DekiEditor
{

bool LoadAssetData(const std::string& path, AssetData& out)
{
    if (!fs::exists(path))
        return false;

    std::ifstream file(path);
    if (!file.is_open())
        return false;

    try
    {
        nlohmann::json j;
        file >> j;

        // Read GUID (required)
        if (!j.contains("guid") || !j["guid"].is_string())
            return false;

        out.guid = j["guid"].get<std::string>();

        // A newer sidecar is read anyway: refusing it would mint a fresh GUID
        // for the asset and orphan every reference to it, which is worse than
        // ignoring settings this editor does not know.
        const int32_t fileVersion = j.value(Deki::AuthoredFormat::VersionKey, 1);
        if (fileVersion > Deki::AuthoredFormat::AssetDataVersion)
            DEKI_LOG_WARNING("AssetData: %s is formatVersion %d, this editor writes %d; settings it does not know are kept as-is",
                             path.c_str(), fileVersion, Deki::AuthoredFormat::AssetDataVersion);

        // Copy all other fields to settings
        out.settings = j;
        out.settings.erase("guid");  // Don't duplicate guid in settings
        out.settings.erase(Deki::AuthoredFormat::VersionKey);

        return true;
    }
    catch (const nlohmann::json::exception&)
    {
        return false;
    }
}

bool SaveAssetData(const std::string& path, const AssetData& data)
{
    try
    {
        // Start with settings, then add guid at top level
        nlohmann::json j = data.settings;
        j["guid"] = data.guid;
        j[Deki::AuthoredFormat::VersionKey] = Deki::AuthoredFormat::AssetDataVersion;

        // .data sidecars carry the asset's GUID. A half-written sidecar
        // would orphan the asset on the next pipeline restart, so write
        // atomically (temp+rename) instead of streaming directly.
        std::string err;
        return AtomicWriteFile(path, j.dump(2), err);
    }
    catch (const nlohmann::json::exception&)
    {
        return false;
    }
}

std::string GetAssetDataPath(const std::string& assetPath)
{
    return assetPath + ".data";
}

std::string ReadAssetGuid(const std::string& assetPath)
{
    std::string dataPath = GetAssetDataPath(assetPath);

    if (fs::exists(dataPath))
    {
        AssetData data;
        if (LoadAssetData(dataPath, data))
            return data.guid;
    }

    return "";
}

std::string GenerateGuid()
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    std::uniform_int_distribution<> dis2(8, 11);

    std::stringstream ss;
    ss << std::hex;

    for (int i = 0; i < 8; i++)
        ss << dis(gen);
    ss << "-";
    for (int i = 0; i < 4; i++)
        ss << dis(gen);
    ss << "-4";  // Version 4
    for (int i = 0; i < 3; i++)
        ss << dis(gen);
    ss << "-";
    ss << dis2(gen);  // Variant
    for (int i = 0; i < 3; i++)
        ss << dis(gen);
    ss << "-";
    for (int i = 0; i < 12; i++)
        ss << dis(gen);

    return ss.str();
}

std::string GenerateDeterministicGuid(const std::string& seed)
{
    // Use engine's shared implementation
    return Deki::GenerateDeterministicGuid(seed);
}

std::string GetOrCreateAssetGuid(const std::string& assetPath)
{
    std::string existingGuid = ReadAssetGuid(assetPath);
    if (!existingGuid.empty())
        return existingGuid;

    // Generate new GUID
    std::string newGuid = GenerateGuid();

    // Write to .data file
    std::string dataPath = GetAssetDataPath(assetPath);
    AssetData data;
    data.guid = newGuid;
    SaveAssetData(dataPath, data);

    return newGuid;
}

}  // namespace DekiEditor
