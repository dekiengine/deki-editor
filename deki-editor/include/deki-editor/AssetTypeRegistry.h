#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <functional>
#include <nlohmann/json_fwd.hpp>

#include <deki-editor/EditorExport.h>
namespace DekiEditor
{

// What format an asset type compiles to
enum class AssetCompileTarget
{
    Texture,  // Compiles to DTEX texture (e.g., ProceduralSprite)
    Audio,    // Compiles to audio format
    Data,     // Compiles to generic binary data (MessagePack, etc.)
    None,     // No compilation needed (pass-through)
};

// Broad asset family used by the editor to pick icons, group browser entries,
// and build file-dialog filters. Packages declare the category for each
// extension they own (via RegisterCategory or AssetTypeHandler::category),
// so the editor never carries a hardcoded `.png == texture` list. See
// CLAUDE.md (project): extensions and asset type names must live in the
// owning package behind a registry, never hardcoded in deki-engine or
// deki-editor.
enum class AssetCategory
{
    Unknown,
    Texture,     // raster images and cached DTEX
    Procedural,  // .asset files driven by package-owned ProceduralSprite/etc.
    Audio,
    Font,
    Scene,
    Animation,
    Pipeline,  // render pipeline definitions
    Script,
    Data,  // generic data files
};

struct DEKI_EDITOR_API AssetTypeHandler
{
    std::string typeName;                 // "ProceduralSprite", "Animation", "Scene"
    std::string displayName;              // "Procedural Sprite"
    std::vector<std::string> extensions;  // {".asset"}, {".anim"}, {".scene"}
    std::string defaultContent;           // Default JSON template
    AssetCompileTarget compilesTo = AssetCompileTarget::Texture;
    AssetCategory category = AssetCategory::Unknown;  // populates extension->category map at Register time

    // Compile to RGBA buffer (for Texture target)
    std::function<bool(const nlohmann::json& data, std::vector<uint8_t>& rgba, int& width, int& height)> compile;

    // Compile to file (for Data target - Animation, Scene, etc.)
    // Takes source JSON and output cache path
    std::function<bool(const nlohmann::json& data, const std::string& cachePath)> compileToFile;

    // Inspector GUI handler: draws editor UI for this asset type
    // Returns true if data was modified (needs save)
    std::function<bool(nlohmann::json& data, const std::string& assetPath, const std::string& assetGuid)> inspectorGUI;
};

class DEKI_EDITOR_API AssetTypeRegistry
{
public:
    static AssetTypeRegistry& Instance();

    void Register(const AssetTypeHandler& handler);
    const std::vector<AssetTypeHandler>& GetTypes() const;
    const AssetTypeHandler* GetHandler(const std::string& typeName) const;
    const AssetTypeHandler* GetHandlerForExtension(const std::string& extension) const;
    bool HasHandlerForExtension(const std::string& extension) const;
    bool CreateAsset(const std::string& typeName, const std::string& filePath);
    bool CompileAsset(const nlohmann::json& data, std::vector<uint8_t>& rgba, int& width, int& height);
    bool CompileAssetToFile(const nlohmann::json& data, const std::string& cachePath);

    // Category-only registration: for extensions that have no AssetTypeHandler
    // (e.g. .png is handled by deki-2d's ImageCacheHandler, not a type entry).
    // Packages call this so the editor's UI classification (icons, filters)
    // has a single source of truth.
    void RegisterCategory(const std::string& extension, AssetCategory category);
    AssetCategory GetCategoryForExtension(const std::string& extension) const;
    std::vector<std::string> GetExtensionsForCategory(AssetCategory category) const;

    /// Drop every registered handler.
    ///
    /// Required before FreeLibrary on any package DLL: AssetTypeHandler holds
    /// std::functions built by package code, and a std::function carries a
    /// pointer to its manager inside the DLL that created it - which runs on
    /// destruction as well as on copy. Handlers also capture the raw
    /// AssetTypeEditor* prototypes owned by AssetTypeEditorRegistry.
    void Clear();

private:
    AssetTypeRegistry() = default;
    std::vector<AssetTypeHandler> m_Types;
    std::unordered_map<std::string, AssetCategory> m_ExtensionCategory;  // ext (lowercase) -> category
};

}  // namespace DekiEditor
