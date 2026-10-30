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
#include <deki-editor/Localization.h>
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
#include <deki/ScreenAnchor.h>
#include <deki/ISortableProvider.h>
#include <deki/IClipProvider.h>
#include <deki/reflection/ComponentFactory.h>
#include "LifecycleManager.h"
#include <deki/Engine.h>
#include <deki/reflection/ComponentRegistry.h>
#include <deki/reflection/Property.h>
#include "imgui.h"
#include "imgui_internal.h"  // ImHashStr
#include <deki-editor/EditorTexture.h>
#include <SDL3/SDL_video.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

namespace DekiEditor
{

// The object's sorting order from its ISortableProvider, or 0.
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
// lines strengthen over [4*min, 8*min]. Since it follows the line's own
// spacing, zooming changes every line smoothly and nothing pops between
// levels. At 1:1, 16 px cells draw at 40 and every 4th line at 80.
float GridLineAlpha(double spacing, float minSpacing)
{
    constexpr float kMinorAlpha = 40.0f, kMajorAlpha = 80.0f;
    return kMinorAlpha * Saturate((spacing - minSpacing) / minSpacing) +
           (kMajorAlpha - kMinorAlpha) * Saturate((spacing - 4.0 * minSpacing) / (4.0 * minSpacing));
}

// One axis of the grid. `origin` is the screen position of line 0, `cell` the
// finest spacing, [lo, hi] the visible range along the axis and [spanLo,
// spanHi] across it. Lines are placed by index, not accumulated, and the
// spacing grows by 4 until lines are at least `minSpacing` apart, so there are
// a few hundred at most at any zoom. Each sits on a pixel centre so it draws
// as one crisp pixel, not two faint ones.
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

// Pixel grid: one line per world pixel, fading in between 2.5x and 4x zoom
// (the maximum).
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

SceneViewPanel::SceneViewPanel(EditorApp* app)
    : m_App(app)
{
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

    // Invisible editor camera, never saved.
    m_EditorCameraObject = std::make_unique<Deki::Object>();
    m_EditorCameraObject->SetName("__EditorCamera");
    m_EditorCameraObject->SetEditorOnly(true);

    // The camera component is added in DrawSceneCanvas(), since packages may not
    // be loaded yet, and dropped before any package unloads (its code lives in
    // deki-rendering).
    PackageLifetime::OnBeforeUnload(this, [this]() { InvalidateEditorCamera(); });
}

SceneViewPanel::~SceneViewPanel()
{
    PackageLifetime::Forget(this);
}

void SceneViewPanel::InvalidateEditorCamera()
{
    m_EditorCamera = nullptr;
    // A new object destroys the old components while their DLL is still loaded.
    m_EditorCameraObject = std::make_unique<Deki::Object>();
    m_EditorCameraObject->SetName("__EditorCamera");
    m_EditorCameraObject->SetEditorOnly(true);
}

void SceneViewPanel::RememberAnchorOffset(Deki::Object* obj)
{
    if (auto* anchor = obj->GetComponent<Deki::ScreenAnchor>())
    {
        m_DragStartAnchorX = anchor->offsetX;
        m_DragStartAnchorY = anchor->offsetY;
    }
}

void SceneViewPanel::PlaceScreenAnchors()
{
    Deki::Scene* scene = m_App->GetCurrentScene();
    Deki::ICamera* camera = GetSceneCamera();
    if (!scene || !camera)
    {
        return;
    }
    const SceneView& view = SceneView::Get();
    Deki::ScreenAnchor::PlaceAll(scene, *camera, view.GetPreviewWidth(), view.GetPreviewHeight());
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

    // Unload every component's assets so they resolve again on the next frame.
    std::function<void(Deki::Object*)> unloadRecursive = [&](Deki::Object* obj)
    {
        if (!obj)
        {
            return;
        }

        for (Deki::Component* comp : obj->GetComponents())
        {
            comp->UnloadAssets();
            comp->ResetRefsResolved();
        }

        for (Deki::Object* child : obj->GetChildren())
        {
            unloadRecursive(child);
        }
    };

    for (Deki::Object* obj : scene->GetObjects())
    {
        unloadRecursive(obj);
    }
}

void SceneViewPanel::RefreshSceneCameraCache()
{
    ::Deki::Scene* scene = m_App->GetCurrentScene();
    const uint64_t gen = CommandHistory::Instance().GetGeneration();
    // Play mode spawns objects outside the command history, so there the walk
    // runs every frame.
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

    // The first object in the tree with an ICamera.
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

    bool visible = ImGui::Begin(TrLabel("SCENE VIEW###Scene View"), &isOpen,
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
        ImGui::TextDisabled("%s", Tr("No scene loaded"));
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

    // Tool shortcuts (Q, W, R) while the canvas is hovered and no text field has focus.
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

    // Delete key. Goes through EditorApp so the protection rules (scene root,
    // boot Setup, instance children) apply here as in the Hierarchy.
    Deki::Object* selected = m_App->GetSelectedObject();
    if (selected && isCanvasHovered && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Delete))
    {
        m_App->DeleteObjects(m_App->GetSelectedObjects());
    }

    // Pan with Alt+Left, Middle or Right mouse.
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

    // Mouse wheel zoom: each notch moves the target, and the zoom eases there
    // below, around the world point that was under the cursor.
    const ImGuiID zoomKey = ImHashStr("SceneViewZoom");
    if (isCanvasHovered && io.MouseWheel != 0)
    {
        m_ZoomAnchorScreen = io.MousePos;
        m_ZoomAnchorWorld = m_Coords->ScreenToWorld(io.MousePos.x, io.MousePos.y);

        // Scaled by the wheel delta, so precise wheels and trackpads move in
        // small steps. Ctrl makes the step smaller still.
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
        // The world point that was under the midpoint moves to where the
        // midpoint is now, at the new zoom: that one move is both the pan and
        // the zoom. m_Coords still holds the view from before the gesture,
        // which is the view the old midpoint was in.
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
    // World to screen is editorZoom * dpi * the project's pixels per meter: the
    // Scene view shows the world at its art density, whatever the camera does.
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

    // No pixel snapping here: it drifts a little with every zoom step.
    // WorldToScreen and GetBufferScreenBounds already floor to whole pixels.
}

void SceneViewPanel::DrawSceneCanvas()
{
    auto* scene = m_App->GetCurrentScene();
    if (!scene)
    {
        return;
    }

    // A new scene is centred once the layout settles.
    if (m_LastScene != scene)
    {
        m_LastScene = scene;
        m_NeedsCentering = true;
        m_CenteringDelay = 3;
    }

    ImVec2 availSize = ImGui::GetContentRegionAvail();
    if (availSize.x <= 0 || availSize.y <= 0)
    {
        return;
    }

    m_CanvasTopLeft = ImGui::GetCursorScreenPos();
    m_CanvasSize = availSize;

    // Keep the centre of the view still when the canvas resizes. The pan is
    // an offset from the canvas's top-left, so it moves by half the size
    // change. Skipped on the first draw, when there is no previous size.
    // Centering below sets the pan outright anyway.
    if (m_PannedForCanvasSize.x > 0.0f && m_PannedForCanvasSize.y > 0.0f)
    {
        m_PanOffsetX += (availSize.x - m_PannedForCanvasSize.x) * 0.5f;
        m_PanOffsetY += (availSize.y - m_PannedForCanvasSize.y) * 0.5f;
    }
    m_PannedForCanvasSize = availSize;

    // Centering runs before the viewport is set, so no frame shows the old
    // pan. The scene rectangle anchors the world origin (SceneViewConstants).
    // The previewed screen, drawn around the origin and snapped to, is the
    // Play view's.
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
            // Zoom 1:1, scene centred in the canvas.
            m_Zoom = 1.0f;
            m_ZoomTarget = 1.0f;
            float sceneWidth = vpWidth * m_Zoom * dpi;
            float sceneHeight = vpHeight * m_Zoom * dpi;

            m_PanOffsetX = (availSize.x - sceneWidth) * 0.5f;
            m_PanOffsetY = (availSize.y - sceneHeight) * 0.5f;
            m_NeedsCentering = false;
        }
    }

