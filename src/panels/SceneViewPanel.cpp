#include "ObjectInternal.h"
#include "../services/PackageLifetime.h"
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
#include <deki-editor/Motion.h>
#include <chrono>
#include "../core/Units.h"
#include "InspectorPanel.h"
#include <deki-editor/CommandHistory.h>
#include "core/Commands.h"
#include "core/Project.h"
#include "services/TouchInput.h"
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
#include "imgui_internal.h"  // ImHashStr
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
    {
        return sortable->GetSortingOrder();
    }
    return 0;
}

namespace
{
// Frame of the last Scene view object drag/resize; -2 so frame 0 is not "editing".
int s_EditingObjectsFrame = -2;

float Saturate(double v)
{
    return static_cast<float>(v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v));
}

// How strongly to draw a grid line whose neighbours on its own level are
// `spacing` screen pixels away. Minor lines fade in over [min, 2*min], major
// lines strengthen over [4*min, 8*min]. Driven by the line's own spacing, so
// zooming changes every line continuously - nothing pops between levels -
// and at 1:1 it is the old look (16 px cells at 40, every 4th line at 80).
float GridLineAlpha(double spacing, float minSpacing)
{
    constexpr float kMinorAlpha = 40.0f, kMajorAlpha = 80.0f;
    return kMinorAlpha * Saturate((spacing - minSpacing) / minSpacing) +
           (kMajorAlpha - kMinorAlpha) * Saturate((spacing - 4.0 * minSpacing) / (4.0 * minSpacing));
}

// One axis of the grid. `origin` is the screen position of line 0, `cell` the
// finest spacing, [lo, hi] the visible range along the axis and [spanLo,
// spanHi] across it. Lines are placed by index - never accumulated - and
// coarsened by 4 until they are at least `minSpacing` apart, so there are a
// few hundred at most at any zoom. Each is snapped to a pixel centre so it
// stays a crisp single pixel instead of smearing over two faint ones.
void DrawGridAxis(ImDrawList* dl, bool vertical, double origin, double cell, float lo, float hi, float spanLo,
                  float spanHi, float minSpacing)
{
    if (cell <= 0.0)
    {
        return;
    }
    double base = cell;
    int guard = 0;
    while (base < minSpacing && guard++ < 32)
    {
        base *= 4.0;
    }

    const long long first = static_cast<long long>(std::ceil((lo - origin) / base));
    const long long last = static_cast<long long>(std::floor((hi - origin) / base));
    for (long long i = first; i <= last; ++i)
    {
        // The line's own level: the most times 4 divides its index. Line 0 is
        // on every level.
        double spacing = base;
        long long j = i;
        for (int k = 0; k < 16 && j != 0 && j % 4 == 0; ++k)
        {
            j /= 4;
            spacing *= 4.0;
        }
        if (j == 0)
        {
            spacing = 1e9;
        }

        const float alpha = GridLineAlpha(spacing, minSpacing);
        if (alpha < 1.0f)
        {
            continue;
        }
        const ImU32 col = IM_COL32(255, 255, 255, static_cast<int>(alpha + 0.5f));
        const float p = std::floor(static_cast<float>(origin + static_cast<double>(i) * base)) + 0.5f;
        if (vertical)
        {
            dl->AddLine(ImVec2(p, spanLo), ImVec2(p, spanHi), col);
        }
        else
        {
            dl->AddLine(ImVec2(spanLo, p), ImVec2(spanHi, p), col);
        }
    }
}

// Pixel grid: one line per world pixel, fading in between 2.5x and 4x (the
// maximum zoom, where it used to switch on at full strength) instead of
// appearing all at once.
void DrawPixelGridAxis(ImDrawList* dl, bool vertical, double origin, float pixelSize, float lo, float hi, float spanLo,
                       float spanHi)
{
    const float alpha = 20.0f * Saturate((pixelSize - 2.5f) / 1.5f);
    if (alpha < 1.0f)
    {
        return;
    }
    const ImU32 col = IM_COL32(255, 255, 255, static_cast<int>(alpha + 0.5f));
    const long long first = static_cast<long long>(std::ceil((lo - origin) / pixelSize));
    const long long last = static_cast<long long>(std::floor((hi - origin) / pixelSize));
    for (long long i = first; i <= last; ++i)
    {
        const float p = std::floor(static_cast<float>(origin + static_cast<double>(i) * pixelSize)) + 0.5f;
        if (vertical)
        {
            dl->AddLine(ImVec2(p, spanLo), ImVec2(p, spanHi), col);
        }
        else
        {
            dl->AddLine(ImVec2(spanLo, p), ImVec2(spanHi, p), col);
        }
    }
}
}  // namespace

