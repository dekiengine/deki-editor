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

    // Draw the scene preview image
    void DrawPreview(const std::string& assetPath);

    // Invalidate cached preview (call when scene changes)
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
