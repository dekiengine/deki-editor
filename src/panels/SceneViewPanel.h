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

    // Invalidate a specific texture from cache (call when asset is recompiled)
    void InvalidateTexture(const std::string& assetRef);

    // Invalidate all AssetRefs in the active scene (forces reload on next frame)
    void InvalidateAllAssets();

    // Destroy editor camera component before package DLL unload (avoids stale vtable)
    void InvalidateEditorCamera();

    // The scene camera is found by a full tree walk. It changes only when the
    // scene, an object or a component changes, so the answer is kept until
    // the command generation moves or objects are destroyed (EditorApp calls
    // this from OnObjectsDestroyed).
    void InvalidateSceneCamera() { m_SceneCamScene = nullptr; }
    // Rebuild the flattened object list next frame (objects died, scene changed).
    void InvalidateFlatObjects() { m_CachedFlatScene = nullptr; }

    // Update all CustomEditors for the active scene (layout sync, etc.)
    void UpdateCustomEditors();

    // True while an object is being dragged or resized in the Scene view (this
    // frame or the last). Drags move objects every frame outside the command
    // history, so views that redraw only on change (the Play view preview)
    // check this alongside the generation, like InspectorPanel::IsAnyLiveEditing.
    static bool IsEditingObjects();

    // Switch the active scene-view tool (Select/Move/Rect). Used to honor
    // package-DLL tool requests, e.g. ClipComponent's "Edit Rect" button.
    void SetCurrentTool(SceneTool tool);

private:
    void DrawSceneCanvas();
    void HandleInput();
    // Adjust the pan so the world point `world` sits at screen point `screen`
    // at the current zoom (zoom-to-cursor).
    void KeepWorldPointAt(const ImVec2& world, const ImVec2& screen);

    // Get camera from scene (creates default if none exists)
    Deki::ICamera* GetSceneCamera();
    Deki::Object* GetSceneCameraObject();

    EditorApp* m_App;

    // Subsystems
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
    // pan keeps the world point that was under the cursor at the cursor.
    float m_ZoomTarget = 1.0f;
    ImVec2 m_ZoomAnchorScreen;
    ImVec2 m_ZoomAnchorWorld;
    float m_PanOffsetX = 0.0f;
    float m_PanOffsetY = 0.0f;
    bool m_IsPanning = false;
    ImVec2 m_LastPanPos;
    bool m_NeedsCentering = true;
    int m_CenteringDelay = 3;  // Wait frames for layout to stabilize

    // Canvas size the pan offset was last valid for. The pan is measured from
    // the canvas's TOP-LEFT, so a resize would otherwise slide the scene: the
    // panel grows right and down while the content stays pinned to the corner.
    // Comparing against this lets the pan absorb the change and keep whatever
    // was in the middle of the view in the middle. Zero until the first draw.
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

    // Click-vs-drag discrimination for the selected object: mouse-down on the
    // selected object arms this. Promotes to a real drag on movement, or
    // triggers a cycle-next on release without movement.
    bool m_PendingDrag = false;
    ImVec2 m_PendingDragStartScreen;

    // A finger pressed in the Select tool, undecided: dragging pans the view,
    // lifting in place selects. See the touch branch of the click handling.
    bool m_TouchPendingPan = false;
    ImVec2 m_TouchPressScreen;

    // Box select: a press anywhere but on the selected object or a handle.
    // Dragging draws the box and selects what it holds, on top of the
    // selection kept from the press when Shift or Ctrl was down; released in
    // place, it is a click and picks what is under it. The pick waits for the
    // release because a background covers most of a scene: picking on press
    // would leave no empty canvas to start a box from.
    bool m_MarqueePending = false;
    bool m_MarqueeActive = false;
    bool m_MarqueeAdditive = false;
    ImVec2 m_MarqueeStart;
    std::vector<Deki::Object*> m_MarqueeBase;

    // Track scene to detect changes
    void* m_LastScene = nullptr;

    ::Deki::Scene* m_SceneCamScene = nullptr;
    uint64_t m_SceneCamGeneration = 0;
    Deki::ICamera* m_SceneCam = nullptr;
    Deki::Object* m_SceneCamObject = nullptr;
    void RefreshSceneCameraCache();

    // Cached flattened+sorted object list (per-frame work in HandleInput).
    // Invalidated by scene pointer change, root count change, any
    // CommandHistory state change, or active drag (drags mutate object
    // positions live; sort order is stable but selection/positions update).
    std::vector<Deki::Object*> m_CachedFlatObjects;
    ::Deki::Scene* m_CachedFlatScene = nullptr;
    size_t m_CachedFlatRootCount = 0;
    uint64_t m_CachedFlatGeneration = 0;

    // Edit-mode buffer render dirty tracking. Skip the offscreen
    // RenderScene call when nothing relevant changed since last render.
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

    // Gizmo hover throttle: skip recomputing translate/resize hover state
    // when neither the mouse moved nor the selection changed.
    Deki::Object* m_LastHoverSelected = nullptr;
};

}  // namespace DekiEditor
