#include "SceneObjectMeasurer.h"
#include "SceneTextureCache.h"
#include "../../EditorApp.h"
#include "../../core/Project.h"
#include <deki-editor/EditorExtension.h>
#include <deki-editor/CustomEditor.h>
#include "../../services/SafeDispatch.h"
#include <deki/Object.h>
#include <deki/Scene.h>
#include <deki/reflection/ComponentRegistry.h>

namespace DekiEditor
{

bool FindResizeProperties(Deki::Component* comp, const Deki::ComponentMeta* meta,
                          float** outWidth, float** outHeight)
{
    if (!comp || !meta || !meta->properties || meta->propertyCount == 0)
        return false;

    float* widthPtr = nullptr;
    float* heightPtr = nullptr;

    auto& registry = Deki::ComponentRegistry::Instance();
    const Deki::ComponentMeta* baseMeta = (meta->baseTypeId != 0) ? registry.GetMeta(meta->baseTypeId) : nullptr;

    auto searchProperties = [&](const Deki::ComponentMeta* m)
    {
        if (!m || !m->properties)
            return;
        for (int i = 0; i < m->propertyCount; ++i)
        {
            const Deki::PropertyInfo& prop = m->properties[i];
            if (!prop.name)
                continue;

            if (prop.type == Deki::PropertyType::Float)
            {
                if (strcmp(prop.name, "width") == 0)
                {
                    widthPtr = reinterpret_cast<float*>(reinterpret_cast<char*>(comp) + prop.offset);
                }
                else if (strcmp(prop.name, "height") == 0)
                {
                    heightPtr = reinterpret_cast<float*>(reinterpret_cast<char*>(comp) + prop.offset);
                }
            }
        }
    };

    // Base class first, then derived — derived can override.
    searchProperties(baseMeta);
    searchProperties(meta);

    if (widthPtr && heightPtr)
    {
        if (outWidth) *outWidth = widthPtr;
        if (outHeight) *outHeight = heightPtr;
        return true;
    }

    return false;
}

SceneObjectMeasurer::SceneObjectMeasurer(EditorApp* app, SceneTextureCache* textureCache)
    : m_App(app), m_TextureCache(textureCache)
{
}

bool SceneObjectMeasurer::GetDisplaySize(Deki::Object* obj, float& outWidth, float& outHeight)
{
    outWidth = 0.0f;
    outHeight = 0.0f;

    // Try to get size from component editors first
    const auto& components = obj->GetComponents();
    for (Deki::Component* comp : components)
    {
        auto& registry = Deki::ComponentRegistry::Instance();
        const Deki::ComponentMeta* meta = registry.GetMeta(comp->GetType());
        if (!meta || !meta->name)
            continue;

        CustomEditor* editor = CustomEditorRegistry::Instance().GetEditorForComponent(meta->name, comp);
        if (!editor)
            continue;

        // Call simplified GetDisplaySize - editors use EditorAssets directly.
        // Guarded so a faulty editor falls through to the next component.
        bool gotSize = false;
        Guard(meta->name, [&]
              { gotSize = editor->GetDisplaySize(comp, outWidth, outHeight); });
        if (gotSize)
        {
            return true;
        }
    }

    return false;
}

void SceneObjectMeasurer::UpdateEditors(Deki::Scene* scene)
{
    if (!scene) return;

    auto& editorRegistry = CustomEditorRegistry::Instance();
    auto& compRegistry = Deki::ComponentRegistry::Instance();

    std::function<void(Deki::Object*)> visitObject = [&](Deki::Object* obj)
    {
        if (!obj) return;

        for (Deki::Component* comp : obj->GetComponents())
        {
            const Deki::ComponentMeta* meta = compRegistry.GetMeta(comp->GetType());
            if (!meta || !meta->name) continue;

            CustomEditor* editor = editorRegistry.GetEditorForComponent(meta->name, comp);
            if (editor)
                Guard(meta->name, [&]
                      { editor->OnEditorUpdate(comp); });
        }

        // Copy the children list before recursing: a CustomEditor's OnEditorUpdate
        // (called for any descendant) may mutate the live children vector and
        // trigger a reallocation, invalidating range-for's cached begin/end.
        auto children = obj->GetChildren();
        for (Deki::Object* child : children)
            visitObject(child);
    };

    for (Deki::Object* root : scene->GetObjects())
        visitObject(root);
}

}  // namespace DekiEditor
