#include "FrameCapturePanel.h"
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

    ImGui::SetNextWindowSize(ImVec2(450, 300), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("FRAME CAPTURE###Frame Capture", &m_Open, DekiEditor::MaximizedWindowFlags("FRAME CAPTURE###Frame Capture")))
    {
        auto* captureService = m_App->GetFrameCaptureService();
        auto* project = m_App->GetProject();
        bool isPlaying = m_App->IsPlaying();
        bool isCapturing = captureService && captureService->IsCapturing();

        // Status bar
        if (isCapturing)
        {
            ImGui::TextColored(DekiEditor::Palette::Red, "RECORDING");
            ImGui::SameLine();
            ImGui::Text("| %d frames captured", captureService->GetFrameCount());
        }
        else if (isPlaying)
        {
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "READY");
            ImGui::SameLine();
            ImGui::TextDisabled("| Press Record to start capturing");
        }
        else
        {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "STOPPED");
            ImGui::SameLine();
            ImGui::TextDisabled("| Enter Play Mode to enable recording");
        }

        ImGui::Separator();

        // Output path configuration
        ImGui::Text("Output Settings");

        // Default path is project/captures
        std::string defaultPath = project ? project->GetPath() + "/captures" : "";

        DekiEditor::SchematicCheckbox("Custom output path", &m_UseCustomPath);

        if (m_UseCustomPath)
        {
            char pathBuf[512] = {};
            strncpy(pathBuf, m_OutputPath.c_str(), sizeof(pathBuf) - 1);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##OutputPath", pathBuf, sizeof(pathBuf)))
            {
                m_OutputPath = pathBuf;
            }
        }
        else
        {
            ImGui::TextDisabled("Path: %s", defaultPath.empty() ? "(no project)" : defaultPath.c_str());
            m_OutputPath = defaultPath;
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Control buttons
        float buttonWidth = 120.0f;

        if (isCapturing)
        {
            // Stop button (red)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, DekiEditor::Palette::Red);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.7f, 0.1f, 0.1f, 1.0f));

            if (ImGui::Button("Stop Recording", ImVec2(buttonWidth, 30)))
            {
                captureService->StopCapture();
            }

            ImGui::PopStyleColor(3);
        }
        else
        {
            // Record button (only enabled in play mode with valid path)
            bool canRecord = isPlaying && !m_OutputPath.empty() && captureService;

            if (!canRecord)
            {
                ImGui::BeginDisabled();
            }

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, DekiEditor::Palette::Red);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.7f, 0.1f, 0.1f, 1.0f));

            if (ImGui::Button("Start Recording", ImVec2(buttonWidth, 30)))
            {
                captureService->StartCapture(m_OutputPath);
            }

            ImGui::PopStyleColor(3);

            if (!canRecord)
            {
                ImGui::EndDisabled();
            }
        }

        ImGui::SameLine();

        // Open folder button
        if (ImGui::Button("Open Folder", ImVec2(buttonWidth, 30)) && !m_OutputPath.empty())
        {
            if (fs::exists(m_OutputPath))
            {
                // Argument vector, not a shell string. The capture directory is
                // derived from the project path, so a quote in it used to end
                // the quoting and hand the rest to the shell. Also works off
                // Windows now, where this button previously did nothing.
                RevealInFileManager(m_OutputPath);
            }
        }

        ImGui::Spacing();
        ImGui::Separator();

        // Instructions
        ImGui::TextDisabled("Instructions:");
        ImGui::BulletText("Enter Play Mode");
        ImGui::BulletText("Click 'Start Recording' to capture frames");
        ImGui::BulletText("Click 'Stop Recording' when done");
        ImGui::BulletText("Use ffmpeg to create video:");

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.9f, 0.7f, 1.0f));
        ImGui::TextWrapped("ffmpeg -framerate 30 -i frame_%%05d.png -c:v libx264 output.mp4");
        ImGui::PopStyleColor();

        // Copy command button
        if (ImGui::SmallButton("Copy Command"))
        {
            ImGui::SetClipboardText("ffmpeg -framerate 30 -i frame_%05d.png -c:v libx264 output.mp4");
        }

        ImGui::Spacing();
        ImGui::TextDisabled("If video plays at correct speed, rendering is truly 30 FPS.");
    }

    ImGui::End();
}

}  // namespace DekiEditor
