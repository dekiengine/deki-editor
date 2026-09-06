#include "SceneViewPanel.h"
#include "../PanelMaximize.h"
#include "scene_view/SceneTextureCache.h"
#include "scene_view/SceneCoordinates.h"
#include "scene_view/SceneObjectMeasurer.h"
#include "scene_view/SceneSelection.h"
#include "scene_view/SceneGizmos.h"
#include "scene_view/SceneToolbar.h"
#include "scene_view/EditModeRenderer.h"
#include <deki-editor/SceneView.h>
#include <deki-editor/CustomEditor.h>
#include <deki-editor/EditorExtension.h>
#include "scene_view/SceneSnapping.h"
#include "EditorApp.h"
#include <deki-editor/EditorTheme.h>
#include "EditorPixelSnap.h"
#include "InspectorPanel.h"
#include <deki-editor/CommandHistory.h>
#include "core/Commands.h"
#include "core/Project.h"
#include <deki/Scene.h>
#include <deki/Object.h>
#include <deki/ICamera.h>
#include <deki/ISortableProvider.h>
#include <deki/IClipProvider.h>
#include <deki/reflection/ComponentFactory.h>
#include "LifecycleManager.h"
#include <deki/Engine.h>
#include <deki/reflection/ComponentRegistry.h>
#include <deki/reflection/Property.h>
#include "imgui.h"
#include <SDL3/SDL_opengl.h>
#include <SDL3/SDL_video.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

