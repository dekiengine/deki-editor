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

    // Draw the asset inspector based on selected asset type
    void Draw();

    // Reset cached usage data (call when project changes)
    void InvalidateUsageCache() { m_HasScannedUsage = false; }

private:
    EditorApp* m_App;
    TextureInspector* m_TextureInspector;  // Not owned
    std::unique_ptr<SceneInspector> m_SceneInspector;

    // Draw the JSON-based asset type editor
    void DrawAssetTypeEditor(const std::string& assetPath, const std::string& assetGuid);

    // Draw deploy properties at bottom of inspector
    void DrawDeployProperties(const std::string& assetGuid);

    // True if the typed JSON asset's registered handler compiles to a texture
    // (e.g. ProceduralSprite). Cached per path — the type field of an asset
    // doesn't change while it is selected.
    bool CompilesToTexture(const std::string& assetPath);

    // Asset usage cache (for deploy properties)
    std::unordered_set<std::string> m_UsedGuids;
    bool m_HasScannedUsage = false;

    // CompilesToTexture cache
    std::string m_CompileTargetPath;
    bool m_CompilesToTexture = false;
};

}  // namespace DekiEditor