    // CameraComponent's vtable lives in deki-rendering.dll. Asking a component
    // for ICamera before that package is loaded (while a project opens, say)
    // would read an unmapped vtable, so wait for a later frame.
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

    // Again, since HandleInput may have changed zoom and pan.
    m_Coords->SetViewport(m_CanvasTopLeft, m_CanvasSize, m_Zoom, m_PanOffsetX, m_PanOffsetY, projectPPM);

    effectiveZoom = m_Zoom * dpi;
    sceneWidth = vpWidth * effectiveZoom;
    sceneHeight = vpHeight * effectiveZoom;
    sceneLeft = m_CanvasTopLeft.x + m_PanOffsetX;
    sceneTop = m_CanvasTopLeft.y + m_PanOffsetY;

    // Flattened hierarchy for selection, rebuilt only when the structure
    // changes. A drag rebuilds it too, since sortable children can reorder
    // mid-drag.
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

    // Snap targets from every object but `exclude`, in world meters.
    // GetDisplaySize returns pixels, so sizes become pixels * scale / ppm.
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

    // Hover state is recomputed only when the mouse moved or the selection
    // changed. Other frames keep the last result and skip the reflection walk
    // in GetDisplaySize.
    const bool mouseMoved = (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f);
    const bool selectionChangedForHover = (selected != m_LastHoverSelected);
    m_LastHoverSelected = selected;
    const bool hoverNeedsUpdate = mouseMoved || selectionChangedForHover;

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

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && isCanvasHovered && !m_IsPanning && !io.KeyAlt)
    {
        ImVec2 mousePos = ImGui::GetMousePos();
        ImVec2 worldPos = m_Coords->ScreenToWorld(mousePos.x, mousePos.y);

        // A finger in the Select tool: a drag pans instead of moving the
        // selected object, which a finger does too easily by accident, and
        // selection waits for the lift so a pan leaves it alone. The mouse
        // keeps select-and-drag; to move by touch, use the Move tool's gizmo.
        if (io.MouseSource == ImGuiMouseSource_TouchScreen && m_Toolbar->GetCurrentTool() == SceneTool::Select)
        {
            m_TouchPendingPan = true;
            m_TouchPressScreen = mousePos;
        }
        // Resize handle first.
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
                    // start the drag from the drawn size, or the first frame
                    // shrinks the object to the cursor's offset from zero.
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
        // Then the translate gizmo.
        else if (selected && m_Gizmos->GetHoveredAxis() != GizmoAxis::None)
        {
            m_IsDragging = true;
            m_Gizmos->SetDraggingAxis(m_Gizmos->GetHoveredAxis());
            m_DragOffsetX = selected->GetWorldX() - worldPos.x;
            m_DragOffsetY = selected->GetWorldY() - worldPos.y;
            m_DragStartObjX = selected->GetWorldX();
            m_DragStartObjY = selected->GetWorldY();
            RememberAnchorOffset(selected);
        }
        else
        {
            // A press on the selected object arms a pending drag. The drag
            // starts once the mouse passes the drag threshold; releasing
            // without moving selects the next overlapping object instead.
            if (selected && m_Selection->HitTestObject(selected, worldPos.x, worldPos.y, projectPPM))
            {
                m_PendingDrag = true;
                m_PendingDragStartScreen = mousePos;
                m_DragOffsetX = selected->GetWorldX() - worldPos.x;
                m_DragOffsetY = selected->GetWorldY() - worldPos.y;
                m_DragStartObjX = selected->GetWorldX();
                m_DragStartObjY = selected->GetWorldY();
                RememberAnchorOffset(selected);
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

    // A pending drag becomes a drag on movement, or a cycle click on release.
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

    // A finger press in the Select tool: a drag becomes a pan from the press
    // point (so the movement so far counts), a lift is a click there.
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

            // Ctrl snaps to the screen edges and other objects. Snapping
            // works in world meters, so pixel sizes are converted.
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

            // Last, so Ctrl-snap results also land on whole pixels when the
            // project is in Pixels mode.
            newWorldX = SnapMetersToPixel(newWorldX);
            newWorldY = SnapMetersToPixel(newWorldY);
            if (auto* anchor = selected->GetComponent<Deki::ScreenAnchor>())
            {
                anchor->offsetX += newWorldX - selected->GetWorldX();
                anchor->offsetY += newWorldY - selected->GetWorldY();
            }
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
                auto* anchor = selected->GetComponent<Deki::ScreenAnchor>();
                if (posChanged && anchor)
                {
                    CommandHistory::Instance().Execute(std::make_unique<SetAnchorOffsetCommand>(
                        anchor, m_DragStartAnchorX, m_DragStartAnchorY, anchor->offsetX, anchor->offsetY));
                }
                else if (posChanged)
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

    // The device screen around world (0,0), shown only when the scene has no
    // camera. With a camera, its gizmo shows it (see CameraCustomEditor).
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

        // Device bounds in the theme accent.
        const ImU32 deviceColor = ImGui::ColorConvertFloat4ToU32(DekiEditor::Palette::Accent);

        ImVec2 borderTopLeft(devTopLeft.x - 0.5f, devTopLeft.y - 0.5f);
        ImVec2 borderBottomRight(devBottomRight.x + 0.5f, devBottomRight.y + 0.5f);
        drawList->AddRect(borderTopLeft, borderBottomRight, deviceColor, 0.0f, 0, 1.0f);

        char deviceLabel[64];
        snprintf(deviceLabel, sizeof(deviceLabel), "%dx%d", screenW, screenH);
        drawList->AddText(ImVec2(devTopLeft.x + 4, devTopLeft.y + 4), deviceColor, deviceLabel);
    }

    // Play mode draws the live play scene through the editor camera, like edit
    // mode, so the grid, gizmos and selection stay (as in Unity's Scene view).
    // The game's own lifecycle resolves the play scene's refs. Outside play
    // the edit lifecycle loads assets; ProcessInternalAwake loads only those
    // not loaded yet.
    const bool playing = m_App->IsPlaying();
    if (!playing)
    {
        m_EditModeLifecycle->ProcessInternalAwake(scene);
    }

    // The render buffer covers the whole canvas, not only the device screen.
    // Its size is kept even so the camera's centre (bufferWidth * 0.5) is a
    // whole buffer pixel: odd sizes put quads on half pixels, lround() then
    // rounds neighbouring tiles apart, and 1-pixel gaps show at tile joins.
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

    // The camera sits at the world point under the canvas centre, so the
    // buffer holds whatever is visible wherever the view is panned.
    // Canvas pixels / effectiveZoom = buffer pixels; / PPM = world meters.
    float canvasCenterX = m_CanvasSize.x * 0.5f;
    float canvasCenterY = m_CanvasSize.y * 0.5f;
    float deviceCenterX = m_PanOffsetX + sceneWidth * 0.5f;
    float deviceCenterY = m_PanOffsetY + sceneHeight * 0.5f;
    const float canvasToWorld = (effectiveZoom * projectPPM > 0.0f) ? (1.0f / (effectiveZoom * projectPPM)) : 0.0f;

    // World position at the canvas centre (screen Y points down).
    float camWorldX = (canvasCenterX - deviceCenterX) * canvasToWorld;
    float camWorldY = -(canvasCenterY - deviceCenterY) * canvasToWorld;

    // Added here rather than in the constructor, which may run before
    // packages load. The check at the top of this function makes sure
    // CameraComponent is registered.
    if (!m_EditorCamera)
    {
        Deki::Internal::AddComponentByName(*m_EditorCameraObject, "CameraComponent");
        m_EditorCamera = m_EditorCameraObject->FindInterface<Deki::ICamera>();
        if (!m_EditorCamera)
        {
            return;
        }
    }

    // The editor camera snaps to whole buffer pixels (1/projectPPM meter).
    // The renderer rounds each quad's position on its own with lround(), so
    // a camera between pixels makes neighbouring tiles round apart and leaves
    // 1-pixel gaps. On a whole pixel, every quad lands on a whole pixel too.
    const float snap = (projectPPM > 0.0f) ? projectPPM : 1.0f;
    const float invSnap = 1.0f / snap;
    const float camRenderX = std::floor(camWorldX * snap) * invSnap;
    const float camRenderY = std::floor(camWorldY * snap) * invSnap;

    m_EditorCameraObject->SetWorldPosition(camRenderX, camRenderY);
    // The buffer renders at the project's own scale; the wheel zoom is applied
    // when the texture is drawn.
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

    // RenderScene runs only when something the buffer shows changed. A drag
    // counts, since it moves the selected object every frame outside the
    // command history.
    const bool contentChanged = !m_EditRenderHasRendered || m_LastEditRenderedScene != scene ||
                                m_LastEditRenderClearR != clearR || m_LastEditRenderClearG != clearG ||
                                m_LastEditRenderClearB != clearB || m_LastEditRenderGeneration != curGen ||
                                m_IsDragging ||
                                m_Gizmos->IsDraggingResize()
                                // Inspector live edits change objects every frame without a
                                // command, so the generation stays put; re-render while one is
                                // active to keep the buffer in step with the gizmos.
                                || InspectorPanel::IsAnyLiveEditing()
                                // The running game moves objects every tick without touching the
                                // generation counter.
                                || playing;
    const bool bufferResized = !m_EditModeRenderer->IsInitialized() || m_EditModeRenderer->GetWidth() != renderWidth ||
                               m_EditModeRenderer->GetHeight() != renderHeight;
    const bool viewChanged = bufferResized || m_LastEditRenderCamX != camRenderX || m_LastEditRenderCamY != camRenderY;

    // While the view moves (wheel zoom easing, or a pan) the last render,
    // moved and scaled by the GPU, stands in for the new view. The software
    // render refreshes once four times its own cost has passed. At 1:1 a
    // render takes a couple of milliseconds, so that is every frame; zoomed
    // far out the view still moves at full frame rate and the picture
    // catches up a few times a second. Edges that come into view show the
    // background until then. The exact render runs as soon as the view stops.
    // Only a view change waits: a change in the scene always renders.
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

    // The last render goes where its own snapped camera places it: the same
    // as this frame's buffer unless the view is still moving.
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

    // The scene's background over the whole canvas, under the picture, so
    // edges that come into view before the next render show the background.
    drawList->AddRectFilled(m_CanvasTopLeft,
                            ImVec2(m_CanvasTopLeft.x + m_CanvasSize.x, m_CanvasTopLeft.y + m_CanvasSize.y),
                            IM_COL32(clearR, clearG, clearB, 255));

    uint32_t texId = m_EditModeRenderer->GetTextureId();
    if (texId != 0)
    {
        // Sampled nearest, so the zoomed scene's pixels stay sharp.
        EditorTexture::AddImageNearest(drawList, texId, lastTopLeft, lastBottomRight, ImVec2(0, 0),
                                       ImVec2(m_EditModeRenderer->GetTextureU(), m_EditModeRenderer->GetTextureV()));
    }

    if (m_Toolbar->ShowOutlines())
    {
        m_Gizmos->DrawComponentOutlines(drawList, scene, m_EditorCamera, renderWidth, renderHeight,
                                        bufferCanvasTopLeft);
    }

    // The camera's rect, selected or not, while gizmos are on.
    // TODO: move to CustomEditor::OnDrawGizmos once project package DLLs are
    // rebuilt against the new CustomEditor header. Calling the new virtual on
    // an older package DLL crashes, since its vtable lacks the slot.
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

    // Calls CustomEditor::OnDrawGizmos / OnDrawGizmosSelected for each
    // component that has an editor. SceneView::BeginObject is set up for each
    // object first, so editors can read its screen position and zoom through
    // SceneView::Get().
    if (m_Toolbar->ShowGizmos() && scene && m_EditorCamera)
    {
        auto& editorRegistry = DekiEditor::CustomEditorRegistry::Instance();
        auto& compRegistry = Deki::ComponentRegistry::Instance();
        auto& view = DekiEditor::SceneView::Get();
        const float zoom = m_Coords->GetEffectiveZoom();

        // Self-passing lambda: recursion without std::function's heap
        // allocation and indirect call per object, every frame.
        auto visit = [&](auto&& self, Deki::Object* obj) -> void
        {
            if (!obj || !obj->IsActive())
            {
                return;
            }

            // World to buffer pixel to canvas screen, as in DrawComponentOutlines.
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

    if (selected && selected->IsActive())
    {
        if (m_Toolbar->ShowGizmos())
        {
            m_Gizmos->DrawSelectionBounds(drawList, selected, m_EditorCamera, renderWidth, renderHeight,
                                          bufferCanvasTopLeft);
        }

        // Snap bounds and targets for a resize, in world meters like the
        // edges HandleResizeDrag computes.
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

        switch (m_Toolbar->GetCurrentTool())
        {
            case SceneTool::Select: break;
            case SceneTool::Move: m_Gizmos->DrawTranslateGizmo(drawList, selected); break;
            case SceneTool::Rect: m_Gizmos->DrawResizeGizmo(drawList, selected, m_Toolbar->RectAnchorEdge()); break;
        }
    }

    m_Snapping->DrawSnapGuides(drawList, m_Coords.get(), m_CanvasTopLeft, m_CanvasSize);

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
        ImGui::Text(Tr("World: (%.1f, %.1f)"), worldPos.x, worldPos.y);
    }
}

}  // namespace DekiEditor
