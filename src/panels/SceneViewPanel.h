#pragma once

#include "IPanel.h"

#include "scene_view/SceneViewTypes.h"
#include "imgui.h"
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

namespace Deki
{
class Object;
}
namespace Deki
{
class ICamera;
}
namespace Deki
{
class LifecycleManager;
}
namespace Deki
{
class Scene;
}

namespace DekiEditor
{

class EditorApp;
class SceneTextureCache;
class SceneCoordinates;
class SceneObjectMeasurer;
class SceneSelection;
class SceneGizmos;
class SceneToolbar;
class EditModeRenderer;
class SceneSnapping;

class SceneViewPanel : public IPanel
{
public:
    const char* Id() const override { return "SceneView"; }
    bool OpenByDefault() const override { return true; }
    SceneViewPanel(EditorApp* app);
    ~SceneViewPanel();

    void Draw();

    /// Drops one texture from the cache. Call it when the asset is recompiled.
    void InvalidateTexture(const std::string& assetRef);

    /// Marks every AssetRef in the active scene for reload on the next frame.
    void InvalidateAllAssets();

    /// Destroys the editor camera before a package DLL unloads, so it keeps
    /// no vtable from the unloaded code.
    void InvalidateEditorCamera();

    /// Forgets the cached scene camera. Finding it walks the whole tree, so the
    /// answer is kept until the command generation moves or objects are
    /// destroyed (EditorApp calls this from OnObjectsDestroyed).
    void InvalidateSceneCamera() { m_SceneCamScene = nullptr; }
    /// Rebuilds the flat object list next frame, after objects die or the
    /// scene changes.
    void InvalidateFlatObjects() { m_CachedFlatScene = nullptr; }

    /// Updates every CustomEditor in the active scene (layout sync and such).
    void UpdateCustomEditors();

    /// Moves the active scene's Screen Anchor objects to their points on the
    /// screen the Play view shows. Play mode places them as it renders.
    void PlaceScreenAnchors();

    /// True while an object is dragged or resized in the Scene view (this frame
    /// or the last). Drags move objects outside the command history, so views
    /// that redraw only on change (the Play view preview) check this as well as
    /// the generation, like InspectorPanel::IsAnyLiveEditing.
    static bool IsEditingObjects();

    /// Switches the Scene view tool (Select/Move/Rect). Packages use it, for
    /// example ClipComponent's "Edit Rect" button.
    void SetCurrentTool(SceneTool tool);

private:
    void DrawSceneCanvas();
    void HandleInput();
    // Moves the pan so the world point `world` sits at screen point `screen`
    // at the current zoom (zoom to cursor).
    void KeepWorldPointAt(const ImVec2& world, const ImVec2& screen);

    // The scene's camera; creates a default one when there is none.
    Deki::ICamera* GetSceneCamera();
    Deki::Object* GetSceneCameraObject();

    EditorApp* m_App;

    std::unique_ptr<SceneTextureCache> m_TextureCache;
    std::unique_ptr<SceneCoordinates> m_Coords;
    std::unique_ptr<SceneObjectMeasurer> m_Measurer;
    std::unique_ptr<SceneSelection> m_Selection;
    std::unique_ptr<SceneGizmos> m_Gizmos;
    std::unique_ptr<SceneToolbar> m_Toolbar;
    std::unique_ptr<EditModeRenderer> m_EditModeRenderer;
    std::unique_ptr<SceneSnapping> m_Snapping;

    // Lifecycle manager for edit mode asset loading
    std::unique_ptr<Deki::LifecycleManager> m_EditModeLifecycle;

    // Invisible editor camera (never serialized)
    std::unique_ptr<Deki::Object> m_EditorCameraObject;
    Deki::ICamera* m_EditorCamera = nullptr;  // Owned by m_EditorCameraObject

    // View state
    float m_Zoom = 1.0f;
    // Wheel zoom eases: m_Zoom follows this target each frame (Motion), and the
    // world point under the cursor stays under the cursor.
    float m_ZoomTarget = 1.0f;
    ImVec2 m_ZoomAnchorScreen;
    ImVec2 m_ZoomAnchorWorld;
    float m_PanOffsetX = 0.0f;
    float m_PanOffsetY = 0.0f;
    bool m_IsPanning = false;
    ImVec2 m_LastPanPos;
    bool m_NeedsCentering = true;
    int m_CenteringDelay = 3;  // frames to wait for the layout to settle

