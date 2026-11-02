#include "SceneView3D.h"

#include "EditModeRenderer.h"
#include "../InspectorPanel.h"
#include "../../EditorApp.h"

#include <deki-editor/CommandHistory.h>
#include <deki-editor/EditorTexture.h>
#include <deki/ICamera.h>
#include <deki/Math.h>
#include <deki/Object.h>
#include <deki/Scene.h>
#include <deki/assets/AssetManager.h>
#include <deki/reflection/ComponentRegistry.h>
#include <deki/reflection/Property.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>

namespace DekiEditor
{
namespace
{

using Deki::Mat4;
using Deki::Vector3;

constexpr float kMinPitch = -1.55f;  // just short of straight down or up
constexpr float kMaxPitch = 1.55f;

// The 3D pass's conventions (deki-3d's Math3D.h), so the overlays land where
// the meshes are drawn: column-major, right-handed, looking down -Z.

Mat4 Mul(const Mat4& a, const Mat4& b)
{
    Mat4 out;
    for (int c = 0; c < 4; ++c)
    {
        for (int r = 0; r < 4; ++r)
        {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k)
            {
                sum += a.m[k][r] * b.m[c][k];
            }
            out.m[c][r] = sum;
        }
    }
    return out;
}

Mat4 Perspective(float fovY, float aspect, float nearZ, float farZ)
{
    Mat4 m;
    const float t = std::tan(fovY * 0.5f);
    if (t == 0.0f || aspect == 0.0f || nearZ == farZ)
    {
        return m;
    }
    const float f = 1.0f / t;
    m.m[0][0] = f / aspect;
    m.m[1][1] = f;
    m.m[2][2] = (farZ + nearZ) / (nearZ - farZ);
    m.m[2][3] = -1.0f;
    m.m[3][2] = (2.0f * farZ * nearZ) / (nearZ - farZ);
    m.m[3][3] = 0.0f;
    return m;
}

Mat4 LookAt(const Vector3& eye, const Vector3& target, const Vector3& up)
{
    const Vector3 f = (target - eye).Normalized();
    Vector3 s = f.Cross(up);
    const float sLen = s.Length();
    if (sLen <= 0.0f)
    {
        return Mat4::Identity();
    }
    s = s / sLen;
    const Vector3 u = s.Cross(f);
    Mat4 m;
    m.m[0][0] = s.x;
    m.m[1][0] = s.y;
    m.m[2][0] = s.z;
    m.m[0][1] = u.x;
    m.m[1][1] = u.y;
    m.m[2][1] = u.z;
    m.m[0][2] = -f.x;
    m.m[1][2] = -f.y;
    m.m[2][2] = -f.z;
    m.m[3][0] = -s.Dot(eye);
    m.m[3][1] = -u.Dot(eye);
    m.m[3][2] = f.Dot(eye);
    return m;
}

float DistanceToSegment(const ImVec2& p, const ImVec2& a, const ImVec2& b)
{
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len2 = dx * dx + dy * dy;
    float t = len2 > 0.0f ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const float ex = a.x + dx * t - p.x, ey = a.y + dy * t - p.y;
    return std::sqrt(ex * ex + ey * ey);
}

/// Sets a float field of the camera component by its reflected name: the
/// lens's planes, which ICamera only reads.
void SetCameraFloat(Deki::Object* cameraObject, const char* name, float value)
{
    for (Deki::Component* comp : cameraObject->GetComponents())
    {
        const Deki::ComponentMeta* meta = Deki::ComponentRegistry::Instance().GetMeta(comp->GetType());
        if (!meta || std::strcmp(meta->name, "CameraComponent") != 0)
        {
            continue;
        }
        for (int i = 0; i < meta->propertyCount; ++i)
        {
            const Deki::PropertyInfo& prop = meta->properties[i];
            if (prop.type == Deki::PropertyType::Float && std::strcmp(prop.name, name) == 0)
            {
                *reinterpret_cast<float*>(reinterpret_cast<char*>(comp) + prop.offset) = value;
                return;
            }
        }
    }
}

Vector3 WorldPosition(const Deki::Object* obj)
{
#ifdef DEKI_TRANSFORM_3D
    return Vector3(obj->GetWorldX(), obj->GetWorldY(), obj->GetWorldZ());
#else
    return Vector3(obj->GetWorldX(), obj->GetWorldY(), 0.0f);
#endif
}

Vector3 AxisOf(int axis)
{
    return axis == 0 ? Vector3(1, 0, 0) : (axis == 1 ? Vector3(0, 1, 0) : Vector3(0, 0, 1));
}

// Two directions across `axis`, for drawing its rotation ring.
void RingBasis(int axis, Vector3& u, Vector3& v)
{
    u = axis == 0 ? Vector3(0, 1, 0) : (axis == 1 ? Vector3(0, 0, 1) : Vector3(1, 0, 0));
    v = axis == 0 ? Vector3(0, 0, 1) : (axis == 1 ? Vector3(1, 0, 0) : Vector3(0, 1, 0));
}

const ImU32 kAxisColor[3] = { IM_COL32(230, 80, 80, 255), IM_COL32(110, 210, 90, 255), IM_COL32(80, 140, 245, 255) };
const ImU32 kActiveColor = IM_COL32(255, 220, 70, 255);

}  // namespace

