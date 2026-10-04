#pragma once

#include <string>
#include <cstdint>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <set>
#include <deki-editor/AssetPipeline.h>  // AssetExportTarget

namespace Deki
{
class Object;
}
namespace Deki
{
class Component;
}
namespace Deki
{
class Scene;
}
namespace Deki
{
struct AssetRefBase;
}

namespace DekiEditor
{

class Project;
class AssetPipeline;
class PackageLoader;

/// Kind of an exported asset, which decides its output extension.
enum class ExportAssetType : uint8_t
{
    Texture = 0,
    Animation = 1,
    Font = 2,
    Scene = 3,
    Audio = 4
};

/// A font used at one size: its TTF source and the size. The export bakes
/// each one found in the scenes' TextComponents.
struct FontUsageInfo
{
    std::string sourceGuid;  // TTF asset GUID (from AssetRef.source)
    int fontSize;            // In pixels

    bool operator<(const FontUsageInfo& other) const
    {
        if (sourceGuid != other.sourceGuid)
        {
            return sourceGuid < other.sourceGuid;
        }
        return fontSize < other.fontSize;
    }

    bool operator==(const FontUsageInfo& other) const
    {
        return sourceGuid == other.sourceGuid && fontSize == other.fontSize;
    }
};

/// The assets the project's scenes use, found by scanning every scene file.
struct AssetUsageCollection
{
    std::unordered_set<std::string> usedGuids;  // Every used asset GUID (textures, fonts, animations, ...)
    std::set<FontUsageInfo> fontUsages;         // Fonts and the sizes to bake them at
    // The scene that first referenced each GUID, to say where a missing one is used.
    std::unordered_map<std::string, std::string> referencedFrom;
    // Scenes that could not be read. Their assets are unknown, so the export
    // must stop rather than ship a build missing them.
    std::vector<std::string> problems;
};

/// Exports the project's assets for the runtime. Each used asset is written
/// under its GUID, with no folders and no extension, next to
/// project_data.bin (the project settings the runtime reads).
class AssetExporter
{
public:
    AssetExporter(Project* project, AssetPipeline* pipeline, PackageLoader* packageLoader = nullptr);

    /// Exports what the scenes use, plus the project's force-included assets,
    /// to <project>/storage, for copying to an SD card. Returns true on success.
    bool ExportToStorage();

    /// The same, into `outDir`: a build's asset folder (Storage::AssetRoot on
    /// the target). `outDir` is cleared first.
    bool ExportAssets(const std::string& outDir);

    /// Exports what a target needs before it can load anything else: the
    /// compiled project settings (project_data.bin) and the boot scene.
    /// `payloadDir` is where the build backend wants them
    /// (ITargetBuilder::GetBootPayloadDirectory) and is cleared first.
    /// `bootSceneAssetPath` is the boot scene's asset path (such as
    /// "boot.scene"); empty skips it.
    bool ExportBootPayload(const std::string& payloadDir, const std::string& bootSceneAssetPath);

    /// What the target platform offers. Scene objects that need something it
    /// lacks are left out of this target's build (services/SceneTargetFilter).
    /// Empty keeps every object, which is right for an export with no target.
    void SetTargetCapabilities(const std::set<std::string>& caps) { m_TargetCapabilities = caps; }

    /// The target this export is for. Assets are stored as that target wants
    /// them: an image set to Automatic follows its screen format, and per-asset
    /// and per-target overrides apply (TextureFormatResolve.h). Without one the
    /// editor cache ships as it is, which suits the editor's own target.
    void SetTarget(const AssetExportTarget& target)
    {
        m_Target = target;
        m_HasTarget = true;
    }

    /// Number of assets the last export wrote.
    int GetExportedAssetCount() const { return m_ExportedCount; }

    /// Why the last ExportAssets / ExportBootPayload returned false: one line
    /// per problem (a scene that does not load, a referenced asset that is not
    /// in the project, ...). Each was logged as an error too.
    const std::vector<std::string>& GetProblems() const { return m_Problems; }

    /// Writes the binary asset lookup table to `outputPath` (such as
    /// "storage/asset_table.bin") for the GUIDs in `usedGuids`. It is sorted
    /// by hash for O(log n) lookup of "path:type" -> GUID; assets only in the
    /// cache (baked fonts) are keyed by their GUID. Returns true on success.
    ///
    /// Format:
    /// - [4 bytes] entry count (uint32_t)
    /// - [N entries, each 44 bytes, sorted by hash]:
    ///   - [4 bytes] FNV-1a hash of "path:type" (uint32_t)
    ///   - [37 bytes] GUID string (null-terminated)
    ///   - [3 bytes] padding
    bool ExportAssetLookupTable(const std::string& outputPath, const std::unordered_set<std::string>& usedGuids);

    /// Collects the asset GUIDs and font sizes every scene in the project uses.
    AssetUsageCollection CollectUsedAssets();

private:
    ExportAssetType DetermineAssetType(const std::string& extension, std::string& outDestExtension);

    // Adds the asset references of an object and its children to `collection`.
    void CollectAssetsFromObject(Deki::Object* obj, AssetUsageCollection& collection);

    // Adds what the collected assets cannot load without: the sheet a sprite
    // frame is cut from, the sheet an animation plays. Repeats until nothing
    // new is added.
    void AddAssetDependencies(AssetUsageCollection& collection);

    // Adds the asset references in a component's reflected properties.
    void CollectAssetsFromComponent(Deki::Component* comp, AssetUsageCollection& collection);

    // Adds the font and size a font AssetRef on a component (usually a
    // TextComponent) uses.
    void CollectFontUsage(Deki::Component* comp, const Deki::AssetRefBase& assetRef, AssetUsageCollection& collection);

    // =========================================================================
    // Font baking
    // =========================================================================

    // Bakes each font at the sizes the scenes use. Returns a map from
    // "<sourceGuid>:<size>" to the baked font's GUID, and from
    // "<sourceGuid>:<size>:atlas" to its atlas's GUID.
    std::unordered_map<std::string, std::string> BakeFontsForExport(const std::set<FontUsageInfo>& fontUsages);

    // =========================================================================
    // Export stages
    // =========================================================================

    // Copies the used assets to `storagePath`, each named by its GUID (no
    // folders, no extension). Assets in `skipGuids` are left out.
    void CopyUsedAssetsToOutput(const std::string& storagePath, const std::unordered_set<std::string>& usedGuids,
                                const std::unordered_set<std::string>& skipGuids = {},
                                const std::unordered_map<std::string, std::string>* referencedFrom = nullptr);

    // Logs a reason the export failed and keeps it for GetProblems().
    void AddProblem(const std::string& problem);

    void WriteProjectSettings(const std::string& storagePath);

    // The file whose bytes ship for this asset: its version for the export
    // target when there is one and the asset type can encode it, else the
    // editor cache. Empty when neither exists.
    std::string ExportSourceFor(const std::string& guid);

    Project* m_Project;
    AssetPipeline* m_Pipeline;
    PackageLoader* m_PackageLoader = nullptr;
    int m_ExportedCount = 0;
    std::vector<std::string> m_Problems;
    std::set<std::string> m_TargetCapabilities;
    AssetExportTarget m_Target;
    bool m_HasTarget = false;
};

}  // namespace DekiEditor
