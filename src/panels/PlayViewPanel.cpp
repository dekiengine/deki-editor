#include "PlayViewPanel.h"
#include "../PanelMaximize.h"
#include <deki-editor/EditorTheme.h>
#include <deki-editor/IconsTabler.h>
#include "EditorApp.h"
#include "core/Project.h"
#include <deki-editor/SceneView.h>
#include <deki-editor/EditorSettings.h>
#include <deki-editor/build/PlatformConfig.h>
#include <deki-editor/TextureFormatResolve.h>
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
#include <cstring>

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

// Where a w x h game image goes in the canvas: 1:1 at the display's DPI,
// centred, and scaled down to fit when the panel is smaller. `scale` is canvas
// pixels per game pixel, which input mapping divides by.
struct FittedRect
{
    ImVec2 p1, p2;
    float scale = 1.0f;
};

static FittedRect FitToCanvas(int w, int h, const ImVec2& canvasTopLeft, const ImVec2& avail)
{
    FittedRect r;
    const float dpiScale = ImGui::GetWindowDpiScale();
    r.scale = dpiScale;
    if (w > 0 && h > 0 && avail.x > 0.0f && avail.y > 0.0f)
    {
        const float fit = std::min(avail.x / static_cast<float>(w), avail.y / static_cast<float>(h));
        if (fit < r.scale)
            r.scale = fit;
    }
    const float texW = std::round(w * r.scale);
    const float texH = std::round(h * r.scale);
    const float offsetX = std::max((avail.x - texW) * 0.5f, 0.0f);
    const float offsetY = std::max((avail.y - texH) * 0.5f, 0.0f);
    r.p1 = ImVec2(std::floor(canvasTopLeft.x + offsetX), std::floor(canvasTopLeft.y + offsetY));
    r.p2 = ImVec2(r.p1.x + texW, r.p1.y + texH);
    return r;
}