SceneView3D::SceneView3D(EditorApp* app) : m_App(app), m_Renderer(std::make_unique<EditModeRenderer>())
{
    // Its size changes whenever the view starts or stops moving: streaming
    // keeps textures that only grow, instead of a new one at each size.
    m_Renderer->SetStreaming(true);
}

SceneView3D::~SceneView3D() = default;

Vector3 SceneView3D::Forward() const
{
    const float cp = std::cos(m_Pitch), sp = std::sin(m_Pitch);
    return Vector3(-cp * std::sin(m_Yaw), sp, -cp * std::cos(m_Yaw));
}

Vector3 SceneView3D::Up() const
{
    const float cp = std::cos(m_Pitch), sp = std::sin(m_Pitch);
    return Vector3(sp * std::sin(m_Yaw), cp, sp * std::cos(m_Yaw));
}

float SceneView3D::FieldOfView() const
{
    return m_FovDegrees * Deki::Math::kDegToRad;
}

void SceneView3D::StartFrom(const Vector3& target, float visibleHeight)
{
    m_Target = target;
    m_Yaw = 0.0f;
    m_Pitch = 0.0f;
    const float half = std::tan(FieldOfView() * 0.5f);
    m_Distance = (visibleHeight > 0.0f && half > 0.0f) ? visibleHeight * 0.5f / half : 10.0f;
    m_HasShown = false;
}

void SceneView3D::RestoreCamera(Deki::Object* cameraObject, Deki::ICamera* camera)
{
    if (!cameraObject || !camera)
    {
        return;
    }
    camera->SetProjectionMode(Deki::ProjectionMode::Orthographic);
#ifdef DEKI_TRANSFORM_3D
    cameraObject->SetZ(0.0f);
    cameraObject->SetRotationX(0.0f);
    cameraObject->SetRotationY(0.0f);
#endif
    cameraObject->SetRotation(0.0f);
}

void SceneView3D::UpdateViewProjection(const ImVec2& size)
{
    const float aspect = size.y > 0.0f ? size.x / size.y : 1.0f;
    const Vector3 eye = Eye();
    m_ViewProjection = Mul(Perspective(FieldOfView(), aspect, m_Distance * 0.02f, m_Distance * 50.0f),
                           LookAt(eye, eye + Forward(), Up()));
}

bool SceneView3D::Project(const Vector3& p, ImVec2& out) const
{
    const Mat4& m = m_ViewProjection;
    const float x = m.m[0][0] * p.x + m.m[1][0] * p.y + m.m[2][0] * p.z + m.m[3][0];
    const float y = m.m[0][1] * p.x + m.m[1][1] * p.y + m.m[2][1] * p.z + m.m[3][1];
    const float w = m.m[0][3] * p.x + m.m[1][3] * p.y + m.m[2][3] * p.z + m.m[3][3];
    if (w <= m_Distance * 0.02f)
    {
        return false;
    }
    out.x = m_TopLeft.x + (x / w * 0.5f + 0.5f) * m_Size.x;
    out.y = m_TopLeft.y + (0.5f - y / w * 0.5f) * m_Size.y;
    return true;
}

