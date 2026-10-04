#pragma once

#include <deki-editor/EditorExport.h>

#include <deki-editor/EditorExtension.h>
#include <string>
#include <cstdint>
// Included here so custom-editor sources need not include imgui.h
// themselves. deki-editor links imgui PUBLIC, so packages get the include
// path too.
#include "imgui.h"

namespace Deki
{
class Component;
}

namespace DekiEditor
{

/// Custom editor for one component type, like Unity's CustomEditor: it can
/// replace the component's Inspector UI and tell the scene view its size.
/// Register it with REGISTER_EDITOR.
///
/// Usage:
///   class SpriteCustomEditor : public CustomEditor
///   {
///   public:
///       const char* GetComponentName() const override { return "SpriteComponent"; }
///
///       bool GetDisplaySize(Deki::Component* comp, float& outWidth, float& outHeight) override
///       {
///           auto* sprite = static_cast<SpriteComponent*>(comp);
///           outWidth = sprite->width;
///           outHeight = sprite->height;
///           return true;
///       }
///   };
///
///   REGISTER_EDITOR(SpriteCustomEditor)
class DEKI_EDITOR_API CustomEditor : public EditorExtension
{
public:
    virtual ~CustomEditor() = default;

    // =========================================================================
    // EditorExtension interface
    // =========================================================================

    EditorType GetEditorType() const override { return EditorType::CustomEditor; }
    const char* GetName() const override { return GetComponentName(); }

    // =========================================================================
    // Metadata
    // =========================================================================

    /// The component type this editor handles, e.g. "SpriteComponent".
    virtual const char* GetComponentName() const = 0;

    // =========================================================================
    // Inspector Override (Optional)
    // =========================================================================

    /// Return true to have OnInspectorGUI draw this component instead of the
    /// default property editor.
    virtual bool WantsInspectorOverride(Deki::Component* comp) { return false; }

    /// Draws the Inspector for the component. Called only when
    /// WantsInspectorOverride() returns true.
    ///
    /// EditorUI::Get() offers:
    /// - DrawDefaultInspector() - draw all properties
    /// - PropertyField(name) - draw one property
    /// - PropertyField(offset) - draw a property by offset
    virtual void OnInspectorGUI(Deki::Component* comp) {}

    // =========================================================================
    // Per-Frame Update (Optional)
    // =========================================================================

    /// Called every frame before rendering, for every component that has an
    /// editor, selected or not. Use it for lazy sync or dirty checks.
    virtual void OnEditorUpdate(Deki::Component* comp) {}

    // =========================================================================
    // Display Size / Hit Testing
    // =========================================================================

    /// The component's size in the scene view. Returns false when it has none.
    virtual bool GetDisplaySize(Deki::Component* comp, float& outWidth, float& outHeight) { return false; }

    /// Whether a point hits the component. `localX`/`localY` are relative to
    /// the object's center; `width`/`height` are its display size.
    virtual bool HitTest(Deki::Component* comp, float localX, float localY, float width, float height)
    {
        float halfW = width * 0.5f;
        float halfH = height * 0.5f;
        return (localX >= -halfW && localX <= halfW && localY >= -halfH && localY <= halfH);
    }

    /// The GUID of the texture the component shows, or "".
    virtual std::string GetTextureAssetGuid(Deki::Component* comp) { return ""; }

    // =========================================================================
    // Resize Support
    // =========================================================================

    /// Points `outWidth`/`outHeight` at the component's width and height
    /// fields, so the resize tool can edit them. Null when it has none.
    virtual void GetResizeTarget(Deki::Component* comp, int32_t** outWidth, int32_t** outHeight)
    {
        if (outWidth)
        {
            *outWidth = nullptr;
        }
        if (outHeight)
        {
            *outHeight = nullptr;
        }
    }

    // =========================================================================
    // Gizmos (Optional)
    // =========================================================================
    //
    // OnDrawGizmos runs every frame for every object that has this component.
    // OnDrawGizmosSelected runs only when the owning object is selected.
    // Both are called between SceneView::BeginObject/EndObject, so
    // SceneView::Get() gives the drawList, screen position, display size and
    // zoom for the current object.
    //
    // Keep these at the END of the class: a virtual added in the middle
    // changes vtable indices and breaks already-built package DLLs.

    virtual void OnDrawGizmos(Deki::Component* comp) {}
    virtual void OnDrawGizmosSelected(Deki::Component* comp) {}
};

}  // namespace DekiEditor
