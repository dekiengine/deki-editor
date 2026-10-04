#pragma once

#include <deki-editor/EditorExport.h>

#include <deki-editor/EditorExtension.h>
#include <string>
#include <cstdint>
// Editors draw with ImGui for bespoke UI; pull it in here so custom-editor
// sources don't each need their own #include "imgui.h" (deki-editor links
// imgui PUBLIC, so the include path is available to packages too).
#include "imgui.h"

// Forward declarations
namespace Deki
{
class Component;
}

namespace DekiEditor
{

/**
 * @brief Custom editor for a specific component type
 *
 * Provides inspector customization and display size info for components.
 * Similar to Unity's CustomEditor attribute.
 *
 * Inherits from EditorExtension for registration with REGISTER_EDITOR macro.
 *
 * Usage:
 * @code
 * class SpriteCustomEditor : public CustomEditor
 * {
 * public:
 *     const char* GetComponentName() const override { return "SpriteComponent"; }
 *
 *     bool GetDisplaySize(Deki::Component* comp, float& outWidth, float& outHeight) override
 *     {
 *         auto* sprite = static_cast<SpriteComponent*>(comp);
 *         outWidth = sprite->width;
 *         outHeight = sprite->height;
 *         return true;
 *     }
 * };
 *
 * REGISTER_EDITOR(SpriteCustomEditor)
 * @endcode
 */
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

    /**
     * @brief Get the name of the component this editor handles
     * @return Component type name (e.g., "SpriteComponent")
     */
    virtual const char* GetComponentName() const = 0;

    // =========================================================================
    // Inspector Override (Optional)
    // =========================================================================

    /**
     * @brief Check if this editor wants to override the inspector
     * @param comp The component being inspected
     * @return True to call OnInspectorGUI instead of default property editor
     */
    virtual bool WantsInspectorOverride(Deki::Component* comp) { return false; }

    /**
     * @brief Draw custom inspector GUI
     * @param comp The component being inspected
     *
     * Only called if WantsInspectorOverride() returns true.
     *
     * Use EditorUI::Get() to access:
     * - DrawDefaultInspector() - draw all properties
     * - PropertyField(name) - draw specific property
     * - PropertyField(offset) - draw property by offset
     */
    virtual void OnInspectorGUI(Deki::Component* comp) {}

    // =========================================================================
    // Per-Frame Update (Optional)
    // =========================================================================

    /**
     * @brief Called every frame before rendering for all components with editors
     * @param comp The component
     *
     * Use this for lazy sync or dirty-checking that needs to run regardless
     * of whether the object is selected.
     */
    virtual void OnEditorUpdate(Deki::Component* comp) {}

    // =========================================================================
    // Display Size / Hit Testing
    // =========================================================================

    /**
     * @brief Get display size for the component
     * @param comp The component
     * @param outWidth Output width
     * @param outHeight Output height
     * @return True if component has a display size
     */
    virtual bool GetDisplaySize(Deki::Component* comp, float& outWidth, float& outHeight) { return false; }

    /**
     * @brief Hit test a point against the component
     * @param comp The component
     * @param localX Local X coordinate (relative to object center)
     * @param localY Local Y coordinate (relative to object center)
     * @param width Display width
     * @param height Display height
     * @return True if point is inside component bounds
     */
    virtual bool HitTest(Deki::Component* comp, float localX, float localY, float width, float height)
    {
        float halfW = width * 0.5f;
        float halfH = height * 0.5f;
        return (localX >= -halfW && localX <= halfW && localY >= -halfH && localY <= halfH);
    }

    /**
     * @brief Get the texture asset GUID for this component (if any)
     * @param comp The component
     * @return Asset GUID or empty string
     */
    virtual std::string GetTextureAssetGuid(Deki::Component* comp) { return ""; }

    // =========================================================================
    // Resize Support
    // =========================================================================

    /**
     * @brief Get resize target pointers
     * @param comp The component
     * @param outWidth Output pointer to width field
     * @param outHeight Output pointer to height field
     */
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
    // SceneView::Get() exposes drawList, screen position, display size, and
    // zoom for the current object.
    //
    // Keep these at the END of the class - adding virtuals in the middle
    // changes vtable indices and breaks already-built package DLLs.

    virtual void OnDrawGizmos(Deki::Component* comp) {}
    virtual void OnDrawGizmosSelected(Deki::Component* comp) {}
};

}  // namespace DekiEditor
