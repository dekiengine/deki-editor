#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <functional>
#include <nlohmann/json_fwd.hpp>

#include <deki-editor/EditorExport.h>
namespace DekiEditor
{

/// What an asset type compiles to.
enum class AssetCompileTarget
{
    Texture,  // a DTEX texture (e.g. ProceduralSprite)
    Audio,
    Data,  // generic binary data (MessagePack, etc.)
    None,  // passed through as is
};

/// The broad family of an asset, used by the editor for icons, browser
/// grouping and file-dialog filters. Each package declares the category of
/// the extensions it owns (RegisterCategory or AssetTypeHandler::category).
/// Extensions and asset type names belong to the owning package behind a
/// registry, never hardcoded in deki-engine or deki-editor.
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
    std::string defaultContent;           // JSON template for a new asset
    AssetCompileTarget compilesTo = AssetCompileTarget::Texture;
    AssetCategory category = AssetCategory::Unknown;  // added to the extension->category map by Register

    // Compiles to an RGBA buffer, for the Texture target.
    std::function<bool(const nlohmann::json& data, std::vector<uint8_t>& rgba, int& width, int& height)> compile;

    // Compiles the source JSON to a cache file, for the Data target
    // (Animation, Scene, etc.).
    std::function<bool(const nlohmann::json& data, const std::string& cachePath)> compileToFile;

    // Draws the Inspector UI for this asset type. Returns true when the data
    // changed and needs saving.
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

    /// Sets the category of an extension that has no AssetTypeHandler (e.g.
    /// .png, handled by deki-2d's ImageCacheHandler). Packages call this so
    /// the editor's icons and filters come from one place.
    void RegisterCategory(const std::string& extension, AssetCategory category);
    AssetCategory GetCategoryForExtension(const std::string& extension) const;
    std::vector<std::string> GetExtensionsForCategory(AssetCategory category) const;

    /// Drop every handler a package registered, keeping the editor's own
    /// (Scene, Asset).
    ///
    /// Required before FreeLibrary on any package DLL: AssetTypeHandler holds
    /// std::functions built by package code, and a std::function points at
    /// its manager inside the DLL that created it, which runs on destruction
    /// as well as on copy. Handlers also capture the raw AssetTypeEditor*
    /// prototypes owned by AssetTypeEditorRegistry.
    void Clear();

private:
    AssetTypeRegistry();
    // The editor's own types. Must be registered here, not by the editor at
    // startup: Clear runs on every project switch and package reload, and
    // .scene files drop out of the pipeline otherwise.
    void RegisterBuiltInTypes();
    std::vector<AssetTypeHandler> m_Types;
    std::unordered_map<std::string, AssetCategory> m_ExtensionCategory;  // ext (lowercase) -> category
};

}  // namespace DekiEditor
