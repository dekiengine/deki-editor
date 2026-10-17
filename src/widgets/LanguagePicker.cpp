#include "LanguagePicker.h"

#include <deki-editor/EditorTheme.h>
#include <deki-editor/Localization.h>

#include "imgui.h"
#include "imgui_internal.h"  // ImGuiSelectableFlags_SpanAvailWidth

#include <string>

namespace DekiEditor
{

void DrawLanguagePicker(const char* id)
{
    const std::string current = Localization::Current();
    std::string preview = current;
    for (const Localization::Language& language : Localization::Languages())
    {
        if (language.code == current)
        {
            preview = language.name;
        }
    }

    if (!SchematicBeginCombo(id, preview.c_str()))
    {
        return;
    }
    for (const Localization::Language& language : Localization::Languages())
    {
        const bool selected = language.code == current;
        if (ImGui::Selectable((language.name + "##" + language.code).c_str(), selected,
                              ImGuiSelectableFlags_SpanAvailWidth) &&
            !selected)
        {
            Localization::SetLanguage(language.code);
        }
        if (selected)
        {
            ImGui::SetItemDefaultFocus();
        }
    }
    SchematicEndCombo();
}

}  // namespace DekiEditor
