#include "EditorSettingsPanel.h"

#include "../PanelMaximize.h"
#include "inspector/InspectorUtils.h"
#include "../services/EditorLogFile.h"
#include "../services/UpdateCheck.h"

#include <deki-editor/EditorSettings.h>
#include <deki-editor/EditorTheme.h>
#include <deki-editor/Motion.h>

#include "imgui.h"

#include <cstring>
#include <string>

namespace DekiEditor
{

using InspectorUtils::BeginPropertyRow;

namespace
{
struct SmoothingChoice
{
    const char* value;  // stored
    const char* label;  // shown
};
constexpr SmoothingChoice kSmoothing[] = {
    { "auto", "Automatic" },
    { "rgb", "Subpixel (RGB)" },
    { "bgr", "Subpixel (BGR)" },
    { "gray", "Greyscale" },
};
constexpr int kSmoothingCount = static_cast<int>(sizeof(kSmoothing) / sizeof(kSmoothing[0]));

// A dim line under a row, for what the row above does or when it applies.
void Note(const char* text)
{
    BeginPropertyRow("");
    ImGui::PushStyleColor(ImGuiCol_Text, Palette::Dim);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}
}  // namespace

void EditorSettingsPanel::Draw()
{
    if (!m_Open)
    {
        return;
    }

    DekiEditor::FirstUseWindowPlacement(440, 470);
    if (!ImGui::Begin("EDITOR SETTINGS###Editor Settings", &m_Open,
                      DekiEditor::MaximizedWindowFlags("EDITOR SETTINGS###Editor Settings")))
    {
        ImGui::End();
        return;
    }

    if (SchematicSectionBegin("Interface"))
    {
        BeginPropertyContext();
        InspectorUtils::SetNextRowTooltip("Popups, panels, scrolling, zoom and presses land at once instead of "
                                          "moving.");
        BeginPropertyRow("Reduce Motion");
        bool reduce = Motion::ReduceMotion();
        if (SchematicCheckbox("##reducemotion", &reduce))
        {
            Motion::SetReduceMotion(reduce);
        }
        EndPropertyContext();
        SchematicSectionEnd();
    }
    ImGui::Spacing();

    if (SchematicSectionBegin("Text"))
    {
        BeginPropertyContext();
        const std::string stored = EditorSettings::GetString(kTextSmoothingKey, "auto");
        int current = 0;
        for (int i = 0; i < kSmoothingCount; ++i)
        {
            if (stored == kSmoothing[i].value)
            {
                current = i;
            }
        }
        const char* labels[kSmoothingCount];
        for (int i = 0; i < kSmoothingCount; ++i)
        {
            labels[i] = kSmoothing[i].label;
        }
        InspectorUtils::SetNextRowTooltip("Automatic follows the system: subpixel when ClearType is on, in your "
                                          "screen's stripe order, greyscale otherwise.");
        BeginPropertyRow("Smoothing");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (SchematicCombo("##textsmoothing", &current, labels, kSmoothingCount))
        {
            EditorSettings::SetString(kTextSmoothingKey, kSmoothing[current].value);
        }
        Note("Applies the next time the editor starts.");
        EndPropertyContext();
        SchematicSectionEnd();
    }
    ImGui::Spacing();

    if (SchematicSectionBegin("Updates"))
    {
        BeginPropertyContext();
        InspectorUtils::SetNextRowTooltip("Once a day, ask GitHub whether a newer editor is out. It only tells "
                                          "you; nothing is downloaded.");
        BeginPropertyRow("Check Daily");
        bool check = UpdateCheck::IsEnabled();
        if (SchematicCheckbox("##checkdaily", &check))
        {
            UpdateCheck::SetEnabled(check);
        }
        EndPropertyContext();
        SchematicSectionEnd();
    }
    ImGui::Spacing();

    if (SchematicSectionBegin("Editor Log"))
    {
        BeginPropertyContext();
        InspectorUtils::SetNextRowTooltip("Editor.log also keeps per-item detail: each asset imported, each "
                                          "component and editor registered. For tracking down a problem.");
        BeginPropertyRow("Detailed");
        bool verbose = EditorSettings::GetBool("editorLog.verbose", false);
        if (SchematicCheckbox("##logverbose", &verbose))
        {
            EditorSettings::SetBool("editorLog.verbose", verbose);
            EditorLogFile::SetVerbose(verbose);
        }
        BeginPropertyRow("File");
        ImGui::PushStyleColor(ImGuiCol_Text, Palette::Dim);
        ImGui::TextWrapped("%s", EditorLogFile::GetLogPath().c_str());
        ImGui::PopStyleColor();
        EndPropertyContext();
        SchematicSectionEnd();
    }

    ImGui::End();
}

}  // namespace DekiEditor