bool SceneViewPanel::IsEditingObjects()
{
    return s_EditingObjectsFrame >= ImGui::GetFrameCount() - 1;
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
    m_EditModeRenderer->SetStreaming(true);  // the buffer resizes with every zoom step
    m_EditModeLifecycle = std::make_unique<Deki::LifecycleManager>();

    // Create invisible editor camera (never serialized)
    m_EditorCameraObject = std::make_unique<Deki::Object>();
    m_EditorCameraObject->SetName("__EditorCamera");
    m_EditorCameraObject->SetEditorOnly(true);  // Never saved to scene

    // Camera component created lazily in DrawSceneCanvas() - packages may not be loaded yet
    // and is dropped before any package unloads (its code lives in deki-rendering).
    PackageLifetime::OnBeforeUnload(this, [this]() { InvalidateEditorCamera(); });
}

SceneViewPanel::~SceneViewPanel()
{
    PackageLifetime::Forget(this);
}

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
    {
        m_Measurer->UpdateEditors(scene);
    }
}

void SceneViewPanel::SetCurrentTool(SceneTool tool)
{
    if (m_Toolbar)
    {
        m_Toolbar->SetCurrentTool(tool);
    }
}

void SceneViewPanel::InvalidateTexture(const std::string& assetRef)
{
    m_TextureCache->Invalidate(assetRef);
}

void SceneViewPanel::InvalidateAllAssets()
{
    auto* scene = m_App->GetCurrentScene();
    if (!scene)
    {
        return;
    }

    // Recursively unload assets from all objects in the scene
    std::function<void(Deki::Object*)> unloadRecursive = [&](Deki::Object* obj)
    {
        if (!obj)
        {
            return;
        }

        // Unload assets from all components and reset resolution state
        for (Deki::Component* comp : obj->GetComponents())
        {
            comp->UnloadAssets();
            comp->ResetRefsResolved();
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
    {
        return;
    }

    m_SceneCamScene = scene;
    m_SceneCamGeneration = gen;
    m_SceneCam = nullptr;
    m_SceneCamObject = nullptr;
    if (!scene)
    {
        return;
    }

    // Search recursively through all objects via ICamera interface
    for (Deki::Object* obj : scene->GetObjects())
    {
        Deki::Object* holder =
            FindInSubtree(obj, [](Deki::Object* o) { return o->FindInterface<Deki::ICamera>() != nullptr; });
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
    if (!isOpen)
    {
        return;
    }

    bool visible = ImGui::Begin("SCENE VIEW###Scene View", &isOpen,
                                DekiEditor::MaximizedWindowFlags("SCENE VIEW###Scene View") |
                                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

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
        {
            m_Toolbar->SetCurrentTool(SceneTool::Select);
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_W))
        {
            m_Toolbar->SetCurrentTool(SceneTool::Move);
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_R))
        {
            m_Toolbar->SetCurrentTool(SceneTool::Rect);
        }
    }

    // Delete key. Goes through EditorApp::DeleteObject so the protection rules
    // (scene root, boot Setup, instance children) apply here exactly as in
    // the Hierarchy - this path used to have none.
    Deki::Object* selected = m_App->GetSelectedObject();
    if (selected && isCanvasHovered && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Delete))
    {
        m_App->DeleteObjects(m_App->GetSelectedObjects());
    }

    // Panning with Alt+Left, Middle or Right mouse
    bool startPan = isCanvasHovered &&
                    ((ImGui::IsMouseClicked(ImGuiMouseButton_Left) && io.KeyAlt) ||
                     ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || ImGui::IsMouseClicked(ImGuiMouseButton_Right));

    if (startPan)
    {
        m_IsPanning = true;
        m_LastPanPos = io.MousePos;
    }

    if (m_IsPanning)
    {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Middle) ||
            ImGui::IsMouseDown(ImGuiMouseButton_Right))
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

    // Zooming with Mouse Wheel: each notch moves the target, and the zoom eases
    // there below, around the world point that was under the cursor.
    const ImGuiID zoomKey = ImHashStr("SceneViewZoom");
    if (isCanvasHovered && io.MouseWheel != 0)
    {
        m_ZoomAnchorScreen = io.MousePos;
        m_ZoomAnchorWorld = m_Coords->ScreenToWorld(io.MousePos.x, io.MousePos.y);

        // Scale by wheel delta so high-precision wheels/trackpads don't rocket
        // through the zoom range. Ctrl is the precision modifier (smaller step).
        float zoomBase = io.KeyCtrl ? 1.03f : 1.10f;
        m_ZoomTarget = std::clamp(m_ZoomTarget * std::pow(zoomBase, io.MouseWheel), SceneViewConstants::kMinZoom,
                                  SceneViewConstants::kMaxZoom);
        DekiEditor::Motion::Set(zoomKey, m_Zoom);  // continue from where it is
    }

    // Two fingers on a touchscreen: the midpoint drags the view, the pinch
    // zooms about it. Applied directly, not eased: the scene has to stay under
    // the fingers, the way a map does.
    DekiEditor::TouchInput::Gesture touch;
    if (DekiEditor::TouchInput::Get().ClaimGesture(
            m_CanvasTopLeft, ImVec2(m_CanvasTopLeft.x + m_CanvasSize.x, m_CanvasTopLeft.y + m_CanvasSize.y), touch))
    {
        // The world point that was under the midpoint goes to where the
        // midpoint is now, at the new zoom: that one move is both the pan and
        // the zoom-about-the-fingers. m_Coords still holds this frame's view
        // from before the gesture, which is the view the old midpoint was in.
        const ImVec2 before(touch.center.x - touch.pan.x, touch.center.y - touch.pan.y);
        const ImVec2 world = m_Coords->ScreenToWorld(before.x, before.y);
        m_Zoom = m_ZoomTarget =
            std::clamp(m_Zoom * touch.scale, SceneViewConstants::kMinZoom, SceneViewConstants::kMaxZoom);
        DekiEditor::Motion::Set(zoomKey, m_Zoom);
        KeepWorldPointAt(world, touch.center);
    }

    if (m_Zoom != m_ZoomTarget)
    {
        m_Zoom = DekiEditor::Motion::Follow(zoomKey, m_ZoomTarget, DekiEditor::Motion::kNormal, m_ZoomTarget * 0.001f);
        KeepWorldPointAt(m_ZoomAnchorWorld, m_ZoomAnchorScreen);
    }
}