void SceneView3D::Draw(Deki::Scene* scene, Deki::Object* cameraObject, Deki::ICamera* camera,
                       const Deki::ICamera* lens, SceneTool& tool, const ImVec2& topLeft, const ImVec2& size,
                       uint8_t clearR, uint8_t clearG, uint8_t clearB, bool showGrid)
{
    m_TopLeft = topLeft;
    m_Size = size;
    m_FovDegrees =
        (lens && lens->GetProjectionMode() == Deki::ProjectionMode::Perspective) ? lens->GetFieldOfView() : 60.0f;

    ImGui::InvisibleButton("SceneCanvas3D", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle |
                               ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mouse = ImGui::GetIO().MousePos;

    HandleNavigation(hovered, mouse);
    UpdateViewProjection(size);
    HandleTools(scene, tool, hovered, mouse);

    // The editor camera, in perspective where the view is: the 3D pass draws
    // through it as through a scene's own perspective camera.
    const Vector3 eye = Eye();
    cameraObject->SetWorldPosition(eye.x, eye.y);
#ifdef DEKI_TRANSFORM_3D
    cameraObject->SetZ(eye.z);
    cameraObject->SetRotationX(m_Pitch);
    cameraObject->SetRotationY(m_Yaw);
#endif
    cameraObject->SetRotation(0.0f);
    camera->SetProjectionMode(Deki::ProjectionMode::Perspective);
    SetCameraFloat(cameraObject, "fieldOfView", m_FovDegrees);
    SetCameraFloat(cameraObject, "nearPlane", m_Distance * 0.02f);
    SetCameraFloat(cameraObject, "farPlane", m_Distance * 50.0f);
    camera->SetClearColor(clearR, clearG, clearB);

    // Rendered only when something it shows moved, at the canvas's size once
    // the view is still. While it moves, every frame is a new picture, and a
    // full-size one costs tens of milliseconds (the 2D on the CPU, the 3D read
    // back, the upload), so it renders smaller, at whatever scale fits the
    // budget, shown stretched; the sharp picture follows when it stops.
    const ImGuiIO& io = ImGui::GetIO();
    // A little after the last movement too: wheel notches come a few frames
    // apart, and each would otherwise bring a full-size render between them.
    const double clock = ImGui::GetTime();
    if (m_Orbiting || m_Looking || m_Panning || io.MouseWheel != 0.0f || m_DragHandle != Handle::None)
    {
        m_LastMoved = clock;
    }
    const bool moving = clock - m_LastMoved < 0.15;
    const float scale = moving ? m_MovingScale : 1.0f;
    int width = std::max(2, static_cast<int>(size.x * scale)) & ~1;
    int height = std::max(2, static_cast<int>(size.y * scale)) & ~1;
    const uint64_t generation = CommandHistory::Instance().GetGeneration();
    const uint64_t assetEpoch = Deki::AssetManager::Get()->GetEpoch();
    Shown now;
    now.target = m_Target;
    now.yaw = m_Yaw;
    now.pitch = m_Pitch;
    now.distance = m_Distance;
    now.fov = m_FovDegrees;
    now.width = width;
    now.height = height;
    now.clearR = clearR;
    now.clearG = clearG;
    now.clearB = clearB;
    now.scene = scene;
    now.generation = generation;
    now.assetEpoch = assetEpoch;
    const bool same = m_HasShown && now.target.x == m_Shown.target.x && now.target.y == m_Shown.target.y &&
                      now.target.z == m_Shown.target.z && now.yaw == m_Shown.yaw && now.pitch == m_Shown.pitch &&
                      now.distance == m_Shown.distance && now.fov == m_Shown.fov && now.width == m_Shown.width &&
                      now.height == m_Shown.height && now.clearR == m_Shown.clearR &&
                      now.clearG == m_Shown.clearG && now.clearB == m_Shown.clearB && now.scene == m_Shown.scene &&
                      now.generation == m_Shown.generation && now.assetEpoch == m_Shown.assetEpoch;
    const bool live = InspectorPanel::IsAnyLiveEditing() || m_App->IsPlaying() || m_DragHandle != Handle::None;
    if (!same || live)
    {
        if (!m_Renderer->IsInitialized())
        {
            m_Renderer->Initialize(width, height);
        }
        else if (m_Renderer->GetWidth() != width || m_Renderer->GetHeight() != height)
        {
            m_Renderer->Resize(width, height);
        }
        const auto start = std::chrono::steady_clock::now();
        m_Renderer->RenderScene(scene, camera, clearR, clearG, clearB, 1.0f);
        const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (moving && ms > 0.0f)
        {
            // The cost goes with the pixels, the square of the scale: aim the
            // next moving frame at the budget, never sharper than full size.
            constexpr float kMovingBudgetMs = 6.0f;
            m_MovingScale = std::clamp(m_MovingScale * std::sqrt(kMovingBudgetMs / ms), 0.2f, 1.0f);
        }
        m_Shown = now;
        m_HasShown = true;
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(topLeft, ImVec2(topLeft.x + size.x, topLeft.y + size.y), true);
    drawList->AddRectFilled(topLeft, ImVec2(topLeft.x + size.x, topLeft.y + size.y),
                            IM_COL32(clearR, clearG, clearB, 255));
    if (const uint32_t tex = m_Renderer->GetTextureId())
    {
        // Over the whole canvas: 1:1 at rest, stretched while moving.
        EditorTexture::AddImageNearest(drawList, tex, topLeft, ImVec2(topLeft.x + size.x, topLeft.y + size.y),
                                       ImVec2(0, 0), ImVec2(m_Renderer->GetTextureU(), m_Renderer->GetTextureV()));
    }
    if (showGrid)
    {
        DrawGrid(drawList);
    }
    if (Deki::Object* selected = m_App->GetSelectedObject())
    {
        DrawGizmo(drawList, selected, tool);
    }
    DrawAxes(drawList);
    drawList->PopClipRect();
}

void SceneView3D::HandleNavigation(bool hovered, const ImVec2& mouse)
{
    ImGuiIO& io = ImGui::GetIO();
    const float dpi = ImGui::GetWindowDpiScale();
    (void)mouse;

    // Starting a drag needs the cursor over the view; carrying one on does not.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && io.KeyAlt)
    {
        m_Orbiting = true;
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
    {
        m_Looking = true;
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
    {
        m_Panning = true;
    }
    m_Orbiting = m_Orbiting && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    m_Looking = m_Looking && ImGui::IsMouseDown(ImGuiMouseButton_Right);
    m_Panning = m_Panning && ImGui::IsMouseDown(ImGuiMouseButton_Middle);

    const float turn = 0.006f / std::max(dpi, 0.5f);
    const ImVec2 d = io.MouseDelta;
    if (m_Orbiting)
    {
        // Round the point looked at: the view turns, the point stays.
        m_Yaw -= d.x * turn;
        m_Pitch = std::clamp(m_Pitch - d.y * turn, kMinPitch, kMaxPitch);
    }
    else if (m_Looking)
    {
        // Round the eye: the eye stays and the point looked at swings.
        const Vector3 eye = Eye();
        m_Yaw -= d.x * turn;
        m_Pitch = std::clamp(m_Pitch - d.y * turn, kMinPitch, kMaxPitch);
        m_Target = eye + Forward() * m_Distance;

        // And fly: the keys move eye and point together.
        const float speed = m_Distance * io.DeltaTime * (io.KeyShift ? 3.0f : 1.0f);
        const Vector3 forward = Forward();
        Vector3 right = forward.Cross(Up());
        right = right.Length() > 0.0f ? right.Normalized() : Vector3(1, 0, 0);
        Vector3 move;
        if (ImGui::IsKeyDown(ImGuiKey_W))
        {
            move = move + forward;
        }
        if (ImGui::IsKeyDown(ImGuiKey_S))
        {
            move = move - forward;
        }
        if (ImGui::IsKeyDown(ImGuiKey_D))
        {
            move = move + right;
        }
        if (ImGui::IsKeyDown(ImGuiKey_A))
        {
            move = move - right;
        }
        if (ImGui::IsKeyDown(ImGuiKey_E))
        {
            move = move + Vector3(0, 1, 0);
        }
        if (ImGui::IsKeyDown(ImGuiKey_Q))
        {
            move = move - Vector3(0, 1, 0);
        }
        m_Target = m_Target + move * speed;
    }
    else if (m_Panning && m_Size.y > 0.0f)
    {
        // One canvas pixel is this much world at the point looked at.
        const float perPixel = 2.0f * m_Distance * std::tan(FieldOfView() * 0.5f) / m_Size.y;
        Vector3 right = Forward().Cross(Up());
        right = right.Length() > 0.0f ? right.Normalized() : Vector3(1, 0, 0);
        m_Target = m_Target - right * (d.x * perPixel) + Up() * (d.y * perPixel);
    }

    if (hovered && io.MouseWheel != 0.0f)
    {
        m_Distance = std::clamp(m_Distance * std::pow(0.85f, io.MouseWheel), 1e-3f, 1e7f);
    }
}

SceneView3D::Handle SceneView3D::HandleUnder(Deki::Object* obj, SceneTool tool, const ImVec2& mouse) const
{
    if (!obj || (tool != SceneTool::Move && tool != SceneTool::Rotate))
    {
        return Handle::None;
    }
    const float dpi = ImGui::GetWindowDpiScale();
    const Vector3 origin = WorldPosition(obj);
    ImVec2 o;
    if (!Project(origin, o))
    {
        return Handle::None;
    }
    const float length = (Eye() - origin).Length() * 0.15f;
    Handle best = Handle::None;
    float bestDistance = 8.0f * dpi;
    for (int axis = 0; axis < 3; ++axis)
    {
        float distance = 1e9f;
        if (tool == SceneTool::Move)
        {
            ImVec2 tip;
            if (Project(origin + AxisOf(axis) * length, tip))
            {
                distance = DistanceToSegment(mouse, o, tip);
            }
        }
        else
        {
            Vector3 u, v;
            RingBasis(axis, u, v);
            ImVec2 prev;
            bool hasPrev = false;
            for (int i = 0; i <= 48; ++i)
            {
                const float t = static_cast<float>(i) / 48.0f * 2.0f * Deki::Math::kPi;
                ImVec2 point;
                const bool ok = Project(origin + (u * std::cos(t) + v * std::sin(t)) * (length * 0.9f), point);
                if (ok && hasPrev)
                {
                    distance = std::min(distance, DistanceToSegment(mouse, prev, point));
                }
                prev = point;
                hasPrev = ok;
            }
        }
        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = static_cast<Handle>(axis + 1);
        }
    }
    return best;
}

Deki::Object* SceneView3D::PickObject(Deki::Scene* scene, const ImVec2& mouse) const
{
    // The object whose reach on screen holds the cursor, nearest the eye. A
    // reach is its scale seen from here, so a large mesh is picked anywhere
    // over its middle and a small one by a few pixels round its origin.
    const float dpi = ImGui::GetWindowDpiScale();
    Vector3 right = Forward().Cross(Up());
    right = right.Length() > 0.0f ? right.Normalized() : Vector3(1, 0, 0);
    Deki::Object* best = nullptr;
    float bestDepth = 1e30f;
    std::function<void(Deki::Object*)> visit = [&](Deki::Object* obj)
    {
        if (!obj || !obj->IsActive())
        {
            return;
        }
        if (!obj->GetComponents().empty())
        {
            const Vector3 origin = WorldPosition(obj);
            ImVec2 o, edge;
#ifdef DEKI_TRANSFORM_3D
            const float scaleZ = std::abs(obj->GetWorldScaleZ());
#else
            const float scaleZ = 0.0f;
#endif
            const float reachWorld =
                std::max({ std::abs(obj->GetWorldScaleX()), std::abs(obj->GetWorldScaleY()), scaleZ });
            if (Project(origin, o) && Project(origin + right * reachWorld, edge))
            {
                const float reach = std::max(std::hypot(edge.x - o.x, edge.y - o.y), 10.0f * dpi);
                const float distance = std::hypot(mouse.x - o.x, mouse.y - o.y);
                const float depth = (origin - Eye()).Length();
                if (distance <= reach && depth < bestDepth)
                {
                    best = obj;
                    bestDepth = depth;
                }
            }
        }
        for (Deki::Object* child : obj->GetChildren())
        {
            visit(child);
        }
    };
    for (Deki::Object* root : scene->GetObjects())
    {
        visit(root);
    }
    return best;
}

void SceneView3D::FrameSelection()
{
    Deki::Object* selected = m_App->GetSelectedObject();
    if (!selected)
    {
        return;
    }
    m_Target = WorldPosition(selected);
#ifdef DEKI_TRANSFORM_3D
    const float scaleZ = std::abs(selected->GetWorldScaleZ());
#else
    const float scaleZ = 0.0f;
#endif
    const float size =
        std::max({ std::abs(selected->GetWorldScaleX()), std::abs(selected->GetWorldScaleY()), scaleZ, 0.01f });
    m_Distance = size * 4.0f;
}

void SceneView3D::HandleTools(Deki::Scene* scene, SceneTool& tool, bool hovered, const ImVec2& mouse)
{
    ImGuiIO& io = ImGui::GetIO();
    Deki::Object* selected = m_App->GetSelectedObject();

    // Keys, when the view has the cursor and nothing is being typed or flown.
    if (hovered && !io.WantTextInput && !m_Looking)
    {
        if (ImGui::IsKeyPressed(ImGuiKey_Q))
        {
            tool = SceneTool::Select;
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_W))
        {
            tool = SceneTool::Move;
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_E))
        {
            tool = SceneTool::Rotate;
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_F))
        {
            FrameSelection();
        }
    }

    // A press: a gizmo handle starts a drag, anywhere else picks.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt && m_DragHandle == Handle::None)
    {
        const Handle handle = HandleUnder(selected, tool, mouse);
        if (handle != Handle::None)
        {
            m_DragHandle = handle;
            m_DragObject = selected;
            m_DragStart = Transform3D::Of(selected);
            m_DragMouseStart = mouse;
            m_DragOrigin = WorldPosition(selected);
        }
        else
        {
            m_App->SetSelectedObject(PickObject(scene, mouse));
        }
    }

    if (m_DragHandle == Handle::None || !m_DragObject)
    {
        return;
    }

    const int axis = static_cast<int>(m_DragHandle) - 1;
    Transform3D now = m_DragStart;
    if (tool == SceneTool::Move)
    {
        // Along the axis by as much as the cursor moved along its picture.
        const float length = (Eye() - m_DragOrigin).Length() * 0.15f;
        ImVec2 o, tip;
        if (Project(m_DragOrigin, o) && Project(m_DragOrigin + AxisOf(axis) * length, tip))
        {
            const float dx = tip.x - o.x, dy = tip.y - o.y;
            const float len2 = dx * dx + dy * dy;
            if (len2 > 1.0f)
            {
                const float mx = mouse.x - m_DragMouseStart.x, my = mouse.y - m_DragMouseStart.y;
                const float along = (mx * dx + my * dy) / len2 * length;
                float* fields[3] = { &now.x, &now.y, &now.z };
                *fields[axis] += along;
            }
        }
    }
    else if (tool == SceneTool::Rotate)
    {
        // By the angle the cursor turned round the object's centre. Positive
        // turns are counter-clockwise seen from the axis's tip; seen from
        // behind it they run the other way.
        ImVec2 o;
        if (Project(m_DragOrigin, o))
        {
            const float a0 = std::atan2(m_DragMouseStart.y - o.y, m_DragMouseStart.x - o.x);
            const float a1 = std::atan2(mouse.y - o.y, mouse.x - o.x);
            float turn = a1 - a0;
            while (turn > Deki::Math::kPi)
            {
                turn -= 2.0f * Deki::Math::kPi;
            }
            while (turn < -Deki::Math::kPi)
            {
                turn += 2.0f * Deki::Math::kPi;
            }
            const bool facing = AxisOf(axis).Dot(Eye() - m_DragOrigin) > 0.0f;
            const float angle = facing ? -turn : turn;  // screen y points down
            float* fields[3] = { &now.rotationX, &now.rotationY, &now.rotation };
            *fields[axis] += angle;
        }
    }
    now.ApplyTo(m_DragObject);

    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        if (!(now == m_DragStart))
        {
            CommandHistory::Instance().Execute(std::make_unique<SetTransform3DCommand>(
                m_DragObject, m_DragStart, now, tool == SceneTool::Rotate ? "Rotate Object" : "Move Object"));
        }
        m_DragHandle = Handle::None;
        m_DragObject = nullptr;
    }
}