// Pixels stay sharp at 1:1 and above. Scaled down to fit the panel, nearest
// sampling drops whole rows and columns and turns dithering into moire, so the
// image is filtered instead.
static void AddGameImage(ImDrawList* drawList, ImTextureID texId, const FittedRect& fit)
{
    if (fit.scale < ImGui::GetWindowDpiScale())
        drawList->AddImage(texId, fit.p1, fit.p2);
    else
        AddImageNearest(drawList, texId, fit.p1, fit.p2);
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

    if (const Project* project = m_App->GetProject(); project && project->GetPath() != m_PreviewProject)
        LoadPreview(project->GetPath());
    ApplyPreviewSize();

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

        DrawPreviewPicker(cursorX, rowMin, rowH, dpi);
        drawDivider(cursorX);
        DrawFormatPicker(cursorX, rowMin, rowH, dpi);
        drawDivider(cursorX);

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
        const int vpW = SceneView::Get().GetPreviewWidth();
        const int vpH = SceneView::Get().GetPreviewHeight();

        if (vpW > 0 && vpH > 0)
        {
            const FittedRect fit = FitToCanvas(vpW, vpH, m_CanvasTopLeft, availSize);
            const ImVec2 p1 = fit.p1, p2 = fit.p2;
            const float texScreenW = p2.x - p1.x, texScreenH = p2.y - p1.y;

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
            const int vpW = SceneView::Get().GetPreviewWidth();
            const int vpH = SceneView::Get().GetPreviewHeight();

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
                    const FittedRect fit = FitToCanvas(vpW, vpH, m_CanvasTopLeft, availSize);
                    const ImVec2 p1 = fit.p1, p2 = fit.p2;

                    AddGameImage(drawList, (ImTextureID)(intptr_t)texId, fit);
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
            const FittedRect fit = FitToCanvas(m_PreviewRenderer->GetWidth(), m_PreviewRenderer->GetHeight(),
                                               m_CanvasTopLeft, availSize);
            AddGameImage(drawList, (ImTextureID)(intptr_t)previewTex, fit);
            drawList->AddRect(fit.p1, fit.p2, IM_COL32(100, 100, 100, 255), 0.0f, 0, 1.0f);
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
        const FittedRect fit = FitToCanvas(playW, playH, m_CanvasTopLeft, availSize);
        const ImVec2 p1 = fit.p1, p2 = fit.p2;
        const float dpiScale = fit.scale;  // canvas pixels per game pixel

        // Draw play mode texture
        AddGameImage(drawList, (ImTextureID)(intptr_t)playTexId, fit);
        drawList->AddRect(p1, p2, IM_COL32(100, 100, 100, 255), 0.0f, 0, 1.0f);

        // Status indicator
        const char* status = m_App->IsPlaying() ? "PLAYING" : "PAUSED";
        ImU32 statusColor = m_App->IsPlaying() ? IM_COL32(0, 255, 0, 200) : IM_COL32(255, 255, 0, 200);
        drawList->AddText(ImVec2(m_CanvasTopLeft.x + 10, m_CanvasTopLeft.y + 10), statusColor, status);

        // Resolution info
        char resInfo[64];
        if (fit.scale < ImGui::GetWindowDpiScale())
            snprintf(resInfo, sizeof(resInfo), "%dx%d (%.0f%%)", playW, playH,
                     100.0f * fit.scale / ImGui::GetWindowDpiScale());
        else
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

namespace
{
// The aspect ratios the picker offers, as Unity's Game view does. An aspect
// ratio previews the largest screen of that shape that fits the panel.
struct AspectChoice
{
    int w, h;
    const char* label;
};
constexpr AspectChoice kAspects[] = {
    { 16, 9, "16:9" }, { 16, 10, "16:10" }, { 4, 3, "4:3" }, { 3, 2, "3:2" }, { 21, 9, "21:9" }, { 9, 16, "9:16" },
};
constexpr const char* kFreeAspect = "free";
constexpr const char* kAspectPrefix = "aspect:";

const AspectChoice* FindAspect(const std::string& id)
{
    if (id.rfind(kAspectPrefix, 0) != 0)
        return nullptr;
    const std::string ratio = id.substr(std::strlen(kAspectPrefix));
    for (const AspectChoice& a : kAspects)
        if (ratio == a.label)
            return &a;
    return nullptr;
}
}  // namespace

void PlayViewPanel::SelectPreview(const std::string& id)
{
    // Anything unknown - including "", the design size of earlier builds - is
    // Free Aspect.
    m_PreviewId = kFreeAspect;
    if (id == "custom" || FindAspect(id) || (!id.empty() && PlatformRegistry::Instance().GetPlatform(id)))
        m_PreviewId = id;

    // A platform's own format; full colour for anything else.
    m_PreviewFormat = Deki::ColorFormat::ARGB8888;
    if (const PlatformConfig* platform = PlatformRegistry::Instance().GetPlatform(m_PreviewId))
    {
        Deki::ColorFormat f;
        if (ParseColorFormat(platform->colorFormat, f))
            m_PreviewFormat = f;
    }
}

void PlayViewPanel::LoadPreview(const std::string& projectPath)
{
    m_PreviewProject = projectPath;
    SelectPreview(EditorSettings::GetString("playView.screen:" + projectPath));

    int w = 0, h = 0;
    if (sscanf(EditorSettings::GetString("playView.customSize:" + projectPath).c_str(), "%dx%d", &w, &h) == 2)
    {
        m_CustomWidth = std::clamp(w, 1, 8192);
        m_CustomHeight = std::clamp(h, 1, 8192);
    }

    // Set after SelectPreview, which picks the screen's own format.
    Deki::ColorFormat format;
    if (ParseColorFormat(EditorSettings::GetString("playView.format:" + projectPath), format))
        m_PreviewFormat = format;
}

void PlayViewPanel::SavePreview() const
{
    if (m_PreviewProject.empty())
        return;
    EditorSettings::SetString("playView.screen:" + m_PreviewProject, m_PreviewId);
    EditorSettings::SetString("playView.customSize:" + m_PreviewProject,
                              std::to_string(m_CustomWidth) + "x" + std::to_string(m_CustomHeight));
    EditorSettings::SetString("playView.format:" + m_PreviewProject, ColorFormatName(m_PreviewFormat));
}

void PlayViewPanel::ApplyPreviewSize()
{
    SceneView::Get().SetPreviewFormat(m_PreviewFormat);
    if (m_PreviewId == "custom")
    {
        SceneView::Get().SetPreviewSize(m_CustomWidth, m_CustomHeight);
        return;
    }
    if (const PlatformConfig* platform = PlatformRegistry::Instance().GetPlatform(m_PreviewId))
    {
        if (platform->screenWidth > 0 && platform->screenHeight > 0)
        {
            SceneView::Get().SetPreviewSize(platform->screenWidth, platform->screenHeight);
            return;
        }
    }
    else if (m_PreviewId != kFreeAspect && !FindAspect(m_PreviewId))
    {
        // The platform went away (another project, a removed package).
        SelectPreview(kFreeAspect);
    }

    // Free Aspect and the aspect ratios follow the panel: last frame's canvas,
    // since this runs before the window is laid out. Play runs at the size it
    // started with, so the size holds still while it runs.
    if (!m_App->IsStopped())
        return;
    const int canvasW = static_cast<int>(m_CanvasSize.x);
    const int canvasH = static_cast<int>(m_CanvasSize.y);
    if (canvasW <= 0 || canvasH <= 0)
        return;  // not laid out yet; SceneView keeps its last size
    int w = canvasW, h = canvasH;
    if (const AspectChoice* a = FindAspect(m_PreviewId))
    {
        // The largest a:b screen that fits the panel.
        if (static_cast<int64_t>(canvasW) * a->h > static_cast<int64_t>(canvasH) * a->w)
            w = canvasH * a->w / a->h;
        else
            h = canvasW * a->h / a->w;
    }
    SceneView::Get().SetPreviewSize(std::max(w, 1), std::max(h, 1));
}

// Toolbar cell naming the previewed screen; opens a list like Unity's Game
// view: Free Aspect, aspect ratios, every known platform's screen, and a custom
// resolution. Play mode runs at the previewed size, so the choice is locked
// while it runs.
void PlayViewPanel::DrawPreviewPicker(float& cursorX, const ImVec2& rowMin, float rowH, float dpi)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 colText = ImGui::ColorConvertFloat4ToU32(DekiEditor::Palette::Dim);
    const ImU32 colHoverBg = IM_COL32(255, 255, 255, 6);

    const int w = SceneView::Get().GetPreviewWidth();
    const int h = SceneView::Get().GetPreviewHeight();
    char label[128];
    if (m_PreviewId == kFreeAspect)
        snprintf(label, sizeof(label), "Free Aspect  %dx%d", w, h);
    else if (const AspectChoice* a = FindAspect(m_PreviewId))
        snprintf(label, sizeof(label), "%s  %dx%d", a->label, w, h);
    else if (m_PreviewId == "custom")
        snprintf(label, sizeof(label), "Custom  %dx%d", w, h);
    else
    {
        const PlatformConfig* p = PlatformRegistry::Instance().GetPlatform(m_PreviewId);
        snprintf(label, sizeof(label), "%s  %dx%d", p ? p->displayName.c_str() : m_PreviewId.c_str(), w, h);
    }

    const ImVec2 textSz = ImGui::CalcTextSize(label);
    const float cellW = textSz.x + 22.0f * dpi;
    ImGui::SetCursorScreenPos(ImVec2(cursorX, rowMin.y));
    const bool locked = !m_App->IsStopped();
    ImGui::BeginDisabled(locked);
    const bool clicked = ImGui::InvisibleButton("##previewsize", ImVec2(cellW, rowH));
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        DekiEditor::Tooltip(locked ? "The screen size is fixed while playing"
                                   : "Screen to preview: a resolution, or an aspect ratio at the panel's size. Play "
                                     "runs at it; the camera decides how much world it shows.");
        dl->AddRectFilled(ImVec2(cursorX, rowMin.y), ImVec2(cursorX + cellW, rowMin.y + rowH), colHoverBg);
    }
    dl->AddText(ImVec2(cursorX + 11.0f * dpi, rowMin.y + (rowH - textSz.y) * 0.5f), colText, label);
    if (clicked)
        ImGui::OpenPopup("##previewsizes");
    ImGui::SetNextWindowPos(ImVec2(cursorX, rowMin.y + rowH));
    cursorX += cellW;

    if (!ImGui::BeginPopup("##previewsizes"))
        return;

    auto pick = [this](const std::string& id)
    {
        SelectPreview(id);
        SavePreview();
    };

    if (ImGui::Selectable("Free Aspect", m_PreviewId == kFreeAspect))
        pick(kFreeAspect);
    for (const AspectChoice& a : kAspects)
    {
        const std::string id = std::string(kAspectPrefix) + a.label;
        if (ImGui::Selectable(a.label, m_PreviewId == id))
            pick(id);
    }

    char item[192];
    ImGui::Separator();
    for (const auto& [id, name] : PlatformRegistry::Instance().GetPlatformDisplayList())
    {
        const PlatformConfig* p = PlatformRegistry::Instance().GetPlatform(id);
        if (!p || p->screenWidth <= 0 || p->screenHeight <= 0)
            continue;
        snprintf(item, sizeof(item), "%s  %dx%d##%s", name.c_str(), p->screenWidth, p->screenHeight, id.c_str());
        if (ImGui::Selectable(item, m_PreviewId == id))
            pick(id);
    }

    ImGui::Separator();
    if (ImGui::Selectable("Custom resolution", m_PreviewId == "custom", ImGuiSelectableFlags_NoAutoClosePopups) &&
        m_PreviewId != "custom")
        pick("custom");
    if (m_PreviewId == "custom")
    {
        ImGui::SetNextItemWidth(120.0f * dpi);
        bool changed = ImGui::InputInt("Width##customw", &m_CustomWidth);
        ImGui::SetNextItemWidth(120.0f * dpi);
        changed |= ImGui::InputInt("Height##customh", &m_CustomHeight);
        m_CustomWidth = std::clamp(m_CustomWidth, 1, 8192);
        m_CustomHeight = std::clamp(m_CustomHeight, 1, 8192);
        if (changed)
            SavePreview();
    }
    ImGui::EndPopup();
}