void SceneViewPanel::KeepWorldPointAt(const ImVec2& world, const ImVec2& screen)
{
    // World→screen is editorZoom * dpi * the project's pixels per meter: the
    // scene view shows the world at its art density, whatever the camera does.
    // Must match SceneCoordinates::ScreenToWorld, or each zoom step drives the
    // pan offset toward inf/NaN.
    const int vpWidth = SceneViewConstants::kAnchorWidth;
    const int vpHeight = SceneViewConstants::kAnchorHeight;

    const float dpi = ImGui::GetWindowDpiScale();
    const float sceneWidth = vpWidth * m_Zoom * dpi;
    const float sceneHeight = vpHeight * m_Zoom * dpi;
    const float worldToScreen = m_Zoom * dpi * Deki::EngineSettings::Global().pixelsPerMeter;

    const float sceneCenterX = m_CanvasTopLeft.x + m_PanOffsetX + sceneWidth * 0.5f;
    const float sceneCenterY = m_CanvasTopLeft.y + m_PanOffsetY + sceneHeight * 0.5f;

    const float nowScreenX = sceneCenterX + world.x * worldToScreen;
    const float nowScreenY = sceneCenterY - world.y * worldToScreen;  // world Y+ is up, screen Y+ is down

    m_PanOffsetX += screen.x - nowScreenX;
    m_PanOffsetY += screen.y - nowScreenY;

    // NOTE: Don't apply pixel-perfect snapping here - it causes cumulative drift
    // when zooming in/out repeatedly. The rendering already handles pixel-perfect
    // positioning via flooring in WorldToScreen and GetBufferScreenBounds.
}

