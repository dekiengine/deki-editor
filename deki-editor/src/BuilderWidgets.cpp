#include <deki-editor/build/BuilderWidgets.h>

#include "imgui.h"

#include <cstring>

namespace DekiEditor
{

void DrawStringListEditor(const char* label, std::vector<std::string>& list,
                          char* newItemBuffer, size_t bufSize)
{
    ImGui::Text("%s", label);

    ImGui::PushID(label);

    int removeIndex = -1;
    for (size_t i = 0; i < list.size(); i++)
    {
        ImGui::PushID(static_cast<int>(i));

        ImGui::SetNextItemWidth(-60);
        char buffer[256];
        strncpy(buffer, list[i].c_str(), sizeof(buffer) - 1);
        buffer[sizeof(buffer) - 1] = '\0';

        if (ImGui::InputText("##item", buffer, sizeof(buffer)))
        {
            list[i] = buffer;
        }

        ImGui::SameLine();
        if (ImGui::Button("X", ImVec2(25, 0)))
        {
            removeIndex = static_cast<int>(i);
        }

        ImGui::PopID();
    }

    if (removeIndex >= 0)
    {
        list.erase(list.begin() + removeIndex);
    }

    ImGui::SetNextItemWidth(-60);
    if (ImGui::InputText("##new", newItemBuffer, bufSize, ImGuiInputTextFlags_EnterReturnsTrue))
    {
        if (strlen(newItemBuffer) > 0)
        {
            list.push_back(newItemBuffer);
            newItemBuffer[0] = '\0';
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("+", ImVec2(25, 0)))
    {
        if (strlen(newItemBuffer) > 0)
        {
            list.push_back(newItemBuffer);
            newItemBuffer[0] = '\0';
        }
    }

    ImGui::PopID();
}

}  // namespace DekiEditor
