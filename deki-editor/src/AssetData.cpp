#include <deki-editor/AssetData.h>
#include <deki-editor/FileIO.h>
#include <deki/Guid.h>
#include "AuthoredFormat.h"
#include <deki/LogSystem.h>
#include <fstream>
#include <filesystem>
#include <random>
#include <regex>
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
    // 122 random bits straight from the OS source. A mt19937 seeded with one
    // random_device value per call could only ever produce 2^32 different
    // GUIDs: about a 1% chance of two assets colliding at 10,000 assets.
    std::random_device rd;
    uint8_t bytes[16];
    for (int i = 0; i < 16; i += 4)
    {
        const uint32_t r = rd();
        bytes[i] = static_cast<uint8_t>(r);
        bytes[i + 1] = static_cast<uint8_t>(r >> 8);
        bytes[i + 2] = static_cast<uint8_t>(r >> 16);
        bytes[i + 3] = static_cast<uint8_t>(r >> 24);
    }
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0F) | 0x40);  // version 4
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3F) | 0x80);  // RFC 4122 variant

    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (int i = 0; i < 16; ++i)
    {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out += '-';
        out += hex[bytes[i] >> 4];
        out += hex[bytes[i] & 0x0F];
    }
    return out;
}

std::string GenerateDeterministicGuid(const std::string& seed)
{
    // Use engine's shared implementation
    return Deki::GenerateDeterministicGuid(seed);
}

std::string SalvageAssetGuid(const std::string& dataPath)
{
    std::ifstream file(dataPath, std::ios::binary);
    if (!file.is_open())
        return "";
    std::stringstream ss;
    ss << file.rdbuf();
    const std::string text = ss.str();

    // Every "guid": "<value>" in the text; a merge conflict on other keys
    // carries the one GUID through untouched.
    static const std::regex kGuid("\"guid\"\\s*:\\s*\"([0-9A-Fa-f-]{8,64})\"");
    std::string found;
    for (std::sregex_iterator it(text.begin(), text.end(), kGuid), end; it != end; ++it)
    {
        const std::string guid = (*it)[1].str();
        if (found.empty())
            found = guid;
        else if (guid != found)
            return "";  // two different GUIDs: not ours to choose
    }
    return found;
}

std::string GetOrCreateAssetGuid(const std::string& assetPath)
{
    std::string existingGuid = ReadAssetGuid(assetPath);
    if (!existingGuid.empty())
        return existingGuid;

    // A sidecar that is there but does not parse is NEVER replaced: it holds
    // the asset's identity and import settings (a git merge conflict is the
    // usual cause, and the user resolves it). Writing a fresh one here used to
    // give the asset a new GUID and break every reference to it.
    const std::string dataPath = GetAssetDataPath(assetPath);
    AssetData data;
    std::error_code ec;
    if (fs::exists(dataPath, ec))
    {
        // Valid JSON with no GUID (settings written by hand): add one, keep the rest.
        std::ifstream in(dataPath, std::ios::binary);
        const nlohmann::json parsed = nlohmann::json::parse(in, nullptr, false);
        in.close();
        if (!parsed.is_discarded() && parsed.is_object())
        {
            data.settings = parsed;
            data.settings.erase("guid");
            data.settings.erase(Deki::AuthoredFormat::VersionKey);
            data.guid = GenerateGuid();
            return SaveAssetData(dataPath, data) ? data.guid : "";
        }

        const std::string salvaged = SalvageAssetGuid(dataPath);
        if (!salvaged.empty())
            DEKI_LOG_ERROR("AssetData: '%s' could not be read (a merge conflict?). Kept its GUID %s; its "
                           "import settings are ignored until the file is fixed. The file was left as it is.",
                           dataPath.c_str(), salvaged.c_str());
        else
            DEKI_LOG_ERROR("AssetData: '%s' could not be read and no single GUID could be recovered from it. "
                           "The asset is skipped until the file is fixed. The file was left as it is.",
                           dataPath.c_str());
        return salvaged;
    }

    // Generate new GUID
    std::string newGuid = GenerateGuid();

    // Write to .data file
    data.guid = newGuid;
    SaveAssetData(dataPath, data);

    return newGuid;
}

}  // namespace DekiEditor