namespace DekiEditor
{

// Helper to get sorting order from a Deki::Object via ISortableProvider interface
static int GetSortingOrder(Deki::Object* obj)
{
    Deki::ISortableProvider* sortable = obj->FindInterface<Deki::ISortableProvider>();
    if (sortable)
        return sortable->GetSortingOrder();
    return 0;
}

// Helper to find float width/height properties via reflection for resize drag
// starting. Sizeable components store size as world meters in float fields.
SceneViewPanel::SceneViewPanel(EditorApp* app)
    : m_App(app)
{
    // Create subsystems
    m_TextureCache = std::make_unique<SceneTextureCache>(app);
    m_Coords = std::make_unique<SceneCoordinates>(app);
    m_Measurer = std::make_unique<SceneObjectMeasurer>(app, m_TextureCache.get());
    m_Selection = std::make_unique<SceneSelection>(m_Measurer.get(), m_TextureCache.get());
    m_Snapping = std::make_unique<SceneSnapping>();
    m_Gizmos = std::make_unique<SceneGizmos>(m_Coords.get(), m_Measurer.get(), m_Snapping.get());
    m_Toolbar = std::make_unique<SceneToolbar>(app);
    m_EditModeRenderer = std::make_unique<EditModeRenderer>();
    m_EditModeLifecycle = std::make_unique<Deki::LifecycleManager>();

    // Create invisible editor camera (never serialized)
    m_EditorCameraObject = std::make_unique<Deki::Object>();
    m_EditorCameraObject->SetName("__EditorCamera");
    m_EditorCameraObject->SetEditorOnly(true);  // Never saved to scene

    // Camera component created lazily in DrawSceneCanvas() — packages may not be loaded yet
}

SceneViewPanel::~SceneViewPanel() = default;

void SceneViewPanel::InvalidateEditorCamera()
{
    m_EditorCamera = nullptr;
    // Recreate the Deki::Object to destroy components while DLL is still loaded
    m_EditorCameraObject = std::make_unique<Deki::Object>();
    m_EditorCameraObject->SetName("__EditorCamera");
    m_EditorCameraObject->SetEditorOnly(true);
}

void SceneViewPanel::UpdateCustomEditors()
{
    auto* scene = m_App->GetCurrentScene();
    if (scene)
        m_Measurer->UpdateEditors(scene);
}

void SceneViewPanel::SetCurrentTool(SceneTool tool)
{
    if (m_Toolbar)
        m_Toolbar->SetCurrentTool(tool);
}

void SceneViewPanel::InvalidateTexture(const std::string& assetRef)
{
    m_TextureCache->Invalidate(assetRef);
}

void SceneViewPanel::InvalidateAllAssets()
{
    auto* scene = m_App->GetCurrentScene();
    if (!scene)
        return;

    // Recursively unload assets from all objects in the scene
    std::function<void(Deki::Object*)> unloadRecursive = [&](Deki::Object* obj)
    {
        if (!obj) return;

        // Unload assets from all components and reset resolution state
        for (Deki::Component* comp : obj->GetComponents())
        {
            comp->UnloadAssets();
            if (auto* behaviour = comp->AsBehaviour())
                behaviour->ResetRefsResolved();
        }

        // Recurse into children
        for (Deki::Object* child : obj->GetChildren())
        {
            unloadRecursive(child);
        }
    };

    // Process all root objects
    for (Deki::Object* obj : scene->GetObjects())
    {
        unloadRecursive(obj);
    }
}

void SceneViewPanel::RefreshSceneCameraCache()
{
    ::Deki::Scene* scene = m_App->GetCurrentScene();
    const uint64_t gen = CommandHistory::Instance().GetGeneration();
    // Play mode spawns objects outside the command history, so it is not
    // cached: the walk runs every frame there, as it always did.
    if (scene == m_SceneCamScene && gen == m_SceneCamGeneration && !m_App->IsPlaying())
        return;

    m_SceneCamScene = scene;
    m_SceneCamGeneration = gen;
    m_SceneCam = nullptr;
    m_SceneCamObject = nullptr;
    if (!scene)
        return;

    // Search recursively through all objects via ICamera interface
    for (Deki::Object* obj : scene->GetObjects())
    {
        Deki::Object* holder = FindInSubtree(obj, [](Deki::Object* o)
                                           { return o->FindInterface<Deki::ICamera>() != nullptr; });
        if (holder)
        {
            m_SceneCamObject = holder;
            m_SceneCam = holder->FindInterface<Deki::ICamera>();
            return;
        }
    }
}

Deki::ICamera* SceneViewPanel::GetSceneCamera()
{
    RefreshSceneCameraCache();
    return m_SceneCam;
}

Deki::Object* SceneViewPanel::GetSceneCameraObject()
{
    RefreshSceneCameraCache();
    return m_SceneCamObject;
}

void SceneViewPanel::Draw()
{
    if (!m_Open)
        return;

    bool visible = ImGui::Begin("SCENE VIEW###Scene View", &m_Open, DekiEditor::MaximizedWindowFlags("SCENE VIEW###Scene View") | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    if (!visible)
    {
        ImGui::End();
        return;
    }

    auto* scene = m_App->GetCurrentScene();
    if (!scene)
    {
        ImGui::TextDisabled("No scene loaded");
        ImGui::End();
        return;
    }

    m_Toolbar->Draw(m_Zoom, m_PanOffsetX, m_PanOffsetY, m_NeedsCentering);

    DrawSceneCanvas();

    ImGui::End();
}

void SceneViewPanel::HandleInput()
{
    ImGuiIO& io = ImGui::GetIO();
    bool isCanvasHovered = ImGui::IsItemHovered();

    // Tool shortcuts (Q, W, R) - only when canvas is hovered and no text input active
    if (isCanvasHovered && !io.WantTextInput)
    {
        if (ImGui::IsKeyPressed(ImGuiKey_Q))
            m_Toolbar->SetCurrentTool(SceneTool::Select);
        else if (ImGui::IsKeyPressed(ImGuiKey_W))
            m_Toolbar->SetCurrentTool(SceneTool::Move);
        else if (ImGui::IsKeyPressed(ImGuiKey_R))
            m_Toolbar->SetCurrentTool(SceneTool::Rect);
    }

    // Delete key. Goes through EditorApp::DeleteObject so the protection rules
    // (scene root, boot Setup, instance children) apply here exactly as in
    // the Hierarchy — this path used to have none.
    Deki::Object* selected = m_App->GetSelectedObject();
    if (selected && isCanvasHovered && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Delete))
        m_App->DeleteObject(selected);

    // Panning with Alt+Left Mouse or Middle Mouse
    bool startPan = isCanvasHovered &&
                    ((ImGui::IsMouseClicked(ImGuiMouseButton_Left) && io.KeyAlt) ||
                     ImGui::IsMouseClicked(ImGuiMouseButton_Middle));

    if (startPan)
    {
        m_IsPanning = true;
        m_LastPanPos = io.MousePos;
    }

    if (m_IsPanning)
    {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Middle))
        {
            ImVec2 delta = ImVec2(io.MousePos.x - m_LastPanPos.x, io.MousePos.y - m_LastPanPos.y);
            m_PanOffsetX += delta.x;
            m_PanOffsetY += delta.y;
            m_LastPanPos = io.MousePos;
        }
        else
        {
            m_IsPanning = false;
        }
    }

    // Zooming with Mouse Wheel
    if (isCanvasHovered && io.MouseWheel != 0)
    {
        ImVec2 mousePos = io.MousePos;
        ImVec2 worldBeforeZoom = m_Coords->ScreenToWorld(mousePos.x, mousePos.y);

        // Scale by wheel delta so high-precision wheels/trackpads don't rocket
        // through the zoom range. Ctrl is the precision modifier (smaller step).
        float zoomBase = io.KeyCtrl ? 1.03f : 1.10f;
        m_Zoom = std::clamp(m_Zoom * std::pow(zoomBase, io.MouseWheel),
                            SceneViewConstants::MinZoom,
                            SceneViewConstants::MaxZoom);

        // Recalculate scene center after zoom change. World→screen is
        // editorZoom * dpi * pixelsPerMeter (world is meters now, not pixels).
        auto* dispSettings = DekiEditor::GetDisplaySettings();
        int vpWidth = dispSettings ? dispSettings->targetWidth : 320;
        int vpHeight = dispSettings ? dispSettings->targetHeight : 240;

        float dpi = ImGui::GetWindowDpiScale();
        float sceneWidth = vpWidth * m_Zoom * dpi;
        float sceneHeight = vpHeight * m_Zoom * dpi;

        Deki::ICamera* sceneCamForZoom = GetSceneCamera();
        const float zoomPPM = sceneCamForZoom ? sceneCamForZoom->GetPixelsPerMeter()
                                              : Deki::EngineSettings::Global().pixelsPerMeter;
        const float worldToScreen = m_Zoom * dpi * (zoomPPM > 0.0f ? zoomPPM : 1.0f);

        float sceneCenterX = m_CanvasTopLeft.x + m_PanOffsetX + sceneWidth * 0.5f;
        float sceneCenterY = m_CanvasTopLeft.y + m_PanOffsetY + sceneHeight * 0.5f;

        float newScreenX = sceneCenterX + worldBeforeZoom.x * worldToScreen;
        float newScreenY = sceneCenterY - worldBeforeZoom.y * worldToScreen;  // Minus: world Y+ is up, screen Y+ is down

        float deltaX = mousePos.x - newScreenX;
        float deltaY = mousePos.y - newScreenY;
        m_PanOffsetX += deltaX;
        m_PanOffsetY += deltaY;

        // NOTE: Don't apply pixel-perfect snapping here - it causes cumulative drift
        // when zooming in/out repeatedly. The rendering already handles pixel-perfect
        // positioning via flooring in WorldToScreen and GetBufferScreenBounds.
    }
}

