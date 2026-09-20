#include "ProgressModal.h"
#include <deki-editor/EditorTheme.h>
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_opengl3.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace DekiEditor
{

SDL_Window* ProgressModal::s_Window = nullptr;
bool ProgressModal::s_Enabled = true;
bool ProgressModal::s_QuitDeferred = false;

bool ProgressModal::TakeDeferredQuit()
{
    const bool q = s_QuitDeferred;
    s_QuitDeferred = false;
    return q;
}

namespace
{
constexpr int kModalWidth = 600;
constexpr float kBarHeight = 14.0f;

struct ProgressState
{
    bool hasStartTime = false;
    uint64_t startMs = 0;
};

ProgressState g_State;

void FormatElapsed(uint64_t elapsedMs, char* out, size_t outSize)
{
    uint64_t totalSec = elapsedMs / 1000;
    uint64_t mins = totalSec / 60;
    uint64_t secs = totalSec % 60;
    uint64_t centis = (elapsedMs % 1000) / 10;
    if (mins > 0)
        std::snprintf(out, outSize, "%llum %02llus", (unsigned long long)mins, (unsigned long long)secs);
    else
        std::snprintf(out, outSize, "%llu.%02llus", (unsigned long long)secs, (unsigned long long)centis);
}

// The card itself: title + elapsed chip, phase, animated bar, detail line and,
// when cancelRequested is given, a Cancel button. Shared by Show (which owns
// its own frame) and DrawInFrame (inside the editor's frame).
void DrawCardBody(const char* title, const char* phase, const char* detail,
                  uint64_t nowMs, float dpi, bool* cancelRequested)
{
    // -------- Title row --------
    if (title && title[0] != '\0')
    {
        ImGui::TextColored(Palette::Accent, "%s", title);
        // Right-aligned elapsed-time chip on the same row
        char elapsed[32];
        FormatElapsed(nowMs - g_State.startMs, elapsed, sizeof(elapsed));
        ImVec2 ts = ImGui::CalcTextSize(elapsed);
        float avail = ImGui::GetContentRegionAvail().x;
        if (avail > ts.x)
            ImGui::SameLine(0.0f, avail - ts.x);
        else
            ImGui::SameLine();
        ImGui::TextColored(Palette::Dim, "%s", elapsed);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }

    // -------- Current phase --------
    const char* phaseDisplay = (phase && phase[0] != '\0') ? phase : "Working...";
    ImGui::TextColored(Palette::Fg, "%s", phaseDisplay);

    ImGui::Spacing();

    // -------- Animated indeterminate bar (pill, themed) --------
    {
        float progress = static_cast<float>(SDL_GetTicks() % 2000) / 2000.0f;
        ImVec2 barPos = ImGui::GetCursorScreenPos();
        ImVec2 barSize(ImGui::GetContentRegionAvail().x, kBarHeight * dpi);
        const ImVec2 barMax(barPos.x + barSize.x, barPos.y + barSize.y);
        const float rounding = barSize.y * 0.5f;  // pill

        ImDrawList* drawList = ImGui::GetWindowDrawList();

        const ImU32 colBarBg = ImGui::ColorConvertFloat4ToU32(Palette::Bg3);
        const ImU32 colBarBor = ImGui::ColorConvertFloat4ToU32(Palette::Line2);
        const ImU32 colFill = ImGui::ColorConvertFloat4ToU32(Palette::Accent);
        const ImU32 colLead = ImGui::ColorConvertFloat4ToU32(ImVec4(0.62f, 0.86f, 1.0f, 1.0f));

        // Track
        drawList->AddRectFilled(barPos, barMax, colBarBg, rounding);

        // Animated sliding segment (clipped to the rounded track).
        float segmentWidth = barSize.x * 0.28f;
        float slidePos = progress * (barSize.x + segmentWidth) - segmentWidth;
        float startX = barPos.x + (std::max)(0.0f, slidePos);
        float endX = barPos.x + (std::min)(barSize.x, slidePos + segmentWidth);
        if (endX > startX)
        {
            drawList->PushClipRect(barPos, barMax, true);
            drawList->AddRectFilled(ImVec2(startX, barPos.y), ImVec2(endX, barMax.y), colFill, rounding);
            float leadW = (std::min)(10.0f * dpi, endX - startX);
            drawList->AddRectFilled(ImVec2(endX - leadW, barPos.y), ImVec2(endX, barMax.y), colLead, rounding);
            drawList->PopClipRect();
        }

        // Track border on top.
        drawList->AddRect(barPos, barMax, colBarBor, rounding);

        ImGui::Dummy(barSize);
    }

    ImGui::Spacing();

    // -------- Detail line (current file/asset being processed) --------
    if (detail && detail[0] != '\0')
    {
        // Truncate gracefully if it overflows the window width.
        std::string text = detail;
        float maxW = ImGui::GetContentRegionAvail().x;
        if (ImGui::CalcTextSize(text.c_str()).x > maxW && text.size() > 8)
        {
            // Replace the middle with an ellipsis to keep both ends visible.
            const std::string ell = " ... ";
            while (text.size() > 16 &&
                   ImGui::CalcTextSize((text.substr(0, text.size() / 2 - ell.size() / 2) +
                                        ell +
                                        text.substr(text.size() / 2 + ell.size() / 2 + 1))
                                           .c_str())
                           .x > maxW)
            {
                text = text.substr(0, text.size() / 2 - 1) + text.substr(text.size() / 2 + 1);
            }
            text = text.substr(0, text.size() / 2 - ell.size() / 2) + ell +
                   text.substr(text.size() / 2 + ell.size() / 2 + 1);
        }
        ImGui::TextColored(Palette::Dim, "%s", text.c_str());
    }
    else
    {
        ImGui::TextColored(Palette::Dim, " ");
    }


    if (cancelRequested)
    {
        ImGui::Spacing();
        const float buttonW = 90.0f * dpi;
        const float avail = ImGui::GetContentRegionAvail().x;
        if (avail > buttonW)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - buttonW);
        if (ImGui::Button("Cancel", ImVec2(buttonW, 0.0f)))
            *cancelRequested = true;
    }
}
}  // namespace

