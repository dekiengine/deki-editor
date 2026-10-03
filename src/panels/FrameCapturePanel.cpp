#include "FrameCapturePanel.h"
#include <cfloat>
#include <deki-editor/IconsTabler.h>
#include <deki-editor/EditorUI.h>
#include "inspector/InspectorUtils.h"
#include "../PanelMaximize.h"
#include <deki-editor/EditorTheme.h>
#include "EditorApp.h"
#include "services/FrameCaptureService.h"
#include <deki-editor/ProcessLaunch.h>
#include "core/Project.h"
#include "imgui.h"
#include <filesystem>

namespace fs = std::filesystem;

namespace DekiEditor
{

FrameCapturePanel::FrameCapturePanel(EditorApp* app)
    : m_App(app)
{
}

void FrameCapturePanel::Draw()
{
    if (!m_Open)
        return;

    DekiEditor::FirstUseWindowPlacement(450, 300);

    if (ImGui::Begin("FRAME CAPTURE###Frame Capture", &m_Open, DekiEditor::MaximizedWindowFlags("FRAME CAPTURE###Frame Capture")))
    {
        auto* captureService = m_App->GetFrameCaptureService();
        auto* project = m_App->GetProject();
        const bool isPlaying = m_App->IsPlaying();
        const bool isCapturing = captureService && captureService->IsCapturing();

        // Default path is project/captures
        const std::string defaultPath = project ? project->GetPath() + "/captures" : "";
        if (!m_UseCustomPath)
            m_OutputPath = defaultPath;

        // Record / stop and the folder, in the panel's toolbar.
        auto& ui = EditorUI::Get();
        ui.BeginToolbar();
        if (isCapturing)
        {
            if (ui.ToolbarButton("Stop Recording", true, /*active*/ true))
                captureService->StopCapture();
        }
        else if (ui.ToolbarButton("Record", isPlaying && !m_OutputPath.empty() && captureService))
        {
            captureService->StartCapture(m_OutputPath);
        }
        if (ui.ToolbarIconButton("##openfolder", ICON_TI_FOLDER, "Open the captures folder",
                                 !m_OutputPath.empty()) &&
            fs::exists(m_OutputPath))
        {
            // Argument vector, not a shell string. The capture directory is
            // derived from the project path, so a quote in it used to end
            // the quoting and hand the rest to the shell.
            RevealInFileManager(m_OutputPath);
        }
        ui.EndToolbar();

        if (DekiEditor::SchematicSectionBegin("Capture"))
        {
            DekiEditor::BeginPropertyContext();
            InspectorUtils::BeginPropertyRow("Status");
            ImGui::AlignTextToFramePadding();
            if (isCapturing)
            {
                ImGui::TextColored(DekiEditor::Palette::Red, "Recording");
                ImGui::SameLine();
                ImGui::TextColored(DekiEditor::Palette::Dim, "%d frames", captureService->GetFrameCount());
            }
            else if (isPlaying)
                ImGui::TextColored(DekiEditor::Palette::Green, "Ready");
            else
                ImGui::TextColored(DekiEditor::Palette::Dim, "Records in Play mode");

            InspectorUtils::BeginPropertyRow("Custom Folder");
            DekiEditor::SchematicCheckbox("##customfolder", &m_UseCustomPath);

            InspectorUtils::BeginPropertyRow("Folder");
            if (m_UseCustomPath)
            {
                char pathBuf[512] = {};
                strncpy(pathBuf, m_OutputPath.c_str(), sizeof(pathBuf) - 1);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::InputText("##OutputPath", pathBuf, sizeof(pathBuf)))
                    m_OutputPath = pathBuf;
            }
            else
            {
                ImGui::PushStyleColor(ImGuiCol_Text, DekiEditor::Palette::Dim);
                ImGui::TextWrapped("%s", defaultPath.empty() ? "No project open" : defaultPath.c_str());
                ImGui::PopStyleColor();
            }
            DekiEditor::EndPropertyContext();
            DekiEditor::SchematicSectionEnd();
        }

        if (DekiEditor::SchematicSectionBegin("Make a Video"))
        {
            // One PNG per frame; ffmpeg turns them into a video. At the right
            // speed means rendering really kept up with 30 FPS.
            static constexpr const char* kCommand = "ffmpeg -framerate 30 -i frame_%05d.png -c:v libx264 output.mp4";
            DekiEditor::BeginPropertyContext();
            InspectorUtils::BeginPropertyRow("ffmpeg");
            if (g_FontMonoSmall)
                ImGui::PushFont(g_FontMonoSmall, DekiEditor::Fonts::CssPx(g_FontMonoSmall, 11.5f));
            ImGui::TextWrapped("%s", kCommand);
            if (g_FontMonoSmall)
                ImGui::PopFont();
            InspectorUtils::BeginPropertyRow("");
            ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
            if (ImGui::Button("Copy Command"))
                ImGui::SetClipboardText(kCommand);
            ImGui::PopStyleVar();
            DekiEditor::EndPropertyContext();
            DekiEditor::SchematicSectionEnd();
        }
    }

    ImGui::End();
}

}  // namespace DekiEditor
