#include "ProfilerPanel.h"
#include "EditorApp.h"
#include "MemoryPanel.h"
#include <deki-editor/EditorUI.h>
#include <deki-editor/Localization.h>
#include "imgui.h"

namespace DekiEditor
{

// Memory has a panel of its own, with what each part of the game holds; this
// tab points there rather than keeping a second, smaller view of it.
void ProfilerPanel::DrawMemoryView()
{
    auto& ui = EditorUI::Get();
    ui.Spacing();
    ui.Hint(Tr("The Memory panel shows what a running game holds, by what holds it, with peaks, and what the open "
               "scene would take on a board."));
    ui.Spacing();
    if (ui.ActionButton(Tr("Open Memory")))
    {
        if (MemoryPanel* memory = m_App->GetMemoryPanel())
        {
            memory->SetOpen(true);
            memory->ShowLive();
            ImGui::SetWindowFocus("MEMORY###Memory");
        }
    }
}

}  // namespace DekiEditor