void SceneView3D::DrawGrid(ImDrawList* drawList) const
{
    // The ground, the XZ plane through the origin, ruled at a step that suits
    // how far away it is seen from.
    const float step = std::pow(10.0f, std::floor(std::log10(std::max(m_Distance * 0.25f, 1e-4f))));
    const int lines = 20;
    const float cx = std::round(m_Target.x / step) * step;
    const float cz = std::round(m_Target.z / step) * step;
    const float extent = step * lines;
    auto line = [&](const Vector3& a, const Vector3& b, ImU32 colour)
    {
        // In pieces, so a line passing behind the eye still draws its front.
        constexpr int kPieces = 16;
        ImVec2 prev;
        bool hasPrev = false;
        for (int i = 0; i <= kPieces; ++i)
        {
            const float t = static_cast<float>(i) / kPieces;
            ImVec2 p;
            const bool ok = Project(a + (b - a) * t, p);
            if (ok && hasPrev)
            {
                drawList->AddLine(prev, p, colour, 1.0f);
            }
            prev = p;
            hasPrev = ok;
        }
    };
    for (int i = -lines; i <= lines; ++i)
    {
        const float x = cx + i * step;
        const float z = cz + i * step;
        line(Vector3(x, 0, cz - extent), Vector3(x, 0, cz + extent),
             std::abs(x) < step * 0.5f ? IM_COL32(80, 140, 245, 110) : IM_COL32(255, 255, 255, 28));
        line(Vector3(cx - extent, 0, z), Vector3(cx + extent, 0, z),
             std::abs(z) < step * 0.5f ? IM_COL32(230, 80, 80, 110) : IM_COL32(255, 255, 255, 28));
    }
}

