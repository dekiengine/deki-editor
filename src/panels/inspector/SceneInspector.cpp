#include "ObjectInternal.h"
#include <deki-editor/EditorTexture.h>
#include "../../services/PackageLifetime.h"
#include <deki-editor/SceneView.h>
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
    // The preview camera is a package component: drop it before packages unload.
    PackageLifetime::OnBeforeUnload(this, [this]() { Cleanup(); });
}

SceneInspector::~SceneInspector()
{
    PackageLifetime::Forget(this);
    Cleanup();
}

void SceneInspector::Cleanup()
{
    if (m_Renderer)
    {
        m_Renderer->Shutdown();
    }
    m_Renderer.reset();
    m_CameraObject.reset();
    m_Camera = nullptr;
    m_PreviewScenePath.clear();
}

void SceneInspector::DrawPreview(const std::string& assetPath)
{
    // Render at the active platform's screen: what the player sees there
    // through the scene's camera.
    int bufW = 0, bufH = 0;
    DekiEditor::ActivePlatformScreen(m_App ? m_App->GetProject() : nullptr, bufW, bufH);

    // Render again only when the asset changed.
    if (assetPath != m_PreviewScenePath)
    {
        m_PreviewScenePath = assetPath;

        // The preview camera needs the rendering package loaded.
        if (!m_Camera)
        {
            if (!Deki::ComponentRegistry::Instance().GetMetaByClassName("CameraComponent"))
            {
                return;
            }
            m_CameraObject = std::make_unique<Deki::Object>();
            Deki::Internal::AddComponentByName(*m_CameraObject, "CameraComponent");
            m_Camera = m_CameraObject->FindInterface<Deki::ICamera>();
            if (!m_Camera)
            {
                return;
            }
        }

        if (!m_Renderer)
        {
            m_Renderer = std::make_unique<EditModeRenderer>();
        }

        if (!m_Renderer->IsInitialized() || m_Renderer->GetWidth() != bufW || m_Renderer->GetHeight() != bufH)
        {
            m_Renderer->Initialize(bufW, bufH);
        }

        // Load a temporary copy of the scene and render it.
        Deki::Scene scene;
        if (scene.LoadFromFile(assetPath))
        {
            // Resolve AssetRefs so sprite frame data is filled in.
            Deki::LifecycleManager lifecycle;
            lifecycle.ProcessInternalAwake(&scene);

            // The scene's own camera if it has one, else the preview camera.
            Deki::ICamera* cam = nullptr;
            std::function<Deki::ICamera*(Deki::Object*)> findCamera = [&](Deki::Object* obj) -> Deki::ICamera*
            {
                Deki::ICamera* c = obj->FindInterface<Deki::ICamera>();
                if (c)
                {
                    return c;
                }
                for (auto* child : obj->GetChildren())
                {
                    c = findCamera(child);
                    if (c)
                    {
                        return c;
                    }
                }
                return nullptr;
            };
            for (Deki::Object* obj : scene.GetObjects())
            {
                cam = findCamera(obj);
                if (cam)
                {
                    break;
                }
            }

            if (!cam)
            {
                // A camera at the origin showing the screen 1:1.
                m_CameraObject->SetWorldPosition(0, 0);
                const float art = Deki::EngineSettings::Global().pixelsPerMeter;
                if (art > 0.0f)
                {
                    m_Camera->SetOrthoHeight(bufH / art);
                }
                m_Camera->SetClearColor(49, 77, 121);
                cam = m_Camera;
            }

            uint8_t clearR = 49, clearG = 77, clearB = 121;
            cam->GetClearColor(clearR, clearG, clearB);

            m_Renderer->RenderScene(&scene, cam, clearR, clearG, clearB);
        }
    }

    if (m_Renderer && m_Renderer->GetTextureId() != 0)
    {
        // The panel has no window padding, so keep the same inset on the right
        // as on the left; otherwise the preview touches the edge, or runs
        // under a scrollbar.
        const float inset = ImGui::GetCursorScreenPos().x - ImGui::GetWindowPos().x;
        float availWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x - inset);
        float fullAspect = static_cast<float>(bufH) / static_cast<float>(bufW);
        float displayWidth = availWidth;
        float displayHeight = displayWidth * fullAspect * 0.7f;

        // Show the middle 70% of the render, cropped rather than stretched.
        float uvCrop = (1.0f - 0.7f) * 0.5f;

        float cursorX = (availWidth - displayWidth) / 2.0f;
        if (cursorX > 0)
        {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + cursorX);
        }

        ImVec2 imagePos = ImGui::GetCursorScreenPos();
        ImGui::Image(EditorTexture::ToImGui(m_Renderer->GetTextureId()), ImVec2(displayWidth, displayHeight),
                     ImVec2(0, uvCrop), ImVec2(1, 1 - uvCrop));

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRect(imagePos, ImVec2(imagePos.x + displayWidth, imagePos.y + displayHeight),
                          IM_COL32(128, 128, 128, 255), 0.0f, 0, 1.0f);
    }
}

}  // namespace DekiEditor