void SceneViewPanel::DrawSceneCanvas()
{
    auto* scene = m_App->GetCurrentScene();
    if (!scene)
    {
        return;
    }

    // Detect scene change and trigger centering
    if (m_LastScene != scene)
    {
        m_LastScene = scene;
        m_NeedsCentering = true;
        m_CenteringDelay = 3;  // Wait for docking layout to stabilize
    }

    ImVec2 availSize = ImGui::GetContentRegionAvail();
    if (availSize.x <= 0 || availSize.y <= 0)
    {
        return;
    }

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
    // The scene rectangle anchors the world origin (SceneViewConstants). The
    // previewed screen, drawn around the origin and snapped to, is the Play
    // view's.
    int vpWidth = SceneViewConstants::kAnchorWidth;
    int vpHeight = SceneViewConstants::kAnchorHeight;
    const int screenW = DekiEditor::SceneView::Get().GetPreviewWidth();
    const int screenH = DekiEditor::SceneView::Get().GetPreviewHeight();

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
            m_ZoomTarget = 1.0f;
            float sceneWidth = vpWidth * m_Zoom * dpi;
            float sceneHeight = vpHeight * m_Zoom * dpi;

            m_PanOffsetX = (availSize.x - sceneWidth) * 0.5f;
            m_PanOffsetY = (availSize.y - sceneHeight) * 0.5f;
            m_NeedsCentering = false;
        }
    }

    // CameraComponent's vtable lives in deki-rendering.dll. Querying ICamera on
    // a scene component before that package is fully loaded (e.g. mid project
    // open) would dereference an unmapped vtable. Bail out - the next frame
    // re-enters once the rendering package is registered.
    if (!Deki::ComponentRegistry::Instance().GetMetaByClassName("CameraComponent"))
    {
        return;
    }

    Deki::ICamera* sceneCam = GetSceneCamera();
    // Must match HandleInput's zoom-to-cursor math, or ScreenToWorld and the
    // post-zoom projection stop being inverses and each wheel notch drives the
    // pan offset toward inf/NaN.
    const float projectPPM = Deki::EngineSettings::Global().pixelsPerMeter;

    m_Coords->SetViewport(m_CanvasTopLeft, m_CanvasSize, m_Zoom, m_PanOffsetX, m_PanOffsetY, projectPPM);

    float effectiveZoom = m_Zoom * dpi;
    float sceneWidth = vpWidth * effectiveZoom;
    float sceneHeight = vpHeight * effectiveZoom;

    float sceneLeft = m_CanvasTopLeft.x + m_PanOffsetX;
    float sceneTop = m_CanvasTopLeft.y + m_PanOffsetY;

    ImGui::InvisibleButton("SceneCanvas", availSize,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle |
                               ImGuiButtonFlags_MouseButtonRight);
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

    // Flatten hierarchy for rendering/selection (cached - rebuilt only when
    // structure changes, not every frame). Drag-active also forces rebuild
    // because sortable children can be reordered mid-drag.
    ImGuiIO& io = ImGui::GetIO();
    const std::vector<Deki::Object*>& rootObjects = scene->GetObjects();
    const uint64_t curGen = CommandHistory::Instance().GetGeneration();
    const bool flatStale = (m_CachedFlatScene != scene) || (m_CachedFlatRootCount != rootObjects.size()) ||
                           (m_CachedFlatGeneration != curGen) || m_IsDragging || m_Gizmos->IsDraggingResize();
    if (flatStale)
    {
        m_CachedFlatObjects.clear();
        std::function<void(Deki::Object*)> collectObjects = [&](Deki::Object* obj)
        {
            m_CachedFlatObjects.push_back(obj);
            for (Deki::Object* child : obj->GetChildren())
            {
                collectObjects(child);
            }
        };
        for (Deki::Object* obj : rootObjects)
        {
            collectObjects(obj);
        }
        std::sort(m_CachedFlatObjects.begin(), m_CachedFlatObjects.end(),
                  [](Deki::Object* a, Deki::Object* b) { return GetSortingOrder(a) < GetSortingOrder(b); });
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
            if (obj == exclude)
            {
                continue;
            }
            float w = 0.0f, h = 0.0f;
            if (!m_Measurer->GetDisplaySize(obj, w, h))
            {
                continue;
            }
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
        {
            m_Gizmos->UpdateTranslateHover(selected, ImGui::GetMousePos());
        }
    }
    else if (m_Toolbar->GetCurrentTool() != SceneTool::Move)
    {
        m_Gizmos->SetHoveredAxis(GizmoAxis::None);
    }

    // Update resize gizmo hover state
    if (selected && isCanvasHovered && !m_IsDragging && !m_Gizmos->IsDraggingResize() &&
        m_Toolbar->GetCurrentTool() == SceneTool::Rect)
    {
        if (hoverNeedsUpdate)
        {
            m_Gizmos->UpdateResizeHover(selected);
        }
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

        // A finger in the Select tool: a drag pans rather than moving the
        // selected object, which a finger does far too easily by accident, and
        // selection waits for the lift so a pan leaves it alone. The mouse
        // keeps select-and-drag; moving by touch is the Move tool's gizmo.
        if (io.MouseSource == ImGuiMouseSource_TouchScreen && m_Toolbar->GetCurrentTool() == SceneTool::Select)
        {
            m_TouchPendingPan = true;
            m_TouchPressScreen = mousePos;
        }
        // Check resize handle first
        else if (selected && m_Gizmos->GetHoveredEdge() != ResizeEdge::None)
        {
            auto& registry = Deki::ComponentRegistry::Instance();
            for (Deki::Component* comp : selected->GetComponents())
            {
                const Deki::ComponentMeta* meta = registry.GetMeta(comp->GetType());
                if (!meta)
                {
                    continue;
                }

                float* widthPtr = nullptr;
                float* heightPtr = nullptr;
                if (FindResizeProperties(comp, meta, &widthPtr, &heightPtr))
                {
                    // 0 means "size from the content" (a sprite's own pixels):
                    // start the drag from the size drawn, or the first frame
                    // collapses the object to the cursor's offset from nothing.
                    float startW = *widthPtr, startH = *heightPtr;
                    float drawnW = 0.0f, drawnH = 0.0f;
                    if ((startW <= 0.0f || startH <= 0.0f) && m_Measurer->GetDisplaySize(selected, drawnW, drawnH))
                    {
                        const float ppm = Deki::EngineSettings::Global().pixelsPerMeter;
                        if (startW <= 0.0f && ppm > 0.0f)
                        {
                            startW = drawnW / ppm;
                        }
                        if (startH <= 0.0f && ppm > 0.0f)
                        {
                            startH = drawnH / ppm;
                        }
                    }
                    m_Gizmos->StartResizeDrag(m_Gizmos->GetHoveredEdge(), startW, startH, mousePos.x, mousePos.y,
                                              selected->GetWorldX(), selected->GetWorldY(), *widthPtr, *heightPtr);
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
                // A click, or the start of a box: the release decides.
                m_MarqueeAdditive = io.KeyShift || io.KeyCtrl;
                m_MarqueeBase = m_MarqueeAdditive ? m_App->GetSelectedObjects() : std::vector<Deki::Object*>{};
                m_MarqueePending = true;
                m_MarqueeActive = false;
                m_MarqueeStart = mousePos;
            }
        }
    }

    // Box select: live while the button is held; a release without a drag is
    // a click on what is under the press.
    if (m_MarqueePending)
    {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            if (!m_MarqueeActive)
            {
                const ImVec2 at = m_Coords->ScreenToWorld(m_MarqueeStart.x, m_MarqueeStart.y);
                Deki::Object* clicked = m_Selection->HandleClick(allObjects, m_MarqueeAdditive ? nullptr : selected,
                                                                 at.x, at.y, projectPPM);
                if (clicked && m_MarqueeAdditive)
                {
                    m_App->ToggleSelection(clicked);
                }
                else if (!m_MarqueeAdditive)
                {
                    m_App->SetSelectedObject(clicked);
                }
            }
            m_MarqueePending = m_MarqueeActive = false;
            m_MarqueeBase.clear();
        }
        else
        {
            if (!m_MarqueeActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left, -1.0f))
            {
                m_MarqueeActive = true;
            }
            if (m_MarqueeActive && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f || flatStale))
            {
                const ImVec2 a = m_Coords->ScreenToWorld(m_MarqueeStart.x, m_MarqueeStart.y);
                const ImVec2 b = m_Coords->ScreenToWorld(io.MousePos.x, io.MousePos.y);
                std::vector<Deki::Object*> next = m_MarqueeBase;
                for (Deki::Object* obj : m_Selection->ObjectsInRect(allObjects, a.x, a.y, b.x, b.y, projectPPM))
                {
                    if (std::find(next.begin(), next.end(), obj) == next.end())
                    {
                        next.push_back(obj);
                    }
                }
                if (next != m_App->GetSelectedObjects())
                {
                    m_App->SetSelectedObjects(next);
                }
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

    // Resolve a finger press in the Select tool: a drag becomes a pan (from the
    // press point, so the movement so far counts), a lift is a click there.
    if (m_TouchPendingPan)
    {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, -1.0f))
        {
            m_IsPanning = true;
            m_LastPanPos = m_TouchPressScreen;
            m_TouchPendingPan = false;
        }
        else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            ImVec2 worldPos = m_Coords->ScreenToWorld(m_TouchPressScreen.x, m_TouchPressScreen.y);
            Deki::Object* clicked = m_Selection->HandleClick(allObjects, selected, worldPos.x, worldPos.y, projectPPM);
            m_App->SetSelectedObject(clicked);
            m_TouchPendingPan = false;
        }
        else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            m_TouchPendingPan = false;
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
                m_Snapping->SetDeviceBounds(static_cast<float>(screenW) * invPPM, static_cast<float>(screenH) * invPPM);
                m_Snapping->SetSnapThreshold(4.0f * invPPM);
                m_Snapping->SetObjectTargets(buildObjectSnapTargets(selected));
                m_Snapping->CalculateSnap(newWorldX, newWorldY, objW * selected->GetWorldScaleX() * invPPM,
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
                bool posChanged =
                    (selected->GetWorldX() != m_DragStartObjX || selected->GetWorldY() != m_DragStartObjY);
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
    // camera - the camera gizmo replaces it (see CameraCustomEditor).
    if (!sceneCam && m_Toolbar->ShowGizmos())
    {
        // The previewed screen at the art density, drawn 1:1 around the origin.
        const float art = Deki::EngineSettings::Global().pixelsPerMeter;
        float halfDevW = art > 0.0f ? screenW * 0.5f / art : 0.0f;
        float halfDevH = art > 0.0f ? screenH * 0.5f / art : 0.0f;
        ImVec2 devTopLeft = m_Coords->WorldToScreen(-halfDevW, halfDevH);
        ImVec2 devBottomRight = m_Coords->WorldToScreen(halfDevW, -halfDevH);

        drawList->AddRectFilled(devTopLeft, devBottomRight, IM_COL32(clearR, clearG, clearB, 255));

        ImU32 overlayColor = IM_COL32(0, 0, 0, 150);

        // Darken the canvas outside the screen.
        const float cL = m_CanvasTopLeft.x, cT = m_CanvasTopLeft.y;
        const float cR = cL + m_CanvasSize.x, cB = cT + m_CanvasSize.y;
        if (devTopLeft.y > cT)
        {
            drawList->AddRectFilled(ImVec2(cL, cT), ImVec2(cR, devTopLeft.y), overlayColor);
        }
        if (devBottomRight.y < cB)
        {
            drawList->AddRectFilled(ImVec2(cL, devBottomRight.y), ImVec2(cR, cB), overlayColor);
        }

        float overlayTop = (std::max)(cT, devTopLeft.y);
        float overlayBottom = (std::min)(cB, devBottomRight.y);
        if (devTopLeft.x > cL)
        {
            drawList->AddRectFilled(ImVec2(cL, overlayTop), ImVec2(devTopLeft.x, overlayBottom), overlayColor);
        }
        if (devBottomRight.x < cR)
        {
            drawList->AddRectFilled(ImVec2(devBottomRight.x, overlayTop), ImVec2(cR, overlayBottom), overlayColor);
        }

        // Device bounds in the theme accent rather than a hardcoded orange, so
        // the overlay belongs to the same palette as the rest of the editor.
        const ImU32 deviceColor = ImGui::ColorConvertFloat4ToU32(DekiEditor::Palette::Accent);

        ImVec2 borderTopLeft(devTopLeft.x - 0.5f, devTopLeft.y - 0.5f);
        ImVec2 borderBottomRight(devBottomRight.x + 0.5f, devBottomRight.y + 0.5f);
        drawList->AddRect(borderTopLeft, borderBottomRight, deviceColor, 0.0f, 0, 1.0f);

        char deviceLabel[64];
        snprintf(deviceLabel, sizeof(deviceLabel), "%dx%d", screenW, screenH);
        drawList->AddText(ImVec2(devTopLeft.x + 4, devTopLeft.y + 4), deviceColor, deviceLabel);
    }

    // Play mode draws the live play scene through the editor camera, exactly
    // like edit mode, so the grid, gizmos and selection stay (Unity's Scene
    // view). The game's own lifecycle resolves the play scene's refs, so the
    // edit lifecycle stays out of it; when stopped or paused, ensure assets
    // are loaded via reflection-based asset resolution. ProcessInternalAwake
    // is idempotent - only loads unloaded assets.
    const bool playing = m_App->IsPlaying();
    if (!playing)
    {
        m_EditModeLifecycle->ProcessInternalAwake(scene);
    }

    // Calculate render buffer size to cover the full canvas area (not just
    // device bounds). Forced even so the camera's center (bufferWidth * 0.5)
    // lands on an integer buffer pixel - odd widths put quad screen
    // positions on half-pixels, and lround() then splits adjacent tiles to
    // opposite sides of the buffer-center boundary, producing 1-pixel gaps
    // (visible as clear-color stripes at tile joins).
    int renderWidth = static_cast<int>(std::ceil(m_CanvasSize.x / effectiveZoom)) + 2;
    int renderHeight = static_cast<int>(std::ceil(m_CanvasSize.y / effectiveZoom)) + 2;
    if (renderWidth & 1)
    {
        ++renderWidth;
    }
    if (renderHeight & 1)
    {
        ++renderHeight;
    }
    if (renderWidth < 2)
    {
        renderWidth = 2;
    }
    if (renderHeight < 2)
    {
        renderHeight = 2;
    }

    // Position camera at the world location corresponding to canvas center
    // This ensures the buffer always contains what's visible, allowing objects
    // placed anywhere in the world to be seen when panning.
    // canvas pixels → buffer pixels (÷ effectiveZoom) → world units (÷ PPM)
    float canvasCenterX = m_CanvasSize.x * 0.5f;
    float canvasCenterY = m_CanvasSize.y * 0.5f;
    float deviceCenterX = m_PanOffsetX + sceneWidth * 0.5f;
    float deviceCenterY = m_PanOffsetY + sceneHeight * 0.5f;
    const float canvasToWorld = (effectiveZoom * projectPPM > 0.0f) ? (1.0f / (effectiveZoom * projectPPM)) : 0.0f;

    // World position at canvas center (Y inverted for screen-to-world)
    float camWorldX = (canvasCenterX - deviceCenterX) * canvasToWorld;
    float camWorldY = -(canvasCenterY - deviceCenterY) * canvasToWorld;

    // Lazy init: panel construction may precede package load. The early bail-out
    // at the top of DrawSceneCanvas guarantees CameraComponent is registered
    // by the time we reach here.
    if (!m_EditorCamera)
    {
        Deki::Internal::AddComponentByName(*m_EditorCameraObject, "CameraComponent");
        m_EditorCamera = m_EditorCameraObject->FindInterface<Deki::ICamera>();
        if (!m_EditorCamera)
        {
            return;
        }
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
    m_EditorCamera->SetFixedPixelsPerMeter(Deki::EngineSettings::Global().pixelsPerMeter);
    m_EditorCamera->SetClearColor(clearR, clearG, clearB);

    if (m_IsDragging || m_Gizmos->IsDraggingResize())
    {
        s_EditingObjectsFrame = ImGui::GetFrameCount();
    }

    // Where this frame's buffer sits on the canvas. Gizmos always use it, so
    // they follow the view exactly even while the picture is not re-rendered.
    ImVec2 bufferCanvasTopLeft, bufferCanvasBottomRight;
    m_Coords->GetBufferScreenBounds(renderWidth, renderHeight, camRenderX, camRenderY, bufferCanvasTopLeft,
                                    bufferCanvasBottomRight);

    // Edit-mode render dirty flags: only call RenderScene when something
    // visible to the buffer actually changed since last render. What the scene
    // shows changing always renders. Drag-active is included because drags
    // mutate the selected object's world position every frame outside the
    // command system.
    const bool contentChanged = !m_EditRenderHasRendered || m_LastEditRenderedScene != scene ||
                                m_LastEditRenderClearR != clearR || m_LastEditRenderClearG != clearG ||
                                m_LastEditRenderClearB != clearB || m_LastEditRenderGeneration != curGen ||
                                m_IsDragging ||
                                m_Gizmos->IsDraggingResize()
                                // Inspector live-edits (transform fields + reflection-based property
                                // widgets) mutate object state every frame without pushing a command,
                                // so the generation counter stays put. Forcing a re-render while any
                                // Inspector widget is active keeps the buffer in sync with the gizmos.
                                || InspectorPanel::IsAnyLiveEditing()
                                // The running game moves objects every tick without touching the
                                // generation counter.
                                || playing;
    const bool bufferResized = !m_EditModeRenderer->IsInitialized() || m_EditModeRenderer->GetWidth() != renderWidth ||
                               m_EditModeRenderer->GetHeight() != renderHeight;
    const bool viewChanged = bufferResized || m_LastEditRenderCamX != camRenderX || m_LastEditRenderCamY != camRenderY;

    // While the view moves (the wheel zoom gliding, or a pan in progress) the
    // last render - moved and scaled by the GPU - stands in for the new view,
    // and the exact software render refreshes only as often as it can afford:
    // once four times its own cost has passed. At 1:1 a render is a couple of
    // milliseconds, so that is still every frame; zoomed far out, where one
    // render covers many times the pixels, the view keeps moving at full frame
    // rate and the picture catches up a few times a second. Edges that come
    // into view in between show the scene's background until then. The exact render
    // runs as soon as the view stops. Only a view change waits: what the scene
    // shows changing always renders.
    const bool viewMoving = m_Zoom != m_ZoomTarget || m_IsPanning;
    const double now = ImGui::GetTime();
    const bool refreshDue = (now - m_LastEditRenderTime) >= 4.0 * m_LastEditRenderCost;
    const bool viewWaits =
        viewChanged && !contentChanged && viewMoving && !refreshDue && m_EditModeRenderer->IsInitialized();

    if ((contentChanged || viewChanged) && !viewWaits)
    {
        if (bufferResized)
        {
            m_EditModeRenderer->Initialize(renderWidth, renderHeight);
        }
        const auto renderStart = std::chrono::steady_clock::now();
        // Zoomed out, the renderer uploads a screen-size copy (same pixels);
        // see EditModeRenderer::RenderScene.
        m_EditModeRenderer->RenderScene(scene, m_EditorCamera, clearR, clearG, clearB, effectiveZoom);
        m_LastEditRenderCost = std::chrono::duration<double>(std::chrono::steady_clock::now() - renderStart).count();
        m_LastEditRenderTime = now;
        m_LastEditRenderZoom = effectiveZoom;
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

    // Display the last render where its own snapped camera places it - the
    // same as this frame's buffer unless the view is still moving.
    ImVec2 lastTopLeft, lastBottomRight;
    m_Coords->GetBufferScreenBounds(m_LastEditRenderWidth, m_LastEditRenderHeight, m_LastEditRenderCamX,
                                    m_LastEditRenderCamY, lastTopLeft, lastBottomRight);
    // A shrunk (zoomed-out) texture holds exactly the screen pixels it was
    // made for: shown 1:1 at rest, scaled only while the zoom still moves.
    if (m_EditModeRenderer->IsTextureShrunk() && m_LastEditRenderZoom > 0.0f)
    {
        const float grow = effectiveZoom / m_LastEditRenderZoom;
        lastBottomRight = ImVec2(lastTopLeft.x + m_EditModeRenderer->GetTextureWidth() * grow,
                                 lastTopLeft.y + m_EditModeRenderer->GetTextureHeight() * grow);
    }

    // The scene's background everywhere on the canvas, drawn by the GPU under
    // the picture: while the view moves ahead of the next render, the edges
    // that come into view show the background, not a blank canvas.
    drawList->AddRectFilled(m_CanvasTopLeft,
                            ImVec2(m_CanvasTopLeft.x + m_CanvasSize.x, m_CanvasTopLeft.y + m_CanvasSize.y),
                            IM_COL32(clearR, clearG, clearB, 255));

    uint32_t texId = m_EditModeRenderer->GetTextureId();
    if (texId != 0)
    {
        // ImGui's GL backend binds a GL_LINEAR sampler object that overrides the texture's
        // own GL_NEAREST params, blurring the scene when zoomed. Unbind the sampler for
        // this draw so the texture's nearest-neighbor filtering takes effect.
        drawList->AddCallback(
            [](const ImDrawList*, const ImDrawCmd*)
            {
                // NOLINTNEXTLINE(readability-identifier-naming): OpenGL's name
                typedef void(APIENTRY * PFN_glBindSampler)(GLuint unit, GLuint sampler);
                static auto s_Fn = (PFN_glBindSampler)SDL_GL_GetProcAddress("glBindSampler");
                if (s_Fn)
                {
                    s_Fn(0, 0);
                }
            },
            nullptr);
        drawList->AddImage((ImTextureID)(intptr_t)texId, lastTopLeft, lastBottomRight, ImVec2(0, 0),
                           ImVec2(m_EditModeRenderer->GetTextureU(), m_EditModeRenderer->GetTextureV()));
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
    // virtual on a stale package DLL crashes - its vtable lacks the slot).
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
            if (!obj || !obj->IsActive())
            {
                return;
            }

            // Convert world → buffer pixel → canvas screen (matches the same
            // coord chain that DrawComponentOutlines uses).
            float bufX, bufY;
            m_EditorCamera->WorldToScreen(obj->GetWorldX(), obj->GetWorldY(), renderWidth, renderHeight, bufX, bufY);
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
                if (!meta || !meta->name)
                {
                    continue;
                }
                DekiEditor::CustomEditor* ed = editorRegistry.GetEditorForComponent(meta->name, comp);
                if (!ed)
                {
                    continue;
                }
                ed->OnDrawGizmos(comp);
                if (ctx.isSelected)
                {
                    ed->OnDrawGizmosSelected(comp);
                }
            }
            view.EndObject();

            for (Deki::Object* child : obj->GetChildren())
            {
                self(self, child);
            }
        };

        for (Deki::Object* root : scene->GetObjects())
        {
            visit(visit, root);
        }
    }

    // Bounds around the rest of a multiple selection; the first one gets
    // them, and its tool gizmo, below.
    if (m_Toolbar->ShowGizmos() && m_App->HasMultipleSelected())
    {
        for (Deki::Object* obj : m_App->GetSelectedObjects())
        {
            if (obj && obj != selected && obj->IsActive())
            {
                m_Gizmos->DrawSelectionBounds(drawList, obj, m_EditorCamera, renderWidth, renderHeight,
                                              bufferCanvasTopLeft);
            }
        }
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
        m_Snapping->SetDeviceBounds(static_cast<float>(screenW) * invPPM, static_cast<float>(screenH) * invPPM);
        m_Snapping->SetSnapThreshold(4.0f * invPPM);

        if (m_Gizmos->IsDraggingResize() && io.KeyCtrl)
        {
            m_Snapping->SetObjectTargets(buildObjectSnapTargets(selected));
        }
        else if (!m_IsDragging)
        {
            m_Snapping->SetObjectTargets({});
        }

        // Draw tool-specific gizmos
        switch (m_Toolbar->GetCurrentTool())
        {
            case SceneTool::Select: break;
            case SceneTool::Move: m_Gizmos->DrawTranslateGizmo(drawList, selected); break;
            case SceneTool::Rect: m_Gizmos->DrawResizeGizmo(drawList, selected, m_Toolbar->RectAnchorEdge()); break;
        }
    }

    // Draw snap guide lines
    m_Snapping->DrawSnapGuides(drawList, m_Coords.get(), m_CanvasTopLeft, m_CanvasSize);

    // Draw grid
    if (m_Toolbar->ShowGrid() && m_Toolbar->GetGridSize() > 0)
    {
        const float canvasLeft = m_CanvasTopLeft.x;
        const float canvasTop = m_CanvasTopLeft.y;
        const float canvasRight = m_CanvasTopLeft.x + m_CanvasSize.x;
        const float canvasBottom = m_CanvasTopLeft.y + m_CanvasSize.y;

        // Cells are GetGridSize() anchor pixels; lines stay at least 8 px
        // apart on screen however far out the view is zoomed.
        const double cell = static_cast<double>(m_Toolbar->GetGridSize()) * effectiveZoom;
        const float minSpacing = 8.0f * dpi;

        DrawPixelGridAxis(drawList, true, sceneLeft, effectiveZoom, canvasLeft, canvasRight, canvasTop, canvasBottom);
        DrawPixelGridAxis(drawList, false, sceneTop, effectiveZoom, canvasTop, canvasBottom, canvasLeft, canvasRight);
        DrawGridAxis(drawList, true, sceneLeft, cell, canvasLeft, canvasRight, canvasTop, canvasBottom, minSpacing);
        DrawGridAxis(drawList, false, sceneTop, cell, canvasTop, canvasBottom, canvasLeft, canvasRight, minSpacing);
    }

    if (m_MarqueeActive)
    {
        const ImVec2 a(std::min(m_MarqueeStart.x, io.MousePos.x), std::min(m_MarqueeStart.y, io.MousePos.y));
        const ImVec2 b(std::max(m_MarqueeStart.x, io.MousePos.x), std::max(m_MarqueeStart.y, io.MousePos.y));
        ImVec4 fill = DekiEditor::Palette::Accent;
        fill.w = 0.10f;
        ImVec4 edge = DekiEditor::Palette::Accent;
        edge.w = 0.85f;
        drawList->AddRectFilled(a, b, ImGui::ColorConvertFloat4ToU32(fill));
        drawList->AddRect(a, b, ImGui::ColorConvertFloat4ToU32(edge), 0.0f, 0, 1.0f);
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

}  // namespace DekiEditor
