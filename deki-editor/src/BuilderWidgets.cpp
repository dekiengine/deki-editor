#include <deki-editor/build/BuilderWidgets.h>
#include <deki-editor/EditorTheme.h>
#include <deki-editor/IconsTabler.h>

#include "imgui.h"

#include <cstring>

namespace DekiEditor
{

namespace
{
// A square, frameless icon button the height of a field, so it lines up with
// the text box beside it at any DPI.
bool ListIconButton(const char* icon, const char* tooltip)
{
    const float side = ImGui::GetFrameHeight();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, Palette::Dim);
    const bool clicked = ImGui::Button(icon, ImVec2(side, side));
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tooltip);
    return clicked;
}
}  // namespace

void DrawStringListEditor(const char* label, std::vector<std::string>& list,
                          char* newItemBuffer, size_t bufSize)
{
    SchematicSubHeader(label);

    ImGui::PushID(label);
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float fieldW = ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - gap;

    int removeIndex = -1;
    for (size_t i = 0; i < list.size(); i++)
    {
        ImGui::PushID(static_cast<int>(i));

        ImGui::SetNextItemWidth(fieldW);
        char buffer[256];
        strncpy(buffer, list[i].c_str(), sizeof(buffer) - 1);
        buffer[sizeof(buffer) - 1] = '\0';

        if (ImGui::InputText("##item", buffer, sizeof(buffer)))
        {
            list[i] = buffer;
        }

        ImGui::SameLine(0.0f, gap);
        if (ListIconButton(ICON_TI_X, "Remove"))
        {
            removeIndex = static_cast<int>(i);
        }

        ImGui::PopID();
    }

    if (removeIndex >= 0)
    {
        list.erase(list.begin() + removeIndex);
    }

    ImGui::SetNextItemWidth(fieldW);
    bool add = ImGui::InputTextWithHint("##new", "Add...", newItemBuffer, bufSize, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine(0.0f, gap);
    add |= ListIconButton(ICON_TI_PLUS, "Add");
    if (add && strlen(newItemBuffer) > 0)
    {
        list.push_back(newItemBuffer);
        newItemBuffer[0] = '\0';
    }

    ImGui::PopID();
}

}  // namespace DekiEditor
