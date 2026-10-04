#pragma once

#include <string>
#include <memory>
#include <unordered_set>

namespace DekiEditor
{

class EditorApp;
class TextureInspector;
class SceneInspector;

class AssetTypeInspector
{
public:
    AssetTypeInspector(EditorApp* app, TextureInspector* textureInspector);
    ~AssetTypeInspector();

    /// Draws the inspector for the selected asset, by its type.
    void Draw();

    /// Forgets which assets scenes use. Call it when the project changes.
    void InvalidateUsageCache() { m_HasScannedUsage = false; }

private:
    EditorApp* m_App;
    TextureInspector* m_TextureInspector;  // Not owned
    std::unique_ptr<SceneInspector> m_SceneInspector;

    // The editor registered for a JSON asset's type.
    void DrawAssetTypeEditor(const std::string& assetPath, const std::string& assetGuid);

    // The deploy rows at the bottom of the inspector.
    void DrawDeployProperties(const std::string& assetGuid);

    // True if the JSON asset's type compiles to a texture (ProceduralSprite,
    // say). Cached per path, since an asset's type does not change while it is
    // selected.
    bool CompilesToTexture(const std::string& assetPath);

    // Assets scenes use, for the deploy rows.
    std::unordered_set<std::string> m_UsedGuids;
    bool m_HasScannedUsage = false;

    // CompilesToTexture cache
    std::string m_CompileTargetPath;
    bool m_CompilesToTexture = false;
};

}  // namespace DekiEditor