    // Canvas size the pan offset was last set for. The pan is measured from
    // the canvas's top-left, so on a resize the pan is shifted by half the
    // change to keep the middle of the view in the middle. Zero until the
    // first draw.
    ImVec2 m_PannedForCanvasSize = ImVec2(0.0f, 0.0f);

    // Canvas position (set during draw)
    ImVec2 m_CanvasTopLeft;
    ImVec2 m_CanvasSize;

    // Object dragging
    bool m_IsDragging = false;
    float m_DragOffsetX = 0.0f;
    float m_DragOffsetY = 0.0f;
    float m_DragStartObjX = 0.0f;
    float m_DragStartObjY = 0.0f;
    // A Screen Anchor's offset when the drag started; a drag moves an
    // anchored object through its offset.
    float m_DragStartAnchorX = 0.0f;
    float m_DragStartAnchorY = 0.0f;
    void RememberAnchorOffset(Deki::Object* obj);

    // Click or drag on the selected object: a press arms this. Moving turns it
    // into a drag; releasing without moving selects the next object under the
    // cursor.
    bool m_PendingDrag = false;
    ImVec2 m_PendingDragStartScreen;

    // A finger pressed in the Select tool, undecided: dragging pans the view,
    // lifting in place selects. See the touch branch of the click handling.
    bool m_TouchPendingPan = false;
    ImVec2 m_TouchPressScreen;

    // Box select: a press anywhere but on the selected object or a handle.
    // Dragging draws the box and selects what it holds, added to the previous
    // selection when Shift or Ctrl was down. Released in place, it is a click
    // and picks what is under it. The pick waits for the release because a
    // background covers most scenes, and picking on press would leave no empty
    // canvas to start a box from.
    bool m_MarqueePending = false;
    bool m_MarqueeActive = false;
    bool m_MarqueeAdditive = false;
    ImVec2 m_MarqueeStart;
    std::vector<Deki::Object*> m_MarqueeBase;

    // The scene drawn last frame, to notice a scene change.
    void* m_LastScene = nullptr;

    ::Deki::Scene* m_SceneCamScene = nullptr;
    uint64_t m_SceneCamGeneration = 0;
    Deki::ICamera* m_SceneCam = nullptr;
    Deki::Object* m_SceneCamObject = nullptr;
    void RefreshSceneCameraCache();

    // Flattened and sorted object list that HandleInput needs every frame.
    // Rebuilt when the scene, its root count or the CommandHistory generation
    // changes, and during a drag.
    std::vector<Deki::Object*> m_CachedFlatObjects;
    ::Deki::Scene* m_CachedFlatScene = nullptr;
    size_t m_CachedFlatRootCount = 0;
    uint64_t m_CachedFlatGeneration = 0;

    // What the last edit-mode render was made for. The offscreen RenderScene
    // is skipped when none of it changed.
    bool m_EditRenderHasRendered = false;
    ::Deki::Scene* m_LastEditRenderedScene = nullptr;
    float m_LastEditRenderCamX = 0.0f;
    float m_LastEditRenderCamY = 0.0f;
    int m_LastEditRenderWidth = 0;
    int m_LastEditRenderHeight = 0;
    // When the last render ran (ImGui time) and what it cost (seconds): while
    // the view moves, renders are spaced by their own cost.
    double m_LastEditRenderTime = 0.0;
    double m_LastEditRenderCost = 0.0;
    float m_LastEditRenderZoom = 1.0f;  // effective zoom that render was made for
    uint8_t m_LastEditRenderClearR = 0;
    uint8_t m_LastEditRenderClearG = 0;
    uint8_t m_LastEditRenderClearB = 0;
    uint64_t m_LastEditRenderGeneration = 0;
    uint64_t m_LastEditRenderAssetEpoch = 0;  // AssetManager epoch: assets that appear or reload

    // Gizmo hover state is recomputed only when the mouse moves or the
    // selection changes.
    Deki::Object* m_LastHoverSelected = nullptr;
};

}  // namespace DekiEditor
