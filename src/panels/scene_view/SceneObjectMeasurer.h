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

/// Locate a component's resizable width/height fields by reflection.
///
/// Sizeable components (Sprite, Clip, Gradient, Text, ...) store size as world
/// meters in float fields named "width"/"height" since the meters-internal
/// migration. Base-class properties are searched first so a derived component
/// can override them.
///
/// Returns true only when BOTH fields were found, writing pointers into the
/// live component through @p outWidth / @p outHeight (either may be null).
bool FindResizeProperties(Deki::Component* comp, const Deki::ComponentMeta* meta, float** outWidth, float** outHeight);

class SceneObjectMeasurer
{
public:
    SceneObjectMeasurer(EditorApp* app, SceneTextureCache* textureCache);

    // Get display dimensions for an object (uses component reflection)
    // Returns true if size was determined, false if object has no visual size
    bool GetDisplaySize(Deki::Object* obj, float& outWidth, float& outHeight);

    // Call OnEditorUpdate on all CustomEditors for all objects in the scene
    void UpdateEditors(Deki::Scene* scene);

private:
    EditorApp* m_App;
    SceneTextureCache* m_TextureCache;
};

}  // namespace DekiEditor
