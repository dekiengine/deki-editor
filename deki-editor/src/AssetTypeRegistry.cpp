#include <deki-editor/AssetTypeRegistry.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <deki/LogSystem.h>

namespace DekiEditor
{

AssetTypeRegistry& AssetTypeRegistry::Instance()
{
    static AssetTypeRegistry instance;
    return instance;
}

AssetTypeRegistry::AssetTypeRegistry()
{
    RegisterBuiltInTypes();
}

void AssetTypeRegistry::RegisterBuiltInTypes()
{
    // Scene: a creatable asset type.
    AssetTypeHandler sceneHandler;
    sceneHandler.typeName = "Scene";
    sceneHandler.displayName = "Scene";
    sceneHandler.extensions = { ".scene" };
    // formatVersion: Deki::AuthoredFormat::SceneVersion (AuthoredFormat.h).
    sceneHandler.defaultContent = R"({
  "formatVersion": 1,
  "objects": [{"name": "Root", "children": []}]
})";
    sceneHandler.compilesTo = AssetCompileTarget::Data;
    sceneHandler.category = AssetCategory::Scene;
    Register(sceneHandler);

    // .asset: the generic JSON envelope for procedural / custom asset types.
    // Packages that own a specific asset shape (e.g. ProceduralSprite) layer
    // their own AssetTypeEditor on top via REGISTER_EDITOR; this makes the
    // bare ".asset" extension resolve to the "Asset" type for AssetPipeline
    // lookups.
    AssetTypeHandler assetHandler;
    assetHandler.typeName = "Asset";
    assetHandler.displayName = "Asset";
    assetHandler.extensions = { ".asset" };
    assetHandler.compilesTo = AssetCompileTarget::None;
    assetHandler.category = AssetCategory::Procedural;
    Register(assetHandler);

    // RenderPipeline is registered by deki-rendering; FrameAnimation, Sprite
    // and BitmapFont by deki-2d.
}

void AssetTypeRegistry::Register(const AssetTypeHandler& handler)
{
    // Replace an existing entry with the same type name (last registration
    // wins, like AssetTypeEditorRegistry). Re-registration is the hot-reload
    // path: RegisterEngineEditors runs again after package DLLs reload, and the
    // fresh handler's callbacks capture the reloaded package's prototype.
    // Skipping here would keep callbacks bound to the unloaded DLL, so every
    // .asset compile after a hot reload would go through a dangling pointer
    // (observed as caches deleted by RefreshAsset and never re-baked).
    bool replaced = false;
    for (auto& existing : m_Types)
    {
        if (existing.typeName == handler.typeName)
        {
            existing = handler;
            replaced = true;
            break;
        }
    }
    if (!replaced)
    {
        m_Types.push_back(handler);
    }

    // Mirror the handler's declared category into the extension map so
    // GetCategoryForExtension can resolve types registered through the
    // handler path without callers having to call RegisterCategory too.
    if (handler.category != AssetCategory::Unknown)
    {
        for (const std::string& ext : handler.extensions)
        {
            std::string lower = ext;
            for (auto& c : lower)
            {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            m_ExtensionCategory[lower] = handler.category;
        }
    }
}

const std::vector<AssetTypeHandler>& AssetTypeRegistry::GetTypes() const
{
    return m_Types;
}

const AssetTypeHandler* AssetTypeRegistry::GetHandler(const std::string& typeName) const
{
    for (const auto& handler : m_Types)
    {
        if (handler.typeName == typeName)
        {
            return &handler;
        }
    }
    return nullptr;
}

const AssetTypeHandler* AssetTypeRegistry::GetHandlerForExtension(const std::string& extension) const
{
    std::string ext = extension;
    // Normalize to lowercase
    for (auto& c : ext)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    for (const auto& handler : m_Types)
    {
        for (const auto& handlerExt : handler.extensions)
        {
            if (handlerExt == ext)
            {
                return &handler;
            }
        }
    }
    return nullptr;
}

bool AssetTypeRegistry::HasHandlerForExtension(const std::string& extension) const
{
    return GetHandlerForExtension(extension) != nullptr;
}

bool AssetTypeRegistry::CreateAsset(const std::string& typeName, const std::string& filePath)
{
    const AssetTypeHandler* handler = GetHandler(typeName);
    // No starting content means the type is imported (an image, a font), not
    // created: writing nothing would leave an empty file that fails to load.
    if (!handler || handler->defaultContent.empty())
    {
        return false;
    }

    std::ofstream file(filePath);
    if (!file.is_open())
    {
        return false;
    }

    file << handler->defaultContent;
    return file.good();
}

bool AssetTypeRegistry::CompileAsset(const nlohmann::json& data, std::vector<uint8_t>& rgba, int& width, int& height)
{
    std::string type = data.value("type", "");
    const AssetTypeHandler* handler = GetHandler(type);
    if (!handler)
    {
        DEKI_LOG_WARNING("AssetTypeRegistry::CompileAsset: No handler for type '%s' (registered types: %zu)",
                         type.c_str(), m_Types.size());
        for (const auto& h : m_Types)
        {
            DEKI_LOG_EDITOR("  - %s", h.typeName.c_str());
        }
        return false;
    }
    if (!handler->compile)
    {
        DEKI_LOG_WARNING("AssetTypeRegistry::CompileAsset: Handler '%s' has no compile function", type.c_str());
        return false;
    }

    return handler->compile(data, rgba, width, height);
}

bool AssetTypeRegistry::CompileAssetToFile(const nlohmann::json& data, const std::string& cachePath)
{
    std::string type = data.value("type", "");
    const AssetTypeHandler* handler = GetHandler(type);
    if (!handler || !handler->compileToFile)
    {
        return false;
    }

    return handler->compileToFile(data, cachePath);
}

void AssetTypeRegistry::Clear()
{
    m_Types.clear();
    m_ExtensionCategory.clear();
    RegisterBuiltInTypes();
}

void AssetTypeRegistry::RegisterCategory(const std::string& extension, AssetCategory category)
{
    std::string lower = extension;
    for (auto& c : lower)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    m_ExtensionCategory[lower] = category;
}

AssetCategory AssetTypeRegistry::GetCategoryForExtension(const std::string& extension) const
{
    std::string lower = extension;
    for (auto& c : lower)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    auto it = m_ExtensionCategory.find(lower);
    if (it != m_ExtensionCategory.end())
    {
        return it->second;
    }
    return AssetCategory::Unknown;
}

std::vector<std::string> AssetTypeRegistry::GetExtensionsForCategory(AssetCategory category) const
{
    std::vector<std::string> result;
    for (const auto& [ext, cat] : m_ExtensionCategory)
    {
        if (cat == category)
        {
            result.push_back(ext);
        }
    }
    return result;
}

}  // namespace DekiEditor
