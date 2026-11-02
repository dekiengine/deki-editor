#pragma once

// The Scene view's 3D view, for projects that declare transform_3d.
//
// The 2D view is a pan and zoom over the world's XY plane; meshes show there
// looking straight down -Z. This view instead puts the editor camera in
// perspective at a point it can orbit, so a 3D scene can be looked at from
// anywhere: the 3D pass draws through any perspective camera, the editor's
// included, so nothing in the engine changes for it.
//
//   Alt + left drag    orbit around the point the view looks at
//   right drag         look around; held, W A S D move and Q E sink and rise
//   middle drag        pan
//   wheel              move in and out
//   F                  frame the selection
//   Q / W / E          select, move (X Y Z arrows), rotate (rings)
//
// Gizmo drags record one undo step each (SetTransform3DCommand). The camera is
// the panel's editor camera, never part of the scene, so nothing of the view
// is saved.

#include "SceneViewTypes.h"
#include "imgui.h"

#include <deki/Vector.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "../../core/commands/PropertyCommands.h"  // Transform3D

namespace Deki
{
class ICamera;
class Object;
class Scene;
}  // namespace Deki

namespace DekiEditor
{

class EditorApp;
class EditModeRenderer;

class SceneView3D
{
public:
    explicit SceneView3D(EditorApp* app);
    ~SceneView3D();

    /// Draws `scene` through `camera` (owned by `cameraObject`, the editor's)
    /// into the canvas at `topLeft`, `size`, and handles the canvas's input.
    /// `lens` is the scene's own camera, whose field of view the view borrows
    /// when it is a perspective one; `showGrid` rules the ground plane. Call
    /// where the 2D view would draw.
    void Draw(Deki::Scene* scene, Deki::Object* cameraObject, Deki::ICamera* camera, const Deki::ICamera* lens,
              SceneTool& tool, const ImVec2& topLeft, const ImVec2& size, uint8_t clearR, uint8_t clearG,
              uint8_t clearB, bool showGrid);

    /// Starts the view where the 2D view was: looking down -Z at `target`,
    /// back far enough that a plane `visibleHeight` tall fills the canvas.
    void StartFrom(const Deki::Vector3& target, float visibleHeight);

    /// Puts the editor camera back as the 2D view wants it: orthographic, at
    /// depth 0, unturned. Call when leaving this view.
    static void RestoreCamera(Deki::Object* cameraObject, Deki::ICamera* camera);

private:
    enum class Handle
    {
        None,
        X,
        Y,
        Z,
    };

    // The view: a point looked at, from yaw/pitch at a distance.
    Deki::Vector3 Forward() const;
    Deki::Vector3 Up() const;
    Deki::Vector3 Eye() const { return m_Target - Forward() * m_Distance; }
    float FieldOfView() const;  // vertical, radians

    /// Canvas pixel of a world point; false when it is behind the eye.
    bool Project(const Deki::Vector3& world, ImVec2& out) const;
    void UpdateViewProjection(const ImVec2& size);

    void HandleNavigation(bool hovered, const ImVec2& mouse);
    void HandleTools(Deki::Scene* scene, SceneTool& tool, bool hovered, const ImVec2& mouse);
    Handle HandleUnder(Deki::Object* obj, SceneTool tool, const ImVec2& mouse) const;
    Deki::Object* PickObject(Deki::Scene* scene, const ImVec2& mouse) const;
    void FrameSelection();

    void DrawGrid(ImDrawList* drawList) const;
    void DrawGizmo(ImDrawList* drawList, Deki::Object* obj, SceneTool tool) const;
    void DrawAxes(ImDrawList* drawList) const;

    EditorApp* m_App;
    std::unique_ptr<EditModeRenderer> m_Renderer;

    Deki::Vector3 m_Target;
    float m_Yaw = 0.0f;    // radians about Y; 0 looks down -Z
    float m_Pitch = 0.0f;  // radians about X; positive looks up
    float m_Distance = 10.0f;
    float m_FovDegrees = 60.0f;

    // This frame's canvas and projection.
    ImVec2 m_TopLeft;
    ImVec2 m_Size;
    Deki::Mat4 m_ViewProjection;

    // Navigation drags.
    bool m_Orbiting = false;
    bool m_Looking = false;
    bool m_Panning = false;

    // A gizmo drag.
    Handle m_DragHandle = Handle::None;
    Deki::Object* m_DragObject = nullptr;
    Transform3D m_DragStart;
    ImVec2 m_DragMouseStart;
    Deki::Vector3 m_DragOrigin;  // the object's world position when it began

    // What the last picture showed, to render again only when it changes.
    struct Shown
    {
        Deki::Vector3 target;
        float yaw = 0, pitch = 0, distance = 0, fov = 0;
        int width = 0, height = 0;
        uint8_t clearR = 0, clearG = 0, clearB = 0;
        const void* scene = nullptr;
        uint64_t generation = 0, assetEpoch = 0;
    };
    Shown m_Shown;
    bool m_HasShown = false;
    // Render scale while the view moves, learnt from what the last moving
    // frames cost.
    float m_MovingScale = 0.5f;
    double m_LastMoved = -1.0;  // ImGui time of the last movement
};

}  // namespace DekiEditor
