#include "PlayViewPanel.h"
#include "../PanelMaximize.h"
#include <deki-editor/EditorTheme.h>
#include <deki-editor/IconsTabler.h>
#include "EditorApp.h"
#include "core/Project.h"
#include "scene_view/EditModeRenderer.h"
#include "services/FrameCaptureService.h"
#include <deki/Scene.h>
#include <deki/Object.h>
#include <deki/ICamera.h>
#include "InputKeys.h"
#include "imgui.h"
#include <SDL3/SDL_opengl.h>
#include <SDL3/SDL_video.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace DekiEditor
{

// ImGui's GL backend binds a GL_LINEAR sampler object that overrides the texture's
// own GL_NEAREST params, blurring the play view when the project resolution doesn't
// land on integer screen pixels. Wrap each AddImage with this pair so the texture's
// nearest-neighbor filtering takes effect (same trick used in SceneViewPanel).
static void AddImageNearest(ImDrawList* drawList, ImTextureID texId, const ImVec2& p1, const ImVec2& p2)
{
    drawList->AddCallback([](const ImDrawList*, const ImDrawCmd*)
                          {
        typedef void (APIENTRY *PFN_glBindSampler)(GLuint unit, GLuint sampler);
        static auto fn = (PFN_glBindSampler)SDL_GL_GetProcAddress("glBindSampler");
        if (fn) fn(0, 0); }, nullptr);
    drawList->AddImage(texId, p1, p2);
    drawList->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

PlayViewPanel::PlayViewPanel(EditorApp* app)
    : m_App(app), m_PreviewRenderer(std::make_unique<EditModeRenderer>())
{
}

PlayViewPanel::~PlayViewPanel() = default;

void PlayViewPanel::Draw()
{
    if (!m_Open)
        return;

    ImGui::Begin("PLAY VIEW###Play View", &m_Open, DekiEditor::MaximizedWindowFlags("PLAY VIEW###Play View") | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    // Forward keyboard state to the game while playing and the play view is
    // focused (InputKeys.h codes, same as the desktop simulator's keyboard).
    // Runs on every draw - including stopped/unfocused - so releases always
    // flow and no key sticks pressed inside the game's injected keyboard.
    {
        const bool acceptKeys = m_App->IsPlaying() && ImGui::IsWindowFocused();
        int slot = 0;
        auto forward = [&](ImGuiKey imguiKey, uint32_t dekiKey)
        {
            const bool down = acceptKeys && ImGui::IsKeyDown(imguiKey);
            if (down != m_PlayKeyDown[slot])
            {
                m_PlayKeyDown[slot] = down;
                m_App->HandlePlayModeKey(dekiKey, down);
            }
            ++slot;
        };

        forward(ImGuiKey_Space, Deki::InputKeys::SPACE);
        forward(ImGuiKey_Enter, Deki::InputKeys::ENTER);
        forward(ImGuiKey_Escape, Deki::InputKeys::ESC);
        forward(ImGuiKey_Backspace, Deki::InputKeys::BACKSPACE);
        forward(ImGuiKey_UpArrow, Deki::InputKeys::UP);
        forward(ImGuiKey_DownArrow, Deki::InputKeys::DOWN);
        forward(ImGuiKey_LeftArrow, Deki::InputKeys::LEFT);
        forward(ImGuiKey_RightArrow, Deki::InputKeys::RIGHT);
        for (int i = 0; i < 26; ++i)  // ImGuiKey_A..Z and Deki::InputKeys::A..Z are both contiguous
            forward(static_cast<ImGuiKey>(ImGuiKey_A + i), Deki::InputKeys::A + static_cast<uint32_t>(i));
    }

    // Console-style toolbar strip: a flat simulate-touch cell (hand-finger,
    // accent when on / dim when off) on the left with 1px dividers, a
    // right-aligned mono fps readout, and a bottom hairline. Mirrors
    // ConsolePanel's toolbar chrome.
    {
        const float dpi = m_App->Dpi(1.0f);
        const float rowH = 34.0f * dpi;
        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
        const float avail = ImGui::GetContentRegionAvail().x;
        const ImVec2 rowMax(rowMin.x + avail, rowMin.y + rowH);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 colLine = ImGui::ColorConvertFloat4ToU32(DekiEditor::Palette::Line2);
        const ImU32 colDim = ImGui::ColorConvertFloat4ToU32(DekiEditor::Palette::Dim);
        const ImU32 colAccent = ImGui::ColorConvertFloat4ToU32(DekiEditor::Palette::Accent);
        const ImU32 colHoverBg = IM_COL32(255, 255, 255, 6);

        float cursorX = rowMin.x;
        auto drawDivider = [&](float x)
        {
            dl->AddLine(ImVec2(x, rowMin.y), ImVec2(x, rowMax.y), colLine, 1.0f);
        };

        // Leading divider at strip start.
        drawDivider(cursorX);

        // Simulate-touch cell - hand-finger, accent while enabled, dim when off.
        {
            const bool on = m_TouchInputMode;
            const char* icon = ICON_TI_HAND_FINGER;
            const ImVec2 iconSz = ImGui::CalcTextSize(icon);
            const float w = iconSz.x + 22.0f * dpi;
            ImGui::SetCursorScreenPos(ImVec2(cursorX, rowMin.y));
            bool clicked = ImGui::InvisibleButton("##simtouch", ImVec2(w, rowH));
            bool hovered = ImGui::IsItemHovered();
            if (hovered) DekiEditor::Tooltip(on ? "Simulate touch: on" : "Simulate touch: off");
            if (hovered)
                dl->AddRectFilled(ImVec2(cursorX, rowMin.y), ImVec2(cursorX + w, rowMax.y), colHoverBg);
            dl->AddText(ImVec2(cursorX + (w - iconSz.x) * 0.5f, rowMin.y + (rowH - iconSz.y) * 0.5f),
                        on ? colAccent : colDim, icon);
            cursorX += w;
            drawDivider(cursorX);
            if (clicked) m_TouchInputMode = !m_TouchInputMode;
        }

        // Right-aligned mono fps readout (editor + game). Game fps is sampled
        // per game tick in EditorApp::UpdatePlayMode and stays at 0 outside play.
        {
            const float editorFps = ImGui::GetIO().Framerate;
            const float gameFps = m_App->GetGameFPS();
            char fpsBuf[64];
            if (m_App->IsPlaying() && gameFps > 0.0f)
                snprintf(fpsBuf, sizeof(fpsBuf), "Editor %.0f fps    Game %.0f fps", editorFps, gameFps);
            else
                snprintf(fpsBuf, sizeof(fpsBuf), "Editor %.0f fps", editorFps);

            if (DekiEditor::g_FontMonoSmall) ImGui::PushFont(DekiEditor::g_FontMonoSmall);
            const ImVec2 fpsSz = ImGui::CalcTextSize(fpsBuf);
            const float padR = 12.0f * dpi;
            const float fpsX = rowMax.x - fpsSz.x - padR;
            if (fpsX > cursorX + 8.0f * dpi)
                dl->AddText(ImVec2(fpsX, rowMin.y + (rowH - fpsSz.y) * 0.5f), colDim, fpsBuf);
            if (DekiEditor::g_FontMonoSmall) ImGui::PopFont();
        }

        // Reserve the row in layout flow and close the bottom edge with a hairline.
        ImGui::SetCursorScreenPos(ImVec2(rowMin.x, rowMax.y));
        dl->AddLine(ImVec2(rowMin.x, rowMax.y), ImVec2(rowMax.x, rowMax.y), colLine, 1.0f);
    }

    ImVec2 availSize = ImGui::GetContentRegionAvail();
    m_CanvasTopLeft = ImGui::GetCursorScreenPos();
    m_CanvasSize = availSize;

    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Check if render pipeline is configured
    auto* project = m_App->GetProject();
    if (project && project->GetRenderPipelineGuid().empty())
    {
        auto* settings = DekiEditor::GetDisplaySettings();
        int vpW = settings ? settings->targetWidth : 320;
        int vpH = settings ? settings->targetHeight : 240;

        if (vpW > 0 && vpH > 0)
        {
            // Game-texture-to-screen scaling: use ImGui's live per-viewport DPI
            // so dragging the editor between monitors of different DPI updates
            // immediately (m_App->GetDpiScale() is captured at startup).
            float dpiScale = ImGui::GetWindowDpiScale();
            float texScreenW = std::round(vpW * dpiScale);
            float texScreenH = std::round(vpH * dpiScale);
            float offsetX = std::max((availSize.x - texScreenW) * 0.5f, 0.0f);
            float offsetY = std::max((availSize.y - texScreenH) * 0.5f, 0.0f);

            ImVec2 p1(std::floor(m_CanvasTopLeft.x + offsetX), std::floor(m_CanvasTopLeft.y + offsetY));
            ImVec2 p2(p1.x + texScreenW, p1.y + texScreenH);

            // Draw viewport border
            drawList->AddRect(p1, p2, IM_COL32(100, 100, 100, 255), 0.0f, 0, 1.0f);

            // Center text inside viewport
            const char* msg = "No render pipeline configured";
            ImVec2 textSize = ImGui::CalcTextSize(msg);
            float tx = p1.x + (texScreenW - textSize.x) * 0.5f;
            float ty = p1.y + (texScreenH - textSize.y) * 0.5f;
            drawList->AddText(ImVec2(tx, ty), IM_COL32(255, 200, 100, 255), msg);
        }

        ImGui::InvisibleButton("PlayViewCanvas", availSize);
        ImGui::End();
        return;
    }

    // Stopped state - render camera viewport preview
    if (m_App->IsStopped())
    {
        auto* scene = m_App->GetCurrentScene();

        if (scene && project)
        {
            auto* settings = DekiEditor::GetDisplaySettings();
            int vpW = settings ? settings->targetWidth : 320;
            int vpH = settings ? settings->targetHeight : 240;

            // Find scene camera via ICamera interface (search recursively)
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
            for (Deki::Object* obj : scene->GetObjects())
            {
                cam = findCamera(obj);
                if (cam) break;
            }

            if (cam && vpW > 0 && vpH > 0)
            {
                // Initialize/resize preview renderer to project display dimensions
                if (!m_PreviewRenderer->IsInitialized() ||
                    m_PreviewRenderer->GetWidth() != vpW ||
                    m_PreviewRenderer->GetHeight() != vpH)
                {
                    m_PreviewRenderer->Initialize(vpW, vpH);
                }

                // Render using scene camera
                uint8_t clearR, clearG, clearB;
                cam->GetClearColor(clearR, clearG, clearB);
                m_PreviewRenderer->RenderScene(scene, cam, clearR, clearG, clearB);

                uint32_t texId = m_PreviewRenderer->GetTextureId();
                if (texId != 0)
                {
                    float dpiScale = ImGui::GetWindowDpiScale();
                    float texScreenW = std::round(vpW * dpiScale);
                    float texScreenH = std::round(vpH * dpiScale);

                    // Center texture in available space
                    float offsetX = (availSize.x - texScreenW) * 0.5f;
                    float offsetY = (availSize.y - texScreenH) * 0.5f;
                    offsetX = std::max(offsetX, 0.0f);
                    offsetY = std::max(offsetY, 0.0f);

                    ImVec2 p1(std::floor(m_CanvasTopLeft.x + offsetX), std::floor(m_CanvasTopLeft.y + offsetY));
                    ImVec2 p2(p1.x + texScreenW, p1.y + texScreenH);

                    AddImageNearest(drawList, (ImTextureID)(intptr_t)texId, p1, p2);
                    drawList->AddRect(p1, p2, IM_COL32(100, 100, 100, 255), 0.0f, 0, 1.0f);

                    // Resolution info
                    char resInfo[64];
                    snprintf(resInfo, sizeof(resInfo), "%dx%d", vpW, vpH);
                    drawList->AddText(ImVec2(m_CanvasTopLeft.x + 10, m_CanvasTopLeft.y + 10),
                                      IM_COL32(200, 200, 200, 200), resInfo);
                }
            }
        }

        ImGui::InvisibleButton("PlayViewCanvas", availSize);
        ImGui::End();
        return;
    }

    // Loading state - show edit-mode preview until first play frame is ready
    if (!m_App->IsPlayModeReady())
    {
        uint32_t previewTex = m_PreviewRenderer->GetTextureId();
        if (previewTex != 0)
        {
            auto* settings = DekiEditor::GetDisplaySettings();
            if (settings)
            {
                int vpW = settings->targetWidth;
                int vpH = settings->targetHeight;
                float dpiScale = ImGui::GetWindowDpiScale();
                float texScreenW = std::round(vpW * dpiScale);
                float texScreenH = std::round(vpH * dpiScale);
                float offsetX = std::max((availSize.x - texScreenW) * 0.5f, 0.0f);
                float offsetY = std::max((availSize.y - texScreenH) * 0.5f, 0.0f);
                ImVec2 p1(std::floor(m_CanvasTopLeft.x + offsetX), std::floor(m_CanvasTopLeft.y + offsetY));
                ImVec2 p2(p1.x + texScreenW, p1.y + texScreenH);
                AddImageNearest(drawList, (ImTextureID)(intptr_t)previewTex, p1, p2);
                drawList->AddRect(p1, p2, IM_COL32(100, 100, 100, 255), 0.0f, 0, 1.0f);
            }
        }

        ImGui::InvisibleButton("PlayViewCanvas", availSize);
        ImGui::End();
        return;
    }

    // Playing or paused - render the play mode texture
    uint32_t playTexId = m_App->GetPlayModeTextureId();
    int playW = m_App->GetPlayModeWidth();
    int playH = m_App->GetPlayModeHeight();

    if (playTexId != 0 && playW > 0 && playH > 0)
    {
        float dpiScale = ImGui::GetWindowDpiScale();
        float texScreenW = std::round(playW * dpiScale);
        float texScreenH = std::round(playH * dpiScale);

        // Center texture in available space
        float offsetX = (availSize.x - texScreenW) * 0.5f;
        float offsetY = (availSize.y - texScreenH) * 0.5f;
        offsetX = std::max(offsetX, 0.0f);
        offsetY = std::max(offsetY, 0.0f);

        ImVec2 p1(std::floor(m_CanvasTopLeft.x + offsetX), std::floor(m_CanvasTopLeft.y + offsetY));
        ImVec2 p2(p1.x + texScreenW, p1.y + texScreenH);

        // Draw play mode texture
        AddImageNearest(drawList, (ImTextureID)(intptr_t)playTexId, p1, p2);
        drawList->AddRect(p1, p2, IM_COL32(100, 100, 100, 255), 0.0f, 0, 1.0f);

        // Status indicator
        const char* status = m_App->IsPlaying() ? "PLAYING" : "PAUSED";
        ImU32 statusColor = m_App->IsPlaying() ? IM_COL32(0, 255, 0, 200) : IM_COL32(255, 255, 0, 200);
        drawList->AddText(ImVec2(m_CanvasTopLeft.x + 10, m_CanvasTopLeft.y + 10), statusColor, status);

        // Resolution info
        char resInfo[64];
        snprintf(resInfo, sizeof(resInfo), "%dx%d (1:1)", playW, playH);
        drawList->AddText(ImVec2(m_CanvasTopLeft.x + 10, m_CanvasTopLeft.y + 25),
                          IM_COL32(200, 200, 200, 200), resInfo);

        // REC indicator when capturing frames (F9 to toggle)
        auto* captureService = m_App->GetFrameCaptureService();
        if (captureService && captureService->IsCapturing())
        {
            float recX = m_CanvasTopLeft.x + availSize.x - 70;
            float recY = m_CanvasTopLeft.y + 10;

            drawList->AddCircleFilled(ImVec2(recX, recY + 6), 6, IM_COL32(255, 0, 0, 255));

            char recText[32];
            snprintf(recText, sizeof(recText), "REC %d", captureService->GetFrameCount());
            drawList->AddText(ImVec2(recX + 10, recY), IM_COL32(255, 255, 255, 255), recText);
        }

        ImGui::InvisibleButton("PlayViewCanvas", availSize);

        // Handle play mode input (mouse/touch forwarded to game)
        {
            bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
            ImGuiIO& io = ImGui::GetIO();
            ImVec2 mousePos = io.MousePos;

            // Convert screen position to game coordinates
            float gameX = (mousePos.x - p1.x) / dpiScale;
            float gameY = (mousePos.y - p1.y) / dpiScale;

            // Check if mouse is within the actual texture bounds
            bool inBounds = hovered &&
                            mousePos.x >= p1.x && mousePos.x < p2.x &&
                            mousePos.y >= p1.y && mousePos.y < p2.y;

            bool isDown = inBounds && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
            bool isUp = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
            bool hasDelta = (io.MouseDelta.x != 0 || io.MouseDelta.y != 0);
            bool isMove = hasDelta && (ImGui::IsMouseDown(ImGuiMouseButton_Left) || inBounds);

            if (isDown)
                m_PlayModeDragging = true;

            if (m_PlayModeDragging && (isDown || isMove || isUp))
            {
                m_App->HandlePlayModeInput(gameX, gameY, isDown, isMove, isUp);
            }
            else if (!m_TouchInputMode && inBounds && hasDelta && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                // Hover move: mouse moving over play view without clicking
                m_App->HandlePlayModeInput(gameX, gameY, false, true, false);
            }

            // When mouse leaves bounds during hover (not dragging), clear hover state
            if (!m_TouchInputMode && !inBounds && m_WasHoveringInBounds && !m_PlayModeDragging)
            {
                m_App->HandlePlayModeInput(-9999, -9999, false, true, false);
            }
            m_WasHoveringInBounds = inBounds;

            if (isUp)
            {
                m_PlayModeDragging = false;
                if (m_TouchInputMode)
                    m_App->HandlePlayModeInput(-9999, -9999, false, true, false);
            }
        }
    }
    else
    {
        ImGui::InvisibleButton("PlayViewCanvas", availSize);
    }

    ImGui::End();
}

}  // namespace DekiEditor
