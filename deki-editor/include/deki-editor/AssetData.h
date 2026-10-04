#pragma once

#include <string>
#include <nlohmann/json.hpp>

#include <deki-editor/EditorExport.h>
namespace DekiEditor
{

/**
 * @brief Asset data stored in .data sidecar files
 *
 * Editor-only. Contains GUID + type-specific import settings as JSON.
 *
 * Format example:
 * {
 *   "guid": "abc-123-xyz",
 *   "sprite": { "frameWidth": 55, "frameHeight": 48 },
 *   "audio": { "volume": 0.8, "loop": true }
 * }
 *
 * For a game build, settings are baked into compiled asset headers.
 */
struct DEKI_EDITOR_API AssetData
{
    std::string guid;
    nlohmann::json settings;  // Type-specific settings, keyed by type name
};

/**
 * @brief Load asset data from a .data file
 * @param path Path to the .data file
 * @param out Output asset data
 * @return true on success, false if file doesn't exist or is invalid
 */
DEKI_EDITOR_API bool LoadAssetData(const std::string& path, AssetData& out);

/**
 * @brief Save asset data to a .data file
 * @param path Path to the .data file
 * @param data Asset data to save
 * @return true on success
 */
DEKI_EDITOR_API bool SaveAssetData(const std::string& path, const AssetData& data);

/**
 * @brief Get the .data file path for an asset
 * @param assetPath Path to the asset (e.g., "foo.png")
 * @return Path with .data appended (e.g., "foo.png.data")
 */
DEKI_EDITOR_API std::string GetAssetDataPath(const std::string& assetPath);

/**
 * @brief Read asset GUID from .data sidecar file
 * @param assetPath Path to the asset file
 * @return GUID string, or empty string if not found
 */
DEKI_EDITOR_API std::string ReadAssetGuid(const std::string& assetPath);

/**
 * @brief Get or create asset GUID (creates .data file if needed)
 * @param assetPath Path to the asset file
 * @return GUID string
 */
DEKI_EDITOR_API std::string GetOrCreateAssetGuid(const std::string& assetPath);

/**
 * The GUID written in a sidecar that does not parse as JSON (a git merge
 * conflict, a torn write), read from its raw text. Returns "" when the text
 * holds no GUID or more than one distinct GUID (a conflict on the GUID itself
 * cannot be settled here).
 */
DEKI_EDITOR_API std::string SalvageAssetGuid(const std::string& dataPath);

/**
 * @brief Generate a deterministic GUID from a seed string (for derived assets like font sizes)
 * @param seed Seed string for GUID generation
 * @return Deterministic GUID string
 */
DEKI_EDITOR_API std::string GenerateDeterministicGuid(const std::string& seed);

/**
 * @brief Generate a random GUID
 * @return Random GUID string
 */
DEKI_EDITOR_API std::string GenerateGuid();

}  // namespace DekiEditor
