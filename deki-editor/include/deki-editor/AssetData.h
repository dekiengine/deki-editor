#pragma once

#include <string>
#include <nlohmann/json.hpp>

#include <deki-editor/EditorExport.h>
namespace DekiEditor
{

/// What an asset's .data sidecar file holds: its GUID and its import
/// settings as JSON, keyed by type name. Editor only; a game build bakes the
/// settings into the compiled asset headers.
///
/// Example:
///   {
///     "guid": "abc-123-xyz",
///     "settings": { "sprite": { "frameWidth": 55, "frameHeight": 48 } },
///     "audio": { "volume": 0.8, "loop": true }
///   }
struct DEKI_EDITOR_API AssetData
{
    std::string guid;
    nlohmann::json settings;  // keyed by type name
};

/// Reads the .data file at `path` into `out`. Returns false when the file is
/// missing or invalid.
DEKI_EDITOR_API bool LoadAssetData(const std::string& path, AssetData& out);

/// Writes `data` to the .data file at `path`. Returns true on success.
DEKI_EDITOR_API bool SaveAssetData(const std::string& path, const AssetData& data);

/// The sidecar path for an asset: "foo.png" gives "foo.png.data".
DEKI_EDITOR_API std::string GetAssetDataPath(const std::string& assetPath);

/// The GUID from the asset's .data sidecar, or "" when there is none.
DEKI_EDITOR_API std::string ReadAssetGuid(const std::string& assetPath);

/// The asset's GUID, creating the .data sidecar with a new GUID if needed.
DEKI_EDITOR_API std::string GetOrCreateAssetGuid(const std::string& assetPath);

/// Reads the GUID from the raw text of a sidecar that does not parse as JSON
/// (a git merge conflict, a torn write). Returns "" when the text holds no
/// GUID or more than one distinct GUID: a conflict on the GUID itself cannot
/// be settled here.
DEKI_EDITOR_API std::string SalvageAssetGuid(const std::string& dataPath);

/// A GUID that is always the same for the same `seed`. Used for derived
/// assets such as font sizes.
DEKI_EDITOR_API std::string GenerateDeterministicGuid(const std::string& seed);

/// The GUID of a font's baked form, as deki-2d bakes it (FontSyncHandler): a
/// TrueType font has one per size ("source:size"), a BDF font one whatever
/// the size ("source:bdf").
DEKI_EDITOR_API std::string BakedFontGuid(const std::string& sourceGuid, int fontSize, bool bdf);

/// The GUID of the atlas of the baked form BakedFontGuid names.
DEKI_EDITOR_API std::string BakedFontAtlasGuid(const std::string& sourceGuid, int fontSize, bool bdf);

/// A new random GUID.
DEKI_EDITOR_API std::string GenerateGuid();

}  // namespace DekiEditor