void SceneViewPanel::DrawSceneCanvas()
{
    auto* scene = m_App->GetCurrentScene();
    if (!scene)
        return;

    // Detect scene change and trigger centering
    if (m_LastScene != scene)
    {
        m_LastScene = scene;
        m_NeedsCentering = true;
        m_CenteringDelay = 3;  // Wait for docking layout to stabilize
    }

    ImVec2 availSize = ImGui::GetContentRegionAvail();
    if (availSize.x <= 0 || availSize.y <= 0)
        return;

    m_CanvasTopLeft = ImGui::GetCursorScreenPos();
    m_CanvasSize = availSize;

    // Absorb a canvas resize into the pan, so the view keeps looking at the
    // same thing instead of sliding. The pan is an offset from the canvas's
    // top-left corner, so growing the panel (maximizing it, dragging a dock
    // splitter, resizing the window) would leave the scene pinned to that
    // corner and push it off-centre. Shifting the pan by half the size change
    // holds the CENTRE of the view still, which is the point the user is
    // looking at. Skipped on the first draw, when there is no previous size,
    // and harmless while m_NeedsCentering is pending since that recomputes
    // the pan outright below.
    if (m_PannedForCanvasSize.x > 0.0f && m_PannedForCanvasSize.y > 0.0f)
    {
        m_PanOffsetX += (availSize.x - m_PannedForCanvasSize.x) * 0.5f;
        m_PanOffsetY += (availSize.y - m_PannedForCanvasSize.y) * 0.5f;
    }
    m_PannedForCanvasSize = availSize;

    // Handle centering BEFORE setting viewport to avoid one-frame glitch
    auto* dispSettings = DekiEditor::GetDisplaySettings();
    int vpWidth = dispSettings ? dispSettings->targetWidth : 320;
    int vpHeight = dispSettings ? dispSettings->targetHeight : 240;

    DekiEditor::SceneView::Get().SetTargetSize(
        dispSettings ? dispSettings->targetWidth : 0,
        dispSettings ? dispSettings->targetHeight : 0);

    float dpi = ImGui::GetWindowDpiScale();

    if (m_NeedsCentering)
    {
        if (m_CenteringDelay > 0)
        {
            m_CenteringDelay--;
        }
        else
        {
            // Reset zoom to 1:1 and center the scene in the canvas
            m_Zoom = 1.0f;
            float sceneWidth = vpWidth * m_Zoom * dpi;
            float sceneHeight = vpHeight * m_Zoom * dpi;

            m_PanOffsetX = (availSize.x - sceneWidth) * 0.5f;
            m_PanOffsetY = (availSize.y - sceneHeight) * 0.5f;
            m_NeedsCentering = false;
        }
    }

    // CameraComponent's vtable lives in deki-rendering.dll. Querying ICamera on
    // a scene component before that package is fully loaded (e.g. mid project
    // open) would dereference an unmapped vtable. Bail out — the next frame
    // re-enters once the rendering package is registered.
    if (!Deki::ComponentRegistry::Instance().GetMetaByClassName("CameraComponent"))
        return;

    Deki::ICamera* sceneCam = GetSceneCamera();
    // Must match the fallback used in HandleInput's zoom-to-cursor math
    // (Deki::EngineSettings::Global().pixelsPerMeter). Using 1.0 here while the
    // zoom branch used global PPM made ScreenToWorld and the post-zoom
    // projection non-inverses, so each wheel notch drove the pan offset toward
    // inf/NaN when the scene had no camera.
    const float projectPPM = sceneCam ? sceneCam->GetPixelsPerMeter()
                                      : Deki::EngineSettings::Global().pixelsPerMeter;

    m_Coords->SetViewport(m_CanvasTopLeft, m_CanvasSize, m_Zoom, m_PanOffsetX, m_PanOffsetY, projectPPM);

    float effectiveZoom = m_Zoom * dpi;
    float sceneWidth = vpWidth * effectiveZoom;
    float sceneHeight = vpHeight * effectiveZoom;

    float sceneLeft = m_CanvasTopLeft.x + m_PanOffsetX;
    float sceneTop = m_CanvasTopLeft.y + m_PanOffsetY;
    float sceneRight = sceneLeft + sceneWidth;
    float sceneBottom = sceneTop + sceneHeight;

    ImGui::InvisibleButton("SceneCanvas", availSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    bool isCanvasHovered = ImGui::IsItemHovered();

    HandleInput();

    // Update coordinate system AFTER HandleInput since zoom/pan may have changed
    m_Coords->SetViewport(m_CanvasTopLeft, m_CanvasSize, m_Zoom, m_PanOffsetX, m_PanOffsetY, projectPPM);

    // Recalculate scene bounds with potentially updated zoom
    effectiveZoom = m_Zoom * dpi;
    sceneWidth = vpWidth * effectiveZoom;
    sceneHeight = vpHeight * effectiveZoom;
    sceneLeft = m_CanvasTopLeft.x + m_PanOffsetX;
    sceneTop = m_CanvasTopLeft.y + m_PanOffsetY;
    sceneRight = sceneLeft + sceneWidth;
    sceneBottom = sceneTop + sceneHeight;

    // Flatten hierarchy for rendering/selection (cached - rebuilt only when
    // structure changes, not every frame). Drag-active also forces rebuild
    // because sortable children can be reordered mid-drag.
    ImGuiIO& io = ImGui::GetIO();
    const std::vector<Deki::Object*>& rootObjects = scene->GetObjects();
    const uint64_t curGen = CommandHistory::Instance().GetGeneration();
    const bool flatStale = (m_CachedFlatScene != scene) || (m_CachedFlatRootCount != rootObjects.size()) || (m_CachedFlatGeneration != curGen) || m_IsDragging || m_Gizmos->IsDraggingResize();
    if (flatStale)
    {
        m_CachedFlatObjects.clear();
        std::function<void(Deki::Object*)> collectObjects = [&](Deki::Object* obj)
        {
            m_CachedFlatObjects.push_back(obj);
            for (Deki::Object* child : obj->GetChildren())
                collectObjects(child);
        };
        for (Deki::Object* obj : rootObjects)
            collectObjects(obj);
        std::sort(m_CachedFlatObjects.begin(), m_CachedFlatObjects.end(),
                  [](Deki::Object* a, Deki::Object* b)
                  {
                      return GetSortingOrder(a) < GetSortingOrder(b);
                  });
        m_CachedFlatScene = scene;
        m_CachedFlatRootCount = rootObjects.size();
        m_CachedFlatGeneration = curGen;
    }
    const std::vector<Deki::Object*>& allObjects = m_CachedFlatObjects;

    Deki::Object* selected = m_App->GetSelectedObject();

    // Helper to build snap targets from all objects except the selected one.
    // Snap targets live in world meters; GetDisplaySize returns int pixels, so
    // convert via (pixels * scale / ppm) to match the object's center, which is
    // already in meters.
    const float invPPM = (projectPPM > 0.0f) ? (1.0f / projectPPM) : 0.0f;
    auto buildObjectSnapTargets = [&](Deki::Object* exclude) -> std::vector<SnapTarget>
    {
        std::vector<SnapTarget> targets;
        for (Deki::Object* obj : allObjects)
        {
            if (obj == exclude) continue;
            float w = 0.0f, h = 0.0f;
            if (!m_Measurer->GetDisplaySize(obj, w, h)) continue;
            float ox = obj->GetWorldX();
            float oy = obj->GetWorldY();
            float hw = w * obj->GetWorldScaleX() * 0.5f * invPPM;
            float hh = h * obj->GetWorldScaleY() * 0.5f * invPPM;
            targets.push_back({ ox - hw, SnapAxis::Vertical });
            targets.push_back({ ox, SnapAxis::Vertical });
            targets.push_back({ ox + hw, SnapAxis::Vertical });
            targets.push_back({ oy - hh, SnapAxis::Horizontal });
            targets.push_back({ oy, SnapAxis::Horizontal });
            targets.push_back({ oy + hh, SnapAxis::Horizontal });
        }
        return targets;
    };

    // Hover-state recomputes only when the mouse moved this frame OR the
    // selection changed since last frame. Idle frames (no mouse motion, same
    // selection) reuse the previous hover result, avoiding the per-component
    // reflection walk in GetDisplaySize.
    const bool mouseMoved = (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f);
    const bool selectionChangedForHover = (selected != m_LastHoverSelected);
    m_LastHoverSelected = selected;
    const bool hoverNeedsUpdate = mouseMoved || selectionChangedForHover;

    // Update translate gizmo hover state
    if (selected && isCanvasHovered && !m_IsDragging && m_Toolbar->GetCurrentTool() == SceneTool::Move)
    {
        if (hoverNeedsUpdate)
            m_Gizmos->UpdateTranslateHover(selected, ImGui::GetMousePos());
    }
    else if (m_Toolbar->GetCurrentTool() != SceneTool::Move)
    {
        m_Gizmos->SetHoveredAxis(GizmoAxis::None);
    }

    // Update resize gizmo hover state
    if (selected && isCanvasHovered && !m_IsDragging && !m_Gizmos->IsDraggingResize() && m_Toolbar->GetCurrentTool() == SceneTool::Rect)
    {
        if (hoverNeedsUpdate)
            m_Gizmos->UpdateResizeHover(selected);
    }
    else if (m_Toolbar->GetCurrentTool() != SceneTool::Rect)
    {
        m_Gizmos->SetHoveredEdge(ResizeEdge::None);
    }

    // Handle click
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && isCanvasHovered && !m_IsPanning && !io.KeyAlt)
    {
        ImVec2 mousePos = ImGui::GetMousePos();
        ImVec2 worldPos = m_Coords->ScreenToWorld(mousePos.x, mousePos.y);

        // Check resize handle first
        if (selected && m_Gizmos->GetHoveredEdge() != ResizeEdge::None)
        {
            auto& registry = Deki::ComponentRegistry::Instance();
            for (Deki::Component* comp : selected->GetComponents())
            {
                const Deki::ComponentMeta* meta = registry.GetMeta(comp->GetType());
                if (!meta)
                    continue;

                float* widthPtr = nullptr;
                float* heightPtr = nullptr;
                if (FindResizeProperties(comp, meta, &widthPtr, &heightPtr))
                {
                    m_Gizmos->StartResizeDrag(m_Gizmos->GetHoveredEdge(),
                                              *widthPtr, *heightPtr,
                                              mousePos.x, mousePos.y,
                                              selected->GetWorldX(), selected->GetWorldY());
                    break;
                }
            }
        }
        // Check translate gizmo
        else if (selected && m_Gizmos->GetHoveredAxis() != GizmoAxis::None)
        {
            m_IsDragging = true;
            m_Gizmos->SetDraggingAxis(m_Gizmos->GetHoveredAxis());
            m_DragOffsetX = selected->GetWorldX() - worldPos.x;
            m_DragOffsetY = selected->GetWorldY() - worldPos.y;
            m_DragStartObjX = selected->GetWorldX();
            m_DragStartObjY = selected->GetWorldY();
        }
        else
        {
            // If click hits the already-selected object, arm a pending drag.
            // Actual drag starts only if the mouse moves past the drag threshold;
            // a click-release without movement advances the overlap cycle instead.
            if (selected && m_Selection->HitTestObject(selected, worldPos.x, worldPos.y, projectPPM))
            {
                m_PendingDrag = true;
                m_PendingDragStartScreen = mousePos;
                m_DragOffsetX = selected->GetWorldX() - worldPos.x;
                m_DragOffsetY = selected->GetWorldY() - worldPos.y;
                m_DragStartObjX = selected->GetWorldX();
                m_DragStartObjY = selected->GetWorldY();
            }
            else
            {
                Deki::Object* clicked = m_Selection->HandleClick(allObjects, selected, worldPos.x, worldPos.y, projectPPM);
                m_App->SetSelectedObject(clicked);
            }
        }
    }

    // Resolve a pending drag: promote to real drag on movement, cycle on release.
    if (m_PendingDrag)
    {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, -1.0f))
        {
            m_IsDragging = true;
            m_Gizmos->SetDraggingAxis(GizmoAxis::XY);
            m_PendingDrag = false;
        }
        else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            ImVec2 worldPos = m_Coords->ScreenToWorld(m_PendingDragStartScreen.x, m_PendingDragStartScreen.y);
            Deki::Object* clicked = m_Selection->HandleClick(allObjects, selected, worldPos.x, worldPos.y, projectPPM);
            m_App->SetSelectedObject(clicked);
            m_PendingDrag = false;
        }
        else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            m_PendingDrag = false;
        }
    }

    selected = m_App->GetSelectedObject();

    // Handle dragging
    if (m_IsDragging)
    {
        if (selected && ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            ImVec2 mousePos = ImGui::GetMousePos();
            ImVec2 worldPos = m_Coords->ScreenToWorld(mousePos.x, mousePos.y);

            float newWorldX = selected->GetWorldX();
            float newWorldY = selected->GetWorldY();

            GizmoAxis draggingAxis = m_Gizmos->GetDraggingAxis();
            if (draggingAxis == GizmoAxis::X || draggingAxis == GizmoAxis::XY)
            {
                newWorldX = worldPos.x + m_DragOffsetX;
            }
            if (draggingAxis == GizmoAxis::Y || draggingAxis == GizmoAxis::XY)
            {
                newWorldY = worldPos.y + m_DragOffsetY;
            }

            // Snap to device screen edges and other objects when Ctrl is held.
            // Snap system speaks world meters; convert pixel bounds/sizes.
            if (io.KeyCtrl)
            {
                float objW = 0.0f, objH = 0.0f;
                m_Measurer->GetDisplaySize(selected, objW, objH);
                m_Snapping->SetDeviceBounds(
                    static_cast<float>(vpWidth) * invPPM,
                    static_cast<float>(vpHeight) * invPPM);
                m_Snapping->SetSnapThreshold(4.0f * invPPM);
                m_Snapping->SetObjectTargets(buildObjectSnapTargets(selected));
                m_Snapping->CalculateSnap(newWorldX, newWorldY,
                                          objW * selected->GetWorldScaleX() * invPPM,
                                          objH * selected->GetWorldScaleY() * invPPM);
            }
            else
            {
                m_Snapping->ClearSnapLines();
                m_Snapping->SetObjectTargets({});
            }

            // Snap last so Ctrl-snap results (in meters) also land on whole
            // pixels when the project is in Pixels mode.
            newWorldX = SnapMetersToPixel(newWorldX);
            newWorldY = SnapMetersToPixel(newWorldY);
            selected->SetWorldPosition(newWorldX, newWorldY);
        }
        else
        {
            m_Snapping->ClearSnapLines();
            m_Snapping->SetObjectTargets({});

            if (selected)
            {
                bool posChanged = (selected->GetWorldX() != m_DragStartObjX || selected->GetWorldY() != m_DragStartObjY);
                if (posChanged)
                {
                    float finalLocalX = selected->GetX();
                    float finalLocalY = selected->GetY();

                    selected->SetWorldPosition(m_DragStartObjX, m_DragStartObjY);

                    CommandHistory::Instance().Execute(
                        std::make_unique<SetPositionCommand>(selected, finalLocalX, finalLocalY));
                }
            }
            m_IsDragging = false;
            m_Gizmos->SetDraggingAxis(GizmoAxis::None);
        }
    }

    // Drawing
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    drawList->PushClipRect(m_CanvasTopLeft,
                           ImVec2(m_CanvasTopLeft.x + m_CanvasSize.x, m_CanvasTopLeft.y + m_CanvasSize.y), true);

    drawList->AddRectFilled(m_CanvasTopLeft,
                            ImVec2(m_CanvasTopLeft.x + m_CanvasSize.x, m_CanvasTopLeft.y + m_CanvasSize.y),
                            IM_COL32(30, 30, 30, 255));

    uint8_t clearR = 49, clearG = 77, clearB = 121;
    if (sceneCam)
    {
        sceneCam->GetClearColor(clearR, clearG, clearB);
    }

    // Device preview rect at world (0,0) is only shown when the scene has no
    // camera — the camera gizmo replaces it (see CameraCustomEditor).
    if (!sceneCam && m_Toolbar->ShowGizmos())
    {
        float halfDevW = SceneViewConstants::DeviceScreenWidth * 0.5f;
        float halfDevH = SceneViewConstants::DeviceScreenHeight * 0.5f;
        ImVec2 devTopLeft = m_Coords->WorldToScreen(-halfDevW, halfDevH);
        ImVec2 devBottomRight = m_Coords->WorldToScreen(halfDevW, -halfDevH);

        drawList->AddRectFilled(devTopLeft, devBottomRight,
                                IM_COL32(clearR, clearG, clearB, 255));

        ImU32 overlayColor = IM_COL32(0, 0, 0, 150);

        if (devTopLeft.y > sceneTop)
            drawList->AddRectFilled(ImVec2(sceneLeft, sceneTop), ImVec2(sceneRight, devTopLeft.y), overlayColor);
        if (devBottomRight.y < sceneBottom)
            drawList->AddRectFilled(ImVec2(sceneLeft, devBottomRight.y), ImVec2(sceneRight, sceneBottom), overlayColor);

        float overlayTop = (std::max)(sceneTop, devTopLeft.y);
        float overlayBottom = (std::min)(sceneBottom, devBottomRight.y);
        if (devTopLeft.x > sceneLeft)
            drawList->AddRectFilled(ImVec2(sceneLeft, overlayTop), ImVec2(devTopLeft.x, overlayBottom), overlayColor);
        if (devBottomRight.x < sceneRight)
            drawList->AddRectFilled(ImVec2(devBottomRight.x, overlayTop), ImVec2(sceneRight, overlayBottom), overlayColor);

        // Device bounds in the theme accent rather than a hardcoded orange, so
        // the overlay belongs to the same palette as the rest of the editor.
        const ImU32 deviceColor = ImGui::ColorConvertFloat4ToU32(DekiEditor::Palette::Accent);

        ImVec2 borderTopLeft(devTopLeft.x - 0.5f, devTopLeft.y - 0.5f);
        ImVec2 borderBottomRight(devBottomRight.x + 0.5f, devBottomRight.y + 0.5f);
        drawList->AddRect(borderTopLeft, borderBottomRight, deviceColor, 0.0f, 0, 1.0f);

        char deviceLabel[64];
        snprintf(deviceLabel, sizeof(deviceLabel), "%dx%d",
                 SceneViewConstants::DeviceScreenWidth, SceneViewConstants::DeviceScreenHeight);
        drawList->AddText(ImVec2(devTopLeft.x + 4, devTopLeft.y + 4), deviceColor, deviceLabel);
    }

    // Play mode short-circuit: SceneView mirrors PlayView's texture rather
    // than running its own scene render. Overlays (gizmos, outlines, grid)
    // are suppressed because they reference the editor camera's view of the
    // world, while the play texture comes from the scene camera. The user
    // can still pan/zoom this image (it scales like a captured frame); world
    // editing is meaningless during play, so input still drives the camera
    // but selection/drag has no visible effect until Stop.
    if (m_App->IsPlaying())
    {
        uint32_t playTexId = m_App->GetPlayModeTextureId();
        int playW = m_App->GetPlayModeWidth();
        int playH = m_App->GetPlayModeHeight();
        if (playTexId != 0 && playW > 0 && playH > 0)
        {
            float texScreenW = playW * effectiveZoom;
            float texScreenH = playH * effectiveZoom;
            float devCenterX = m_PanOffsetX + sceneWidth * 0.5f;
            float devCenterY = m_PanOffsetY + sceneHeight * 0.5f;
            ImVec2 p1(std::floor(m_CanvasTopLeft.x + devCenterX - texScreenW * 0.5f),
                      std::floor(m_CanvasTopLeft.y + devCenterY - texScreenH * 0.5f));
            ImVec2 p2(p1.x + texScreenW, p1.y + texScreenH);

            drawList->AddCallback([](const ImDrawList*, const ImDrawCmd*)
                                  {
                typedef void (APIENTRY *PFN_glBindSampler)(GLuint unit, GLuint sampler);
                static auto fn = (PFN_glBindSampler)SDL_GL_GetProcAddress("glBindSampler");
                if (fn) fn(0, 0); }, nullptr);
            drawList->AddImage((ImTextureID)(intptr_t)playTexId, p1, p2);
            drawList->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
            drawList->AddRect(p1, p2, IM_COL32(100, 100, 100, 255), 0.0f, 0, 1.0f);
        }

        drawList->PopClipRect();

        if (isCanvasHovered)
        {
            ImVec2 mousePos = ImGui::GetMousePos();
            ImVec2 worldPos = m_Coords->ScreenToWorld(mousePos.x, mousePos.y);
            ImGui::SetCursorScreenPos(ImVec2(m_CanvasTopLeft.x + 5, m_CanvasTopLeft.y + m_CanvasSize.y - 20));
            ImGui::Text("World: (%.1f, %.1f)", worldPos.x, worldPos.y);
        }

        // Edit-mode buffer is stale once we re-enter edit mode — force a
        // fresh render on the first Stop frame.
        m_EditRenderHasRendered = false;
        return;
    }

    // Edit mode: ensure assets are loaded via reflection-based asset
    // resolution. ProcessInternalAwake is idempotent — only loads unloaded
    // assets.
    m_EditModeLifecycle->ProcessInternalAwake(scene);

    // Calculate render buffer size to cover the full canvas area (not just
    // device bounds). Forced even so the camera's center (bufferWidth * 0.5)
    // lands on an integer buffer pixel — odd widths put quad screen
    // positions on half-pixels, and lround() then splits adjacent tiles to
    // opposite sides of the buffer-center boundary, producing 1-pixel gaps
    // (visible as clear-color stripes at tile joins).
    int renderWidth = static_cast<int>(std::ceil(m_CanvasSize.x / effectiveZoom)) + 2;
    int renderHeight = static_cast<int>(std::ceil(m_CanvasSize.y / effectiveZoom)) + 2;
    if (renderWidth & 1) ++renderWidth;
    if (renderHeight & 1) ++renderHeight;
    if (renderWidth < 2) renderWidth = 2;
    if (renderHeight < 2) renderHeight = 2;

    // Position camera at the world location corresponding to canvas center
    // This ensures the buffer always contains what's visible, allowing objects
    // placed anywhere in the world to be seen when panning.
    // canvas pixels → buffer pixels (÷ effectiveZoom) → world units (÷ PPM)
    float canvasCenterX = m_CanvasSize.x * 0.5f;
    float canvasCenterY = m_CanvasSize.y * 0.5f;
    float deviceCenterX = m_PanOffsetX + sceneWidth * 0.5f;
    float deviceCenterY = m_PanOffsetY + sceneHeight * 0.5f;
    const float canvasToWorld = (effectiveZoom * projectPPM > 0.0f)
                                    ? (1.0f / (effectiveZoom * projectPPM))
                                    : 0.0f;

    // World position at canvas center (Y inverted for screen-to-world)
    float camWorldX = (canvasCenterX - deviceCenterX) * canvasToWorld;
    float camWorldY = -(canvasCenterY - deviceCenterY) * canvasToWorld;

    // Lazy init: panel construction may precede package load. The early bail-out
    // at the top of DrawSceneCanvas guarantees CameraComponent is registered
    // by the time we reach here.
    if (!m_EditorCamera)
    {
        m_EditorCameraObject->AddComponent("CameraComponent");
        m_EditorCamera = m_EditorCameraObject->FindInterface<Deki::ICamera>();
        if (!m_EditorCamera) return;
    }

    // Snap the editor camera to whole buffer-pixel boundaries (1/projectPPM
    // meter). This is critical because the renderer's per-quad pixel_snap
    // rounds each blit position independently with lround(); when the camera
    // is at a sub-buffer-pixel offset, adjacent tiles/sprites can round in
    // opposite directions and produce 1-pixel gaps (visible as clear-color
    // stripes). Snapping the camera makes every quad's screen position land
    // on an integer buffer pixel, eliminating the rounding mismatch.
    const float snap = (projectPPM > 0.0f) ? projectPPM : 1.0f;
    const float invSnap = 1.0f / snap;
    const float camRenderX = std::floor(camWorldX * snap) * invSnap;
    const float camRenderY = std::floor(camWorldY * snap) * invSnap;

    m_EditorCameraObject->SetWorldPosition(camRenderX, camRenderY);
    // Render the scene edit-mode buffer at the project's native scale; the
    // editor's wheel-zoom is applied separately when uploading the texture.
    m_EditorCamera->SetPixelsPerMeter(Deki::EngineSettings::Global().pixelsPerMeter);
    m_EditorCamera->SetClearColor(clearR, clearG, clearB);

    // Initialize/resize renderer if needed. A buffer resize forces a re-render.
    bool sizeChanged = false;
    if (!m_EditModeRenderer->IsInitialized() ||
        m_EditModeRenderer->GetWidth() != renderWidth ||
        m_EditModeRenderer->GetHeight() != renderHeight)
    {
        m_EditModeRenderer->Initialize(renderWidth, renderHeight);
        sizeChanged = true;
    }

    // Edit-mode render dirty flag: only call RenderScene when something
    // visible to the buffer actually changed since last render. Drag-active
    // is included because drags mutate selected object's world position
    // every frame outside the command system.
    const bool renderNeeded = !m_EditRenderHasRendered || sizeChanged || m_LastEditRenderedScene != scene || m_LastEditRenderCamX != camRenderX || m_LastEditRenderCamY != camRenderY || m_LastEditRenderClearR != clearR || m_LastEditRenderClearG != clearG || m_LastEditRenderClearB != clearB || m_LastEditRenderGeneration != curGen || m_IsDragging || m_Gizmos->IsDraggingResize()
                              // Inspector live-edits (transform fields + reflection-based property
                              // widgets) mutate object state every frame without pushing a command,
                              // so the generation counter stays put. Forcing a re-render while any
                              // Inspector widget is active keeps the buffer in sync with the gizmos.
                              || InspectorPanel::IsAnyLiveEditing();

    if (renderNeeded)
    {
        m_EditModeRenderer->RenderScene(scene, m_EditorCamera, clearR, clearG, clearB);
        m_EditRenderHasRendered = true;
        m_LastEditRenderedScene = scene;
        m_LastEditRenderCamX = camRenderX;
        m_LastEditRenderCamY = camRenderY;
        m_LastEditRenderWidth = renderWidth;
        m_LastEditRenderHeight = renderHeight;
        m_LastEditRenderClearR = clearR;
        m_LastEditRenderClearG = clearG;
        m_LastEditRenderClearB = clearB;
        m_LastEditRenderGeneration = curGen;
    }

    // Display the rendered texture using WorldToScreen with the same snapped
    // camera position used for rendering — keeps the off-screen render and
    // on-screen ImGui display pixel-aligned even as the camera drifts.
    ImVec2 bufferCanvasTopLeft, bufferCanvasBottomRight;
    m_Coords->GetBufferScreenBounds(renderWidth, renderHeight, camRenderX, camRenderY,
                                    bufferCanvasTopLeft, bufferCanvasBottomRight);

    uint32_t texId = m_EditModeRenderer->GetTextureId();
    if (texId != 0)
    {
        // ImGui's GL backend binds a GL_LINEAR sampler object that overrides the texture's
        // own GL_NEAREST params, blurring the scene when zoomed. Unbind the sampler for
        // this draw so the texture's nearest-neighbor filtering takes effect.
        drawList->AddCallback([](const ImDrawList*, const ImDrawCmd*)
                              {
            typedef void (APIENTRY *PFN_glBindSampler)(GLuint unit, GLuint sampler);
            static auto fn = (PFN_glBindSampler)SDL_GL_GetProcAddress("glBindSampler");
            if (fn) fn(0, 0); }, nullptr);
        drawList->AddImage((ImTextureID)(intptr_t)texId, bufferCanvasTopLeft, bufferCanvasBottomRight,
                           ImVec2(0, 0), ImVec2(1, 1));
        drawList->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    }

    // Draw component outlines when toggle is on
    if (m_Toolbar->ShowOutlines())
    {
        m_Gizmos->DrawComponentOutlines(drawList, scene, m_EditorCamera, renderWidth, renderHeight,
                                        bufferCanvasTopLeft);
    }

    // Always draw the camera's rect (regardless of selection) when gizmos are on.
    // TODO: replace with CustomEditor::OnDrawGizmos dispatch once project package
    // DLLs are rebuilt against the new CustomEditor header (calling the new
    // virtual on a stale package DLL crashes — its vtable lacks the slot).
    if (m_Toolbar->ShowGizmos())
    {
        if (auto* camObj = GetSceneCameraObject())
        {
            if (camObj != selected)
            {
                m_Gizmos->DrawSelectionBounds(drawList, camObj, m_EditorCamera, renderWidth, renderHeight,
                                              bufferCanvasTopLeft);
            }
        }
    }

    // Dispatch CustomEditor::OnDrawGizmos / OnDrawGizmosSelected. Walks the
    // scene tree, sets up SceneView::BeginObject for each object so editors
    // can use SceneView::Get() to read screen position/zoom, and routes calls
    // to whichever editor is registered for that component type.
    if (m_Toolbar->ShowGizmos() && scene && m_EditorCamera)
    {
        auto& editorRegistry = DekiEditor::CustomEditorRegistry::Instance();
        auto& compRegistry = Deki::ComponentRegistry::Instance();
        auto& view = DekiEditor::SceneView::Get();
        const float zoom = m_Coords->GetEffectiveZoom();

        // Self-passing lambda: recursive without std::function's heap
        // allocation and indirect call per object, every frame.
        auto visit = [&](auto&& self, Deki::Object* obj) -> void
        {
            if (!obj || !obj->IsActive()) return;

            // Convert world → buffer pixel → canvas screen (matches the same
            // coord chain that DrawComponentOutlines uses).
            float bufX, bufY;
            m_EditorCamera->WorldToScreen(obj->GetWorldX(), obj->GetWorldY(),
                                          renderWidth, renderHeight, bufX, bufY);
            float screenX = bufferCanvasTopLeft.x + bufX * zoom;
            float screenY = bufferCanvasTopLeft.y + bufY * zoom;

            float displayW = 0, displayH = 0;
            m_Measurer->GetDisplaySize(obj, displayW, displayH);

            DekiEditor::SceneViewContext ctx;
            ctx.object = obj;
            ctx.drawList = drawList;
            ctx.screenX = screenX;
            ctx.screenY = screenY;
            ctx.worldX = obj->GetWorldX();
            ctx.worldY = obj->GetWorldY();
            ctx.scaleX = obj->GetWorldScaleX();
            ctx.scaleY = obj->GetWorldScaleY();
            ctx.rotation = obj->GetWorldRotation();
            ctx.zoom = zoom;
            ctx.pixelsPerMeter = projectPPM;
            ctx.displayWidth = displayW;
            ctx.displayHeight = displayH;
            ctx.isSelected = (obj == selected);

            view.BeginObject(ctx);
            for (Deki::Component* comp : obj->GetComponents())
            {
                const Deki::ComponentMeta* meta = compRegistry.GetMeta(comp->GetType());
                if (!meta || !meta->name) continue;
                DekiEditor::CustomEditor* ed = editorRegistry.GetEditorForComponent(meta->name, comp);
                if (!ed) continue;
                ed->OnDrawGizmos(comp);
                if (ctx.isSelected)
                    ed->OnDrawGizmosSelected(comp);
            }
            view.EndObject();

            for (Deki::Object* child : obj->GetChildren())
                self(self, child);
        };

        for (Deki::Object* root : scene->GetObjects())
            visit(visit, root);
    }

    // Draw gizmos
    if (selected && selected->IsActive())
    {
        // Selection bounds (skipped when gizmos are toggled off)
        if (m_Toolbar->ShowGizmos())
        {
            m_Gizmos->DrawSelectionBounds(drawList, selected, m_EditorCamera, renderWidth, renderHeight,
                                          bufferCanvasTopLeft);
        }

        // Update snapping device bounds and object targets for resize.
        // Convert pixel viewport size to world meters so snap targets are
        // comparable with world-space edges computed in HandleResizeDrag.
        m_Snapping->SetDeviceBounds(
            static_cast<float>(vpWidth) * invPPM,
            static_cast<float>(vpHeight) * invPPM);
        m_Snapping->SetSnapThreshold(4.0f * invPPM);

        if (m_Gizmos->IsDraggingResize() && io.KeyCtrl)
            m_Snapping->SetObjectTargets(buildObjectSnapTargets(selected));
        else if (!m_IsDragging)
            m_Snapping->SetObjectTargets({});

        // Draw tool-specific gizmos
        switch (m_Toolbar->GetCurrentTool())
        {
            case SceneTool::Select:
                break;
            case SceneTool::Move:
                m_Gizmos->DrawTranslateGizmo(drawList, selected);
                break;
            case SceneTool::Rect:
                m_Gizmos->DrawResizeGizmo(drawList, selected, m_Toolbar->RectAnchorEdge());
                break;
        }
    }

    // Draw snap guide lines
    m_Snapping->DrawSnapGuides(drawList, m_Coords.get(), m_CanvasTopLeft, m_CanvasSize);

    if (!sceneCam && m_Toolbar->ShowGizmos())
    {
        drawList->AddRect(ImVec2(sceneLeft, sceneTop), ImVec2(sceneRight, sceneBottom),
                          IM_COL32(100, 100, 100, 255), 0.0f, 0, 2.0f);
    }

    // Draw grid
    if (m_Toolbar->ShowGrid() && m_Toolbar->GetGridSize() > 0)
    {
        float gridStep = m_Toolbar->GetGridSize() * effectiveZoom;
        ImU32 gridColor = IM_COL32(255, 255, 255, 40);
        ImU32 gridColorMajor = IM_COL32(255, 255, 255, 80);

        float canvasLeft = m_CanvasTopLeft.x;
        float canvasTop = m_CanvasTopLeft.y;
        float canvasRight = m_CanvasTopLeft.x + m_CanvasSize.x;
        float canvasBottom = m_CanvasTopLeft.y + m_CanvasSize.y;

        float offsetX = fmod(sceneLeft - canvasLeft, gridStep);
        float offsetY = fmod(sceneTop - canvasTop, gridStep);

        for (float x = canvasLeft + offsetX; x <= canvasRight; x += gridStep)
        {
            float worldX = (x - sceneLeft) / effectiveZoom;
            bool isMajor = (std::abs(fmod(worldX, m_Toolbar->GetGridSize() * 4)) < 0.5f);
            drawList->AddLine(ImVec2(x, canvasTop), ImVec2(x, canvasBottom),
                              isMajor ? gridColorMajor : gridColor);
        }

        for (float y = canvasTop + offsetY; y <= canvasBottom; y += gridStep)
        {
            float worldY = (y - sceneTop) / effectiveZoom;
            bool isMajor = (std::abs(fmod(worldY, m_Toolbar->GetGridSize() * 4)) < 0.5f);
            drawList->AddLine(ImVec2(canvasLeft, y), ImVec2(canvasRight, y),
                              isMajor ? gridColorMajor : gridColor);
        }

        // Draw pixel grid when zoomed in enough (1 world pixel = 1 cell)
        if (effectiveZoom >= 4.0f)
        {
            ImU32 pixelGridColor = IM_COL32(255, 255, 255, 20);
            float pixelStep = effectiveZoom;  // 1 world pixel in physical pixels

            float pixelOffsetX = fmod(sceneLeft - canvasLeft, pixelStep);
            float pixelOffsetY = fmod(sceneTop - canvasTop, pixelStep);

            for (float x = canvasLeft + pixelOffsetX; x <= canvasRight; x += pixelStep)
            {
                drawList->AddLine(ImVec2(std::floor(x), canvasTop), ImVec2(std::floor(x), canvasBottom),
                                  pixelGridColor);
            }

            for (float y = canvasTop + pixelOffsetY; y <= canvasBottom; y += pixelStep)
            {
                drawList->AddLine(ImVec2(canvasLeft, std::floor(y)), ImVec2(canvasRight, std::floor(y)),
                                  pixelGridColor);
            }
        }
    }

    drawList->PopClipRect();

    if (isCanvasHovered)
    {
        ImVec2 mousePos = ImGui::GetMousePos();
        ImVec2 worldPos = m_Coords->ScreenToWorld(mousePos.x, mousePos.y);

        ImGui::SetCursorScreenPos(ImVec2(m_CanvasTopLeft.x + 5, m_CanvasTopLeft.y + m_CanvasSize.y - 20));
        ImGui::Text("World: (%.1f, %.1f)", worldPos.x, worldPos.y);
    }
}

uint32_t SceneViewPanel::GetEditTextureId() const
{
    return m_EditModeRenderer ? m_EditModeRenderer->GetTextureId() : 0;
}

int SceneViewPanel::GetEditTextureWidth() const
{
    return m_EditModeRenderer ? m_EditModeRenderer->GetWidth() : 0;
}

int SceneViewPanel::GetEditTextureHeight() const
{
    return m_EditModeRenderer ? m_EditModeRenderer->GetHeight() : 0;
}

}  // namespace DekiEditor