void SceneView3D::DrawGizmo(ImDrawList* drawList, Deki::Object* obj, SceneTool tool) const
{
    const float dpi = ImGui::GetWindowDpiScale();
    const Vector3 origin = WorldPosition(obj);
    ImVec2 o;
    if (!Project(origin, o))
    {
        return;
    }
    const Handle hot = m_DragHandle != Handle::None ? m_DragHandle : HandleUnder(obj, tool, ImGui::GetIO().MousePos);
    const float length = (Eye() - origin).Length() * 0.15f;
    for (int axis = 0; axis < 3; ++axis)
    {
        const ImU32 colour = hot == static_cast<Handle>(axis + 1) ? kActiveColor : kAxisColor[axis];
        if (tool == SceneTool::Rotate)
        {
            Vector3 u, v;
            RingBasis(axis, u, v);
            ImVec2 prev;
            bool hasPrev = false;
            for (int i = 0; i <= 48; ++i)
            {
                const float t = static_cast<float>(i) / 48.0f * 2.0f * Deki::Math::kPi;
                ImVec2 point;
                const bool ok = Project(origin + (u * std::cos(t) + v * std::sin(t)) * (length * 0.9f), point);
                if (ok && hasPrev)
                {
                    drawList->AddLine(prev, point, colour, 2.0f * dpi);
                }
                prev = point;
                hasPrev = ok;
            }
        }
        else if (tool == SceneTool::Move)
        {
            ImVec2 tip;
            if (Project(origin + AxisOf(axis) * length, tip))
            {
                drawList->AddLine(o, tip, colour, 3.0f * dpi);
                drawList->AddCircleFilled(tip, 5.0f * dpi, colour);
            }
        }
    }
    // The selection's origin, whatever the tool.
    drawList->AddRectFilled(ImVec2(o.x - 3 * dpi, o.y - 3 * dpi), ImVec2(o.x + 3 * dpi, o.y + 3 * dpi),
                            IM_COL32(255, 255, 255, 230));
}

void SceneView3D::DrawAxes(ImDrawList* drawList) const
{
    // Which way X, Y and Z point, in the bottom-left corner.
    const float dpi = ImGui::GetWindowDpiScale();
    const ImVec2 centre(m_TopLeft.x + 36.0f * dpi, m_TopLeft.y + m_Size.y - 36.0f * dpi);
    Vector3 right = Forward().Cross(Up());
    right = right.Length() > 0.0f ? right.Normalized() : Vector3(1, 0, 0);
    const Vector3 up = Up();
    const char* names[3] = { "X", "Y", "Z" };
    for (int axis = 0; axis < 3; ++axis)
    {
        const Vector3 a = AxisOf(axis);
        const ImVec2 tip(centre.x + a.Dot(right) * 24.0f * dpi, centre.y - a.Dot(up) * 24.0f * dpi);
        drawList->AddLine(centre, tip, kAxisColor[axis], 2.0f * dpi);
        drawList->AddText(ImVec2(tip.x + 2.0f * dpi, tip.y - 7.0f * dpi), kAxisColor[axis], names[axis]);
    }
}

}  // namespace DekiEditor
