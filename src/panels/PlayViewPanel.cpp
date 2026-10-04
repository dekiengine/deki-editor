#include "PlayViewPanel.h"
#include "../PanelMaximize.h"
#include <deki-editor/EditorTheme.h>
#include <deki-editor/Motion.h>
#include <deki-editor/IconsTabler.h>
#include "EditorApp.h"
#include "inspector/InspectorUtils.h"
#include <nlohmann/json.hpp>
#include "core/Project.h"
#include <deki-editor/SceneView.h>
#include <deki-editor/EditorSettings.h>
#include <deki-editor/build/PlatformConfig.h>
#include <deki-editor/TextureFormatResolve.h>
#include "scene_view/EditModeRenderer.h"
#include "services/FrameCaptureService.h"
#include "services/TouchInput.h"
#include "InspectorPanel.h"
#include "SceneViewPanel.h"
#include <deki-editor/CommandHistory.h>
#include <deki/assets/AssetManager.h>
#include <deki/Scene.h>
#include <deki/Object.h>
#include <deki/ICamera.h>
#include "InputKeys.h"
#include "imgui.h"
#include "imgui_internal.h"  // ImGuiSelectableFlags_SpanAvailWidth
#include <SDL3/SDL_opengl.h>
#include <SDL3/SDL_video.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace DekiEditor
{

using InspectorUtils::BeginPropertyRow;

// ImGui's GL backend binds a GL_LINEAR sampler object that overrides the texture's
// own GL_NEAREST params, blurring the play view when the project resolution doesn't
// land on integer screen pixels. Wrap each AddImage with this pair so the texture's
// nearest-neighbor filtering takes effect (same trick used in SceneViewPanel).
static void AddImageNearest(ImDrawList* drawList, ImTextureID texId, const ImVec2& p1, const ImVec2& p2)
{
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
    drawList->AddImage(texId, p1, p2);
    drawList->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

// Where a w x h game image goes in the canvas: 1:1 at the display's DPI,
// centred, and scaled down to fit when the panel is smaller. `scale` is canvas
// pixels per game pixel, which input mapping divides by.
struct FittedRect
{
    ImVec2 p1, p2;
    ImVec2 clipMin, clipMax;  // the canvas: a picture bigger than it is cropped
    float scale = 1.0f;       // canvas pixels per game pixel
};

// The picture one game pixel per screen pixel, centred in the canvas. It is
// never resized to fit: a screen bigger than the panel is cropped.
static FittedRect PlaceInCanvas(int w, int h, const ImVec2& canvasTopLeft, const ImVec2& avail)
{
    FittedRect r;
    r.scale = ImGui::GetWindowDpiScale();
    const float texW = std::round(w * r.scale);
    const float texH = std::round(h * r.scale);
    r.p1 = ImVec2(std::floor(canvasTopLeft.x + (avail.x - texW) * 0.5f),
                  std::floor(canvasTopLeft.y + (avail.y - texH) * 0.5f));
    r.p2 = ImVec2(r.p1.x + texW, r.p1.y + texH);
    r.clipMin = canvasTopLeft;
    r.clipMax = ImVec2(canvasTopLeft.x + avail.x, canvasTopLeft.y + avail.y);
    return r;
}

// Sharp pixels, clipped to the canvas, with its outline.
static void AddGameImage(ImDrawList* drawList, ImTextureID texId, const FittedRect& fit)
{
    drawList->PushClipRect(fit.clipMin, fit.clipMax, true);
    AddImageNearest(drawList, texId, fit.p1, fit.p2);
    drawList->AddRect(fit.p1, fit.p2, IM_COL32(100, 100, 100, 255), 0.0f, 0, 1.0f);
    drawList->PopClipRect();
}

PlayViewPanel::PlayViewPanel(EditorApp* app)
    : m_App(app),
      m_PreviewRenderer(std::make_unique<EditModeRenderer>())
{
}

PlayViewPanel::~PlayViewPanel() = default;

void PlayViewPanel::Draw()
{
    if (!isOpen)
    {
        return;
    }

    if (const Project* project = m_App->GetProject(); project && project->GetPath() != m_PreviewProject)
    {
        LoadPreview(project->GetPath());
    }
    ApplyPreviewSize();

    const bool visible = ImGui::Begin("PLAY VIEW###Play View", &isOpen,
                                      DekiEditor::MaximizedWindowFlags("PLAY VIEW###Play View") |
                                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

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

        forward(ImGuiKey_Space, Deki::InputKeys::Space);
        forward(ImGuiKey_Enter, Deki::InputKeys::Enter);
        forward(ImGuiKey_Escape, Deki::InputKeys::Esc);
        forward(ImGuiKey_Backspace, Deki::InputKeys::Backspace);
        forward(ImGuiKey_UpArrow, Deki::InputKeys::Up);
        forward(ImGuiKey_DownArrow, Deki::InputKeys::Down);
        forward(ImGuiKey_LeftArrow, Deki::InputKeys::Left);
        forward(ImGuiKey_RightArrow, Deki::InputKeys::Right);
        for (int i = 0; i < 26; ++i)  // ImGuiKey_A..Z and Deki::InputKeys::A..Z are both contiguous
        {
            forward(static_cast<ImGuiKey>(ImGuiKey_A + i), Deki::InputKeys::A + static_cast<uint32_t>(i));
        }
    }

    // A hidden tab (another tab selected in its dock) or a collapsed window
    // draws nothing. Drawing anyway rendered the whole scene every frame behind
    // the Scene view and held the editor near 40 fps. The key releases above
    // still go through, so no game key sticks down.
    if (!visible)
    {
        m_PreviewStale = true;  // the scene may change while hidden
        ImGui::End();
        return;
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
        auto drawDivider = [&](float x) { dl->AddLine(ImVec2(x, rowMin.y), ImVec2(x, rowMax.y), colLine, 1.0f); };

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
            if (hovered)
            {
                DekiEditor::Tooltip(on ? "Simulate touch: on" : "Simulate touch: off");
            }
            if (const float hoverT = DekiEditor::Motion::ItemHover(hovered); hoverT > 0.0f)
            {
                dl->AddRectFilled(ImVec2(cursorX, rowMin.y), ImVec2(cursorX + w, rowMax.y),
                                  DekiEditor::Motion::WithAlpha(colHoverBg, hoverT));
            }
            if (const float pressT = DekiEditor::Motion::ItemPress(); pressT > 0.0f)
            {
                dl->AddRectFilled(ImVec2(cursorX, rowMin.y), ImVec2(cursorX + w, rowMax.y),
                                  DekiEditor::Motion::PressShade(pressT));
            }
            dl->AddText(ImVec2(cursorX + (w - iconSz.x) * 0.5f, rowMin.y + (rowH - iconSz.y) * 0.5f),
                        on ? colAccent : colDim, icon);
            cursorX += w;
            drawDivider(cursorX);
            if (clicked)
            {
                m_TouchInputMode = !m_TouchInputMode;
            }
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
            {
                snprintf(fpsBuf, sizeof(fpsBuf), "Editor %.0f fps    Game %.0f fps", editorFps, gameFps);
            }
            else
            {
                snprintf(fpsBuf, sizeof(fpsBuf), "Editor %.0f fps", editorFps);
            }

            if (DekiEditor::g_FontMonoSmall)
            {
                ImGui::PushFont(DekiEditor::g_FontMonoSmall,
                                DekiEditor::Fonts::CssPx(DekiEditor::g_FontMonoSmall, 11.5f));
            }
            const ImVec2 fpsSz = ImGui::CalcTextSize(fpsBuf);
            const float padR = 12.0f * dpi;
            const float fpsX = rowMax.x - fpsSz.x - padR;
            if (fpsX > cursorX + 8.0f * dpi)
            {
                dl->AddText(ImVec2(fpsX, rowMin.y + (rowH - fpsSz.y) * 0.5f), colDim, fpsBuf);
            }
            if (DekiEditor::g_FontMonoSmall)
            {
                ImGui::PopFont();
            }
        }

        // Reserve the row in layout flow and close the bottom edge with a hairline.
        ImGui::SetCursorScreenPos(ImVec2(rowMin.x, rowMax.y));
        dl->AddLine(ImVec2(rowMin.x, rowMax.y), ImVec2(rowMax.x, rowMax.y), colLine, 1.0f);
    }

    ImVec2 availSize = ImGui::GetContentRegionAvail();
    m_CanvasTopLeft = ImGui::GetCursorScreenPos();
    m_CanvasSize = availSize;
    m_CanvasDpi = std::max(ImGui::GetWindowDpiScale(), 0.01f);

    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Check if render pipeline is configured
    auto* project = m_App->GetProject();
    if (project && project->GetRenderPipelineGuid().empty())
    {
        const int vpW = SceneView::Get().GetPreviewWidth();
        const int vpH = SceneView::Get().GetPreviewHeight();

        if (vpW > 0 && vpH > 0)
        {
            const FittedRect fit = PlaceInCanvas(vpW, vpH, m_CanvasTopLeft, availSize);
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
            for (Deki::Object* obj : scene->GetObjects())
            {
                cam = findCamera(obj);
                if (cam)
                {
                    break;
                }
            }

            if (cam && vpW > 0 && vpH > 0)
            {
                // Initialize/resize preview renderer to project display dimensions
                if (!m_PreviewRenderer->IsInitialized() || m_PreviewRenderer->GetWidth() != vpW ||
                    m_PreviewRenderer->GetHeight() != vpH)
                {
                    m_PreviewRenderer->Initialize(vpW, vpH);
                    m_PreviewStale = true;
                }

                // Render using scene camera, only when what it shows moved.
                uint8_t clearR, clearG, clearB;
                cam->GetClearColor(clearR, clearG, clearB);
                const uint64_t generation = CommandHistory::Instance().GetGeneration();
                const uint64_t assetEpoch = Deki::AssetManager::Get()->GetEpoch();
                const bool renderNeeded = m_PreviewStale || m_PreviewScene != scene || m_PreviewCamera != cam ||
                                          m_PreviewGeneration != generation || m_PreviewAssetEpoch != assetEpoch ||
                                          m_PreviewClear[0] != clearR || m_PreviewClear[1] != clearG ||
                                          m_PreviewClear[2] != clearB ||
                                          // Live edits change objects without a command (see SceneViewPanel).
                                          InspectorPanel::IsAnyLiveEditing() || SceneViewPanel::IsEditingObjects();
                if (renderNeeded)
                {
                    m_PreviewRenderer->RenderScene(scene, cam, clearR, clearG, clearB);
                    m_PreviewStale = false;
                    m_PreviewScene = scene;
                    m_PreviewCamera = cam;
                    m_PreviewGeneration = generation;
                    m_PreviewAssetEpoch = assetEpoch;
                    m_PreviewClear[0] = clearR;
                    m_PreviewClear[1] = clearG;
                    m_PreviewClear[2] = clearB;
                }

                uint32_t texId = m_PreviewRenderer->GetTextureId();
                if (texId != 0)
                {
                    const FittedRect fit = PlaceInCanvas(vpW, vpH, m_CanvasTopLeft, availSize);
                    AddGameImage(drawList, (ImTextureID)(intptr_t)texId, fit);

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

    // Play and pause edit a copy of the scene; render fresh after Stop.
    m_PreviewStale = true;

    // Loading state - show edit-mode preview until first play frame is ready
    if (!m_App->IsPlayModeReady())
    {
        uint32_t previewTex = m_PreviewRenderer->GetTextureId();
        if (previewTex != 0)
        {
            const FittedRect fit = PlaceInCanvas(m_PreviewRenderer->GetWidth(), m_PreviewRenderer->GetHeight(),
                                                 m_CanvasTopLeft, availSize);
            AddGameImage(drawList, (ImTextureID)(intptr_t)previewTex, fit);
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
        const FittedRect fit = PlaceInCanvas(playW, playH, m_CanvasTopLeft, availSize);
        const ImVec2 p1 = fit.p1, p2 = fit.p2;
        const float dpiScale = fit.scale;  // canvas pixels per game pixel

        // Draw play mode texture
        AddGameImage(drawList, (ImTextureID)(intptr_t)playTexId, fit);

        // Status indicator
        const char* status = m_App->IsPlaying() ? "PLAYING" : "PAUSED";
        ImU32 statusColor = m_App->IsPlaying() ? IM_COL32(0, 255, 0, 200) : IM_COL32(255, 255, 0, 200);
        drawList->AddText(ImVec2(m_CanvasTopLeft.x + 10, m_CanvasTopLeft.y + 10), statusColor, status);

        // Resolution info
        char resInfo[64];
        snprintf(resInfo, sizeof(resInfo), "%dx%d", playW, playH);
        drawList->AddText(ImVec2(m_CanvasTopLeft.x + 10, m_CanvasTopLeft.y + 25), IM_COL32(200, 200, 200, 200),
                          resInfo);

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

        // A finger on the running game is the game's touch, pressed the moment
        // it lands, not the editor's tap-or-hold-or-scroll.
        DekiEditor::TouchInput::Get().AddDirectRegion(p1, p2);

        // Handle play mode input (mouse/touch forwarded to game)
        {
            bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
            ImGuiIO& io = ImGui::GetIO();
            ImVec2 mousePos = io.MousePos;

            // Convert screen position to game coordinates
            float gameX = (mousePos.x - p1.x) / dpiScale;
            float gameY = (mousePos.y - p1.y) / dpiScale;

            // Check if mouse is within the actual texture bounds
            bool inBounds =
                hovered && mousePos.x >= p1.x && mousePos.x < p2.x && mousePos.y >= p1.y && mousePos.y < p2.y;

            bool isDown = inBounds && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
            bool isUp = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
            bool hasDelta = (io.MouseDelta.x != 0 || io.MouseDelta.y != 0);
            bool isMove = hasDelta && (ImGui::IsMouseDown(ImGuiMouseButton_Left) || inBounds);

            if (isDown)
            {
                m_PlayModeDragging = true;
            }

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
                {
                    m_App->HandlePlayModeInput(-9999, -9999, false, true, false);
                }
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
// Screen sizes the picker offers, as Unity's Game view does. An aspect ratio
// previews the largest screen of that shape that fits the panel; a resolution
// is exactly that many pixels.
//
// Ids, as remembered per project: "free" (Free Aspect: the panel), "aspect:W:H",
// "res:WxH", or a platform id (its screen).
constexpr const char* kFreeAspect = "free";

struct BuiltInAspect
{
    int w, h;
};
constexpr BuiltInAspect kAspects[] = { { 16, 9 }, { 16, 10 }, { 4, 3 }, { 3, 2 }, { 21, 9 }, { 9, 16 } };

constexpr const char* kUserSizesKey = "playView.userScreens";

std::string AspectId(int w, int h)
{
    return "aspect:" + std::to_string(w) + ":" + std::to_string(h);
}

std::string ResolutionId(int w, int h)
{
    return "res:" + std::to_string(w) + "x" + std::to_string(h);
}

bool ParseAspect(const std::string& id, int& w, int& h)
{
    return sscanf(id.c_str(), "aspect:%d:%d", &w, &h) == 2 && w > 0 && h > 0;
}

bool ParseResolution(const std::string& id, int& w, int& h)
{
    return sscanf(id.c_str(), "res:%dx%d", &w, &h) == 2 && w > 0 && h > 0 && w <= 8192 && h <= 8192;
}
}  // namespace

std::string PlayViewPanel::UserScreen::Id() const
{
    return aspect ? AspectId(w, h) : ResolutionId(w, h);
}

void PlayViewPanel::LoadUserScreens()
{
    m_UserScreens.clear();
    try
    {
        const nlohmann::json list = nlohmann::json::parse(EditorSettings::GetString(kUserSizesKey, "[]"));
        for (const auto& e : list)
        {
            UserScreen s;
            s.name = e.value("name", std::string());
            s.aspect = e.value("aspect", false);
            s.w = e.value("w", 0);
            s.h = e.value("h", 0);
            if (s.w > 0 && s.h > 0)
            {
                m_UserScreens.push_back(s);
            }
        }
    }
    catch (...)
    {
        // A hand-edited list that does not parse is simply empty.
    }
}

void PlayViewPanel::SaveUserScreens() const
{
    nlohmann::json list = nlohmann::json::array();
    for (const UserScreen& s : m_UserScreens)
    {
        list.push_back({ { "name", s.name }, { "aspect", s.aspect }, { "w", s.w }, { "h", s.h } });
    }
    EditorSettings::SetString(kUserSizesKey, list.dump());
}

std::string PlayViewPanel::ScreenLabel(const std::string& id) const
{
    for (const UserScreen& s : m_UserScreens)
    {
        if (!s.name.empty() && s.Id() == id)
        {
            return s.name;
        }
    }
    int w = 0, h = 0;
    if (id == kFreeAspect)
    {
        return "Free Aspect";
    }
    if (ParseAspect(id, w, h))
    {
        return std::to_string(w) + ":" + std::to_string(h);
    }
    if (ParseResolution(id, w, h))
    {
        return std::to_string(w) + "x" + std::to_string(h);
    }
    const PlatformConfig* p = PlatformRegistry::Instance().GetPlatform(id);
    return p ? p->displayName : id;
}

void PlayViewPanel::SelectPreview(const std::string& id)
{
    // Anything unknown - including "", the design size of earlier builds - is
    // Free Aspect.
    int w = 0, h = 0;
    m_PreviewId = kFreeAspect;
    if (ParseAspect(id, w, h) || ParseResolution(id, w, h) ||
        (!id.empty() && PlatformRegistry::Instance().GetPlatform(id)))
    {
        m_PreviewId = id;
    }

    // A platform's own format; full colour for anything else.
    m_PreviewFormat = Deki::ColorFormat::ARGB8888;
    if (const PlatformConfig* platform = PlatformRegistry::Instance().GetPlatform(m_PreviewId))
    {
        Deki::ColorFormat f;
        if (ParseColorFormat(platform->colorFormat, f))
        {
            m_PreviewFormat = f;
        }
    }
}

void PlayViewPanel::LoadPreview(const std::string& projectPath)
{
    m_PreviewProject = projectPath;
    if (!m_UserScreensLoaded)
    {
        LoadUserScreens();
        m_UserScreensLoaded = true;
    }

    std::string id = EditorSettings::GetString("playView.screen:" + projectPath);
    if (id == "custom")
    {
        // The custom size of earlier builds is a resolution like any other.
        int w = 0, h = 0;
        if (sscanf(EditorSettings::GetString("playView.customSize:" + projectPath).c_str(), "%dx%d", &w, &h) == 2)
        {
            id = ResolutionId(std::clamp(w, 1, 8192), std::clamp(h, 1, 8192));
        }
    }
    SelectPreview(id);

    // Set after SelectPreview, which picks the screen's own format.
    Deki::ColorFormat format;
    if (ParseColorFormat(EditorSettings::GetString("playView.format:" + projectPath), format))
    {
        m_PreviewFormat = format;
    }
}

void PlayViewPanel::SavePreview() const
{
    if (m_PreviewProject.empty())
    {
        return;
    }
    EditorSettings::SetString("playView.screen:" + m_PreviewProject, m_PreviewId);
    EditorSettings::SetString("playView.format:" + m_PreviewProject, ColorFormatName(m_PreviewFormat));
}

void PlayViewPanel::ApplyPreviewSize()
{
    SceneView::Get().SetPreviewFormat(m_PreviewFormat);
    int aw = 0, ah = 0;
    if (ParseResolution(m_PreviewId, aw, ah))
    {
        SceneView::Get().SetPreviewSize(aw, ah);
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
    else if (m_PreviewId != kFreeAspect && !ParseAspect(m_PreviewId, aw, ah))
    {
        // The platform went away (another project, a removed package).
        SelectPreview(kFreeAspect);
    }

    // Free Aspect and the aspect ratios follow the panel: last frame's canvas,
    // since this runs before the window is laid out. Play runs at the size it
    // started with, so the size holds still while it runs.
    if (!m_App->IsStopped())
    {
        return;
    }
    // In screen pixels.
    const int canvasW = static_cast<int>(m_CanvasSize.x / m_CanvasDpi);
    const int canvasH = static_cast<int>(m_CanvasSize.y / m_CanvasDpi);
    if (canvasW <= 0 || canvasH <= 0)
    {
        return;  // not laid out yet; SceneView keeps its last size
    }
    int w = canvasW, h = canvasH;
    if (ParseAspect(m_PreviewId, aw, ah))
    {
        // The largest aw:ah screen that fits the panel.
        if (static_cast<int64_t>(canvasW) * ah > static_cast<int64_t>(canvasH) * aw)
        {
            w = static_cast<int>(static_cast<int64_t>(canvasH) * aw / ah);
        }
        else
        {
            h = static_cast<int>(static_cast<int64_t>(canvasW) * ah / aw);
        }
    }
    SceneView::Get().SetPreviewSize(std::max(w, 1), std::max(h, 1));
}

// The screen picker: the toolbar dropdown every panel uses, with the list
// Unity's Game view has - Free Aspect, aspect ratios, every known platform's
// screen, your own sizes, and Add... for more. Your own sizes are yours across
// projects; right-click one to remove it. Play mode runs at the previewed size,
// so the choice is locked while it runs.
void PlayViewPanel::DrawPreviewPicker(float& cursorX, const ImVec2& rowMin, float rowH, float dpi)
{
    (void)dpi;
    // The entry's name and the size it previews at; once when they are the same.
    const std::string label = ScreenLabel(m_PreviewId);
    char size[32];
    snprintf(size, sizeof(size), "%dx%d", SceneView::Get().GetPreviewWidth(), SceneView::Get().GetPreviewHeight());
    char preview[160];
    snprintf(preview, sizeof(preview), "%s", label == size ? size : (label + "  " + size).c_str());

    const bool locked = !m_App->IsStopped();
    ImGui::SetCursorScreenPos(ImVec2(cursorX, rowMin.y));
    const float width = SchematicToolbarComboWidth(preview);
    ImGui::SetNextItemWidth(width);
    ImGui::BeginDisabled(locked);
    const bool open = SchematicBeginToolbarCombo("##previewsize", preview, rowH);
    if (!open && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        DekiEditor::Tooltip(locked ? "The screen size is fixed while playing"
                                   : "Screen to preview: a resolution, or an aspect ratio at the panel's size. "
                                     "Play runs at it.");
    }
    cursorX += width;

    if (open)
    {
        auto row = [this](const std::string& id, const std::string& label, int userIndex)
        {
            if (ImGui::Selectable((label + "##" + id + std::to_string(userIndex)).c_str(), m_PreviewId == id,
                                  ImGuiSelectableFlags_SpanAvailWidth))
            {
                SelectPreview(id);
                SavePreview();
            }
            if (m_PreviewId == id)
            {
                ImGui::SetItemDefaultFocus();
            }
            if (userIndex < 0)
            {
                return;
            }
            PushContextMenuPopupStyle();
            if (ImGui::BeginPopupContextItem())
            {
                PushContextMenuItemStyle();
                if (ImGui::MenuItem("Remove"))
                {
                    m_RemoveUserScreen = userIndex;
                }
                ImGui::PopStyleVar();
                ImGui::EndPopup();
            }
            ImGui::PopStyleVar();
        };
        auto userLabel = [](const UserScreen& s)
        {
            std::string dims = s.aspect ? std::to_string(s.w) + ":" + std::to_string(s.h)
                                        : std::to_string(s.w) + "x" + std::to_string(s.h);
            return s.name.empty() ? dims : s.name + "  " + dims;
        };

        row(kFreeAspect, "Free Aspect", -1);

        ImGui::Separator();
        for (const BuiltInAspect& a : kAspects)
        {
            row(AspectId(a.w, a.h), std::to_string(a.w) + ":" + std::to_string(a.h), -1);
        }
        for (size_t i = 0; i < m_UserScreens.size(); ++i)
        {
            if (m_UserScreens[i].aspect)
            {
                row(m_UserScreens[i].Id(), userLabel(m_UserScreens[i]), static_cast<int>(i));
            }
        }

        ImGui::Separator();
        for (const auto& [id, name] : PlatformRegistry::Instance().GetPlatformDisplayList())
        {
            const PlatformConfig* p = PlatformRegistry::Instance().GetPlatform(id);
            if (!p || p->screenWidth <= 0 || p->screenHeight <= 0)
            {
                continue;
            }
            row(id, name + "  " + std::to_string(p->screenWidth) + "x" + std::to_string(p->screenHeight), -1);
        }
        for (size_t i = 0; i < m_UserScreens.size(); ++i)
        {
            if (!m_UserScreens[i].aspect)
            {
                row(m_UserScreens[i].Id(), userLabel(m_UserScreens[i]), static_cast<int>(i));
            }
        }

        ImGui::Separator();
        if (ImGui::Selectable("Add...", false, ImGuiSelectableFlags_SpanAvailWidth))
        {
            m_OpenAddScreen = true;
        }

        SchematicEndToolbarCombo();
    }
    ImGui::EndDisabled();

    if (m_RemoveUserScreen >= 0 && m_RemoveUserScreen < static_cast<int>(m_UserScreens.size()))
    {
        m_UserScreens.erase(m_UserScreens.begin() + m_RemoveUserScreen);
        SaveUserScreens();
    }
    m_RemoveUserScreen = -1;

    DrawAddScreenDialog();
}

// Add a screen size of your own: an aspect ratio or a resolution, with an
// optional name. Kept for every project, like Unity's custom Game view sizes.
void PlayViewPanel::DrawAddScreenDialog()
{
    if (m_OpenAddScreen)
    {
        m_OpenAddScreen = false;
        m_NewScreen = UserScreen{};
        m_NewScreen.w = 1920;
        m_NewScreen.h = 1080;
        m_NewScreenName[0] = '\0';
        ImGui::OpenPopup("Add Screen Size");
    }
    if (!BeginSchematicModal("Add Screen Size", 360.0f))
    {
        return;
    }

    BeginPropertyContext();
    BeginPropertyRow("Name");
    ImGui::InputTextWithHint("##name", "Optional", m_NewScreenName, sizeof(m_NewScreenName));
    BeginPropertyRow("Type");
    int type = m_NewScreen.aspect ? 0 : 1;
    if (SchematicCombo("##type", &type, "Aspect Ratio\0Resolution\0"))
    {
        m_NewScreen.aspect = (type == 0);
    }
    BeginPropertyRow("Width");
    SchematicDragInt("##w", &m_NewScreen.w, 1.0f, 1, 8192);
    BeginPropertyRow("Height");
    SchematicDragInt("##h", &m_NewScreen.h, 1.0f, 1, 8192);
    EndPropertyContext();
    m_NewScreen.w = std::clamp(m_NewScreen.w, 1, 8192);
    m_NewScreen.h = std::clamp(m_NewScreen.h, 1, 8192);

    const int button = SchematicDialogButtons({ { "Cancel" }, { "Add", DialogButton::Primary } });
    if (button == 1)
    {
        m_NewScreen.name = m_NewScreenName;
        m_UserScreens.push_back(m_NewScreen);
        SaveUserScreens();
        SelectPreview(m_NewScreen.Id());
        SavePreview();
        ImGui::CloseCurrentPopup();
    }
    else if (button == 0)
    {
        ImGui::CloseCurrentPopup();
    }
    EndSchematicModal();
}

// The previewed screen's pixel format, next to its size, in the same toolbar
// dropdown. Picking a size selects that screen's format; this changes it. Play
// renders in it, so it is locked while playing.
void PlayViewPanel::DrawFormatPicker(float& cursorX, const ImVec2& rowMin, float rowH, float dpi)
{
    (void)dpi;
    const char* preview = ColorFormatName(m_PreviewFormat);
    const bool locked = !m_App->IsStopped();
    ImGui::SetCursorScreenPos(ImVec2(cursorX, rowMin.y));
    const float width = SchematicToolbarComboWidth(preview);
    ImGui::SetNextItemWidth(width);
    ImGui::BeginDisabled(locked);
    const bool open = SchematicBeginToolbarCombo("##previewformat", preview, rowH);
    if (!open && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        DekiEditor::Tooltip(locked ? "The pixel format is fixed while playing"
                                   : "Pixel format of the previewed screen. Play renders in it.");
    }
    cursorX += width;
    if (open)
    {
        for (Deki::ColorFormat f : { Deki::ColorFormat::RGB565, Deki::ColorFormat::RGB565A8, Deki::ColorFormat::RGB888,
                                     Deki::ColorFormat::ARGB8888 })
        {
            if (ImGui::Selectable(ColorFormatName(f), m_PreviewFormat == f, ImGuiSelectableFlags_SpanAvailWidth))
            {
                m_PreviewFormat = f;
                SavePreview();
            }
        }
        SchematicEndToolbarCombo();
    }
    ImGui::EndDisabled();
}

}  // namespace DekiEditor
