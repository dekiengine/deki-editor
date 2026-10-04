#pragma once

#include <string>
#include <memory>

namespace Deki
{
class Object;
}
namespace Deki
{
class ICamera;
}

namespace DekiEditor
{

class EditorApp;
class EditModeRenderer;

class SceneInspector
{
public:
    SceneInspector(EditorApp* app);
    ~SceneInspector();

    /// Draws a preview image of the scene file.
    void DrawPreview(const std::string& assetPath);

    /// Drops the cached preview. Call it when the scene changes.
    void InvalidatePreview() { m_PreviewScenePath.clear(); }

private:
    EditorApp* m_App;

    std::unique_ptr<EditModeRenderer> m_Renderer;
    std::unique_ptr<Deki::Object> m_CameraObject;
    Deki::ICamera* m_Camera = nullptr;
    std::string m_PreviewScenePath;

    void Cleanup();
};

}  // namespace DekiEditor