void ProgressModal::SetWindow(SDL_Window* window)
{
    s_Window = window;
}

void ProgressModal::SetEnabled(bool enabled)
{
    s_Enabled = enabled;
}

bool ProgressModal::IsEnabled()
{
    return s_Enabled;
}

void ProgressModal::Reset()
{
    g_State = ProgressState{};
}

void ProgressModal::Show(const char* title, const char* phase, const char* detail)
{
    if (!s_Window || !s_Enabled)
        return;

    // Pump SDL events to prevent "not responding". Everything is forwarded to
    // ImGui (so focus/mouse state stays coherent) except a close request,
    // which is parked for the main loop - see TakeDeferredQuit().
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
        {
            s_QuitDeferred = true;
            continue;
        }
        ImGui_ImplSDL3_ProcessEvent(&event);
    }

    // Check if we're inside an active ImGui frame (e.g. called from DrawMenuBar)
    // We can't use ImGui mid-frame because EndFrame asserts on unclosed windows,
    // so just pump events to keep the window responsive and skip the visual.
    bool midFrame = ImGui::GetCurrentContext() && ImGui::GetCurrentContext()->WithinFrameScope;
    if (midFrame)
        return;

    // First call after Reset (or first ever): start the elapsed-time clock.
    uint64_t nowMs = SDL_GetTicks();
    if (!g_State.hasStartTime)
    {
        g_State.startMs = nowMs;
        g_State.hasStartTime = true;
    }

    // Start a new ImGui frame for the progress modal
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    // DPI scale: ImGui units are physical pixels here (EditorApp::Dpi multiplies
    // by the window DPI), so the modal must scale its size/padding too or the
    // DPI-enlarged fonts overflow a fixed box and get clipped.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float dpi = (vp->DpiScale > 0.0f) ? vp->DpiScale : 1.0f;
    const float modalW = static_cast<float>(kModalWidth) * dpi;

    // Center the modal. Fixed width, AUTO height (0) so content is never cut.
    ImVec2 center = vp->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(modalW, 0.0f), ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoScrollbar;

    // Card styling: padded, rounded, a slightly-raised panel bg with a hairline
    // border so it stands out from the cleared backdrop.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f * dpi, 20.0f * dpi));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f * dpi, 10.0f * dpi));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f * dpi);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Palette::Bg2);
    ImGui::PushStyleColor(ImGuiCol_Border, Palette::Line3);

    ImGui::Begin("##ProgressModal", nullptr, flags);

    DrawCardBody(title, phase, detail, nowMs, dpi, nullptr);
    ImGui::End();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(4);

    // Render
    ImGui::Render();

    // Clear the full framebuffer (physical pixels) to the editor background, then
    // draw the modal on top.
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(s_Window, &w, &h);
    glViewport(0, 0, w, h);
    glClearColor(Palette::Bg.x, Palette::Bg.y, Palette::Bg.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Draw ImGui
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    // Swap buffers to show the progress
    SDL_GL_SwapWindow(s_Window);
}

void ProgressModal::DrawInFrame(const char* title, const char* phase, const char* detail, bool* cancelRequested)
{
    if (!s_Enabled)
        return;

    const uint64_t nowMs = SDL_GetTicks();
    if (!g_State.hasStartTime)
    {
        g_State.startMs = nowMs;
        g_State.hasStartTime = true;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float dpi = (vp->DpiScale > 0.0f) ? vp->DpiScale : 1.0f;
    const float modalW = static_cast<float>(kModalWidth) * 0.75f * dpi;

    // Bottom-right corner of the main viewport, above the status bar: the
    // editor stays usable while the build runs.
    ImVec2 pos(vp->WorkPos.x + vp->WorkSize.x - 16.0f * dpi,
               vp->WorkPos.y + vp->WorkSize.y - 40.0f * dpi);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(modalW, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.97f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoDocking |
                             ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f * dpi, 14.0f * dpi));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f * dpi, 8.0f * dpi));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f * dpi);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Palette::Bg2);
    ImGui::PushStyleColor(ImGuiCol_Border, Palette::Line3);

    if (ImGui::Begin("##BuildProgress", nullptr, flags))
    {
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        DrawCardBody(title, phase, detail, nowMs, dpi, cancelRequested);
    }
    ImGui::End();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(4);
}

}  // namespace DekiEditor
