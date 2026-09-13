#include "ObjectInternal.h"
#include "SceneInspector.h"
#include "../../EditorApp.h"
#include "../../core/Project.h"
#include "../scene_view/EditModeRenderer.h"
#include <deki/Object.h>
#include <deki/ICamera.h>
#include <deki/Scene.h>
#include <deki/Engine.h>
#include "LifecycleManager.h"
#include <deki/reflection/ComponentRegistry.h>
#include "imgui.h"
#include <functional>

namespace DekiEditor
{

SceneInspector::SceneInspector(EditorApp* app)
    : m_App(app)
{
}

SceneInspector::~SceneInspector()
{
    Cleanup();
}

void SceneInspector::Cleanup()
{
    if (m_Renderer)
        m_Renderer->Shutdown();
    m_Renderer.reset();
    m_CameraObject.reset();
    m_Camera = nullptr;
    m_PreviewScenePath.clear();
}

void SceneInspector::DrawPreview(const std::string& assetPath)
{
    // Render at 2x target display size to show more of the scene (zoomed out)
    auto* display = DekiEditor::GetDisplaySettings();
    int bufW = (display ? display->targetWidth : 320) * 2;
    int bufH = (display ? display->targetHeight : 240) * 2;

    // Render preview if asset changed
    if (assetPath != m_PreviewScenePath)
    {
        m_PreviewScenePath = assetPath;

        // Ensure camera exists (rendering package must be loaded)
        if (!m_Camera)
        {
            if (!Deki::ComponentRegistry::Instance().GetMetaByClassName("CameraComponent"))
                return;
            m_CameraObject = std::make_unique<Deki::Object>();
            Deki::detail::AddComponentByName(*m_CameraObject, "CameraComponent");
            m_Camera = m_CameraObject->FindInterface<Deki::ICamera>();
            if (!m_Camera) return;
        }

        // Initialize/resize renderer
        if (!m_Renderer)
            m_Renderer = std::make_unique<EditModeRenderer>();

        if (!m_Renderer->IsInitialized() ||
            m_Renderer->GetWidth() != bufW ||
            m_Renderer->GetHeight() != bufH)
        {
            m_Renderer->Initialize(bufW, bufH);
        }

        // Load scene temporarily and render
        Deki::Scene scene;
        if (scene.LoadFromFile(assetPath))
        {
            // Resolve AssetRefs so sprite frame data gets populated
            Deki::LifecycleManager lifecycle;
            lifecycle.ProcessInternalAwake(&scene);

            // Use camera from the scene if available, else our preview camera
            Deki::ICamera* cam = nullptr;
            std::function<Deki::ICamera*(Deki::Object*)> findCamera = [&](Deki::Object* obj) -> Deki::ICamera*
            {
                Deki::ICamera* c = obj->FindInterface<Deki::ICamera>();
                if (c) return c;
                for (auto* child : obj->GetChildren())
                {
                    c = findCamera(child);
                    if (c) return c;
                }
                return nullptr;
            };
            for (Deki::Object* obj : scene.GetObjects())
            {
                cam = findCamera(obj);
                if (cam) break;
            }

            if (!cam)
            {
                // Default camera at origin, project-scale 1:1.
                m_CameraObject->SetWorldPosition(0, 0);
                m_Camera->SetPixelsPerMeter(Deki::EngineSettings::Global().pixelsPerMeter);
                m_Camera->SetClearColor(49, 77, 121);
                cam = m_Camera;
            }

            uint8_t clearR = 49, clearG = 77, clearB = 121;
            cam->GetClearColor(clearR, clearG, clearB);

            m_Renderer->RenderScene(&scene, cam, clearR, clearG, clearB);
        }
    }

    // Display preview
    if (m_Renderer && m_Renderer->GetTextureId() != 0)
    {
        float availWidth = ImGui::GetContentRegionAvail().x;
        float fullAspect = static_cast<float>(bufH) / static_cast<float>(bufW);
        float displayWidth = availWidth;
        float displayHeight = displayWidth * fullAspect * 0.7f;

        // Crop UVs to show center portion (not stretched)
        float uvCrop = (1.0f - 0.7f) * 0.5f;

        float cursorX = (availWidth - displayWidth) / 2.0f;
        if (cursorX > 0)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + cursorX);

        ImVec2 imagePos = ImGui::GetCursorScreenPos();
        ImGui::Image((ImTextureID)(intptr_t)m_Renderer->GetTextureId(),
                     ImVec2(displayWidth, displayHeight),
                     ImVec2(0, uvCrop), ImVec2(1, 1 - uvCrop));

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRect(imagePos,
                          ImVec2(imagePos.x + displayWidth, imagePos.y + displayHeight),
                          IM_COL32(128, 128, 128, 255), 0.0f, 0, 1.0f);
    }
}

}  // namespace DekiEditor
