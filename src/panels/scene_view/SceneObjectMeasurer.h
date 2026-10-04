#pragma once

namespace Deki
{
class Object;
}
namespace Deki
{
class Scene;
}
namespace Deki
{
class Component;
}
namespace Deki
{
struct ComponentMeta;
}

namespace DekiEditor
{

class EditorApp;
class SceneTextureCache;

/// Finds a component's resizable width and height fields by reflection.
///
/// Sizeable components (Sprite, Clip, Gradient, Text, ...) store their size in
/// world meters, in float fields named "width" and "height". Base-class
/// properties are searched first, so a derived component can override them.
///
/// Returns true only when both fields are found, and writes pointers into the
/// live component to `outWidth` / `outHeight` (either may be null).
bool FindResizeProperties(Deki::Component* comp, const Deki::ComponentMeta* meta, float** outWidth, float** outHeight);

class SceneObjectMeasurer
{
public:
    SceneObjectMeasurer(EditorApp* app, SceneTextureCache* textureCache);

    /// The object's drawn size in pixels, asked of its components' editors.
    /// Returns false when the object has no visible size.
    bool GetDisplaySize(Deki::Object* obj, float& outWidth, float& outHeight);

    /// Calls OnEditorUpdate on the CustomEditors of every object in the scene.
    void UpdateEditors(Deki::Scene* scene);

private:
    EditorApp* m_App;
    SceneTextureCache* m_TextureCache;
};

}  // namespace DekiEditor