// Toolbar cell naming the previewed screen's pixel format, next to its size.
// Picking a size selects that screen's format; this changes it. Play renders
// in it, so it is locked while playing.
void PlayViewPanel::DrawFormatPicker(float& cursorX, const ImVec2& rowMin, float rowH, float dpi)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 colText = ImGui::ColorConvertFloat4ToU32(DekiEditor::Palette::Dim);
    const ImU32 colHoverBg = IM_COL32(255, 255, 255, 6);

    const char* label = ColorFormatName(m_PreviewFormat);
    const ImVec2 textSz = ImGui::CalcTextSize(label);
    const float cellW = textSz.x + 22.0f * dpi;
    ImGui::SetCursorScreenPos(ImVec2(cursorX, rowMin.y));
    const bool locked = !m_App->IsStopped();
    ImGui::BeginDisabled(locked);
    const bool clicked = ImGui::InvisibleButton("##previewformat", ImVec2(cellW, rowH));
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        DekiEditor::Tooltip(locked ? "The pixel format is fixed while playing"
                                   : "Pixel format of the previewed screen. Play renders in it.");
        dl->AddRectFilled(ImVec2(cursorX, rowMin.y), ImVec2(cursorX + cellW, rowMin.y + rowH), colHoverBg);
    }
    dl->AddText(ImVec2(cursorX + 11.0f * dpi, rowMin.y + (rowH - textSz.y) * 0.5f), colText, label);
    if (clicked)
        ImGui::OpenPopup("##previewformats");
    ImGui::SetNextWindowPos(ImVec2(cursorX, rowMin.y + rowH));
    cursorX += cellW;

    if (!ImGui::BeginPopup("##previewformats"))
        return;
    for (Deki::ColorFormat f : { Deki::ColorFormat::RGB565, Deki::ColorFormat::RGB565A8, Deki::ColorFormat::RGB888,
                                 Deki::ColorFormat::ARGB8888 })
    {
        if (ImGui::Selectable(ColorFormatName(f), m_PreviewFormat == f))
        {
            m_PreviewFormat = f;
            SavePreview();
        }
    }
    ImGui::EndPopup();
}

}  // namespace DekiEditor
