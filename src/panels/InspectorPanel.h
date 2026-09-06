#pragma once

#include "IPanel.h"

#include <string>
#include <memory>
#include <vector>
#include <map>
#include <nlohmann/json.hpp>
#include "inspector/InspectorTypes.h"

// Forward declarations - engine types
namespace Deki { class Object; }
namespace Deki { class Component; }
namespace Deki { struct ComponentMeta; }
namespace Deki { class Scene; }

namespace DekiEditor
{

// Forward declarations - editor types
class EditorApp;
class AssetPicker;
class PropertyEditorFactory;
class AssetPropertyEditor;
class ComponentManager;
class TextureInspector;
class AssetTypeInspector;

class InspectorPanel : public IPanel
{
public:
    const char* Id() const override { return "Inspector"; }
    bool OpenByDefault() const override { return true; }
    InspectorPanel(EditorApp* app);
    ~InspectorPanel();

    void Draw();

    // Clear any cached references to scene objects (call before scene reload)
    void ClearSceneReferences();

    // Drop transient UI state that holds raw pointers into plugin-DLL data
    // (Deki::PropertyInfo*, Deki::Component*). Must be called BEFORE the plugin
    // DLL unloads during hot reload, otherwise those pointers dangle.
    void ResetTransientUiState();

    // Invalidate preview texture (call when asset is recompiled)
    void InvalidatePreview();

    bool m_Locked = false;  // When true, inspector stays on current selection

    // Live-edit tracking. Set true between IsItemActivated and
    // IsItemDeactivatedAfterEdit while the user is dragging or typing into
    // a transform field or any deferred property widget. SceneView reads
    // IsAnyLiveEditing() to force a buffer re-render each frame during the
    // edit — without this, the scene view stays frozen on the pre-edit
    // value until the field commits, and only gizmos follow the live value.
    bool IsLiveEditing() const { return m_LiveEditCount > 0; }
    void BeginLiveEdit()
    {
        ++m_LiveEditCount;
        ++s_GlobalLiveEditCount;
    }
    void EndLiveEdit()
    {
        if (m_LiveEditCount > 0) --m_LiveEditCount;
        if (s_GlobalLiveEditCount > 0) --s_GlobalLiveEditCount;
    }

    // Global counter for edits made without an InspectorPanel pointer to hand.
    // Both the instance Begin/End and the Global* statics increment this, so a
    // single SceneView check covers transform fields AND reflection-based
    // widgets.
    static bool IsAnyLiveEditing() { return s_GlobalLiveEditCount > 0; }
    static void GlobalBeginLiveEdit() { ++s_GlobalLiveEditCount; }
    static void GlobalEndLiveEdit()
    {
        if (s_GlobalLiveEditCount > 0) --s_GlobalLiveEditCount;
    }

private:
    int m_LiveEditCount = 0;
    static int s_GlobalLiveEditCount;

    // Locked state: cached selection from when lock was enabled
    // Inspector-wide property filter (published to InspectorUtils each frame,
    // where the row and component drawing read it).
    char m_PropertyFilter[64] = {};

    // Inline rename on the header heading (the object name IS the field, the
    // way a node's title is in the graph editor). Null unless a rename is open.
    Deki::Object* m_RenamingObject = nullptr;
    bool m_RenameFocusPending = false;
    char m_RenameBuffer[256] = {};
    std::string m_RenameOldName;

    // Transform's expansion while a filter is active — same deal as the
    // components' (see ComponentManager): forced open so a match is visible,
    // with the pre-filter state recorded on the first frame and restored when
    // the filter clears.
    unsigned int m_TransformHeaderId = 0;
    bool m_TransformPreFilterOpen = true;
    bool m_TransformStateKnown = false;

    std::string m_LockedAssetPath;
    std::string m_LockedAssetCachePath;
    std::string m_LockedAssetGuid;
    bool m_LockedAssetHasCached = false;
    std::vector<Deki::Object*> m_LockedObjects;

    EditorApp* m_App;

    // Subsystems (owned)
    std::unique_ptr<AssetPicker> m_AssetPicker;
    std::unique_ptr<PropertyEditorFactory> m_PropertyEditor;
    std::unique_ptr<AssetPropertyEditor> m_AssetPropertyEditor;
    std::unique_ptr<ComponentManager> m_ComponentManager;
    std::unique_ptr<TextureInspector> m_TextureInspector;
    std::unique_ptr<AssetTypeInspector> m_AssetTypeInspector;

    // State
    PropertyEditState m_PropertyEditState;

    // Transform drag tracking (for undo on drag end)
    Deki::Object* m_DraggedTransformObject = nullptr;
    float m_DragStartPosX = 0.0f;
    float m_DragStartPosY = 0.0f;
    float m_DragStartRotation = 0.0f;
    float m_DragStartScaleX = 0.0f;
    float m_DragStartScaleY = 0.0f;

    // Cache for the "has any overrides?" check used to gate the Apply button
    // on scene-instance objects. CollectOverrides loads the source file and
    // walks the component tree, so we only recompute when the inspected object
    // changes or a new command is executed.
    Deki::Object* m_OverrideCacheObj = nullptr;
    size_t m_OverrideCacheUndoCount = 0;
    bool m_OverrideCacheHasOverrides = false;

    // Pending Apply confirmation — populated when the user clicks Apply,
    // drained when the modal is confirmed or cancelled.
    struct ApplyPopupEntry
    {
        std::string path;       // object path relative to instance root ("" = root)
        std::string component;  // "" for object-level props (e.g. active)
        std::string label;      // human-friendly property name
        std::string oldStr;     // formatted source value
        std::string newStr;     // formatted current value
        nlohmann::json raw;     // the original override JSON object
        bool selected = true;   // per-row checkbox state in the popup
    };

    bool m_ApplyPopupOpen = false;
    Deki::Object* m_ApplyPopupInstance = nullptr;
    ::Deki::Scene* m_ApplyPopupScene = nullptr;
    std::vector<std::string> m_ApplyPopupOtherPaths;
    std::vector<ApplyPopupEntry> m_ApplyPopupEntries;
    std::string m_ApplyPopupSourceName;

    // Object inspector methods
    void DrawTransform(Deki::Object* obj);
    void DrawComponent(Deki::Component* comp, const Deki::ComponentMeta* meta);

    // Apply-to-scene confirmation modal
    void DrawApplyConfirmPopup();
    void ClearApplyPopupState();

    // Multi-object inspector
    void DrawMultiObjectInspector(const std::vector<Deki::Object*>& objects);

    // Multi-edit helpers
    struct ComponentGroup
    {
        std::string typeName;
        const Deki::ComponentMeta* meta;
        std::vector<Deki::Component*> components;
    };
    std::vector<ComponentGroup> GetCommonComponents(const std::vector<Deki::Object*>& objects);
    void DrawMultiComponentSection(const ComponentGroup& group);
};

}  // namespace DekiEditor
