// Inspector for Screen Anchor: the anchor as a 3x3 grid of the screen's
// points, then the offset.

#ifdef DEKI_EDITOR

#include <deki-editor/CommandHistory.h>
#include <deki-editor/CustomEditor.h>
#include <deki-editor/EditorRegistry.h>
#include <deki-editor/EditorTheme.h>
#include <deki-editor/EditorUI.h>
#include <deki-editor/SceneView.h>
#include <deki-editor/Localization.h>
#include <deki/ScreenAnchor.h>
#include <deki/reflection/ComponentRegistry.h>

#include "../core/commands/PropertyCommands.h"

#include "imgui.h"

#include <cstring>
#include <memory>

namespace DekiEditor
{
namespace
{

const char* const kPointNames[9] = { TrMark("Top Left"),    TrMark("Top"),    TrMark("Top Right"),
                                     TrMark("Left"),        TrMark("Center"), TrMark("Right"),
                                     TrMark("Bottom Left"), TrMark("Bottom"), TrMark("Bottom Right") };

const Deki::PropertyInfo* FindProperty(Deki::Component* comp, const char* name)
{
    const Deki::ComponentMeta* meta = Deki::ComponentRegistry::Instance().GetMeta(comp->GetType());
    if (!meta)
    {
        return nullptr;
    }
    for (int i = 0; i < meta->propertyCount; ++i)
    {
        if (std::strcmp(meta->properties[i].name, name) == 0)
        {
            return &meta->properties[i];
        }
    }
    return nullptr;
}

// Draws the 3x3 grid and returns the point clicked, or -1.
int PointGrid(int selected)
{
    const float cell = ImGui::GetFontSize();
    const float gap = ImGui::GetStyle().ItemInnerSpacing.x * 0.5f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    int clicked = -1;

    for (int i = 0; i < 9; ++i)
    {
        const ImVec2 min(origin.x + (i % 3) * (cell + gap), origin.y + (i / 3) * (cell + gap));
        const ImVec2 max(min.x + cell, min.y + cell);
        ImGui::SetCursorScreenPos(min);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##point", ImVec2(cell, cell)))
        {
            clicked = i;
        }
        const bool hovered = ImGui::IsItemHovered();
        if (hovered)
        {
            DekiEditor::Tooltip("%s", Tr(kPointNames[i]));
        }
        ImGui::PopID();

        draw->AddRect(min, max, ImGui::GetColorU32(hovered ? Palette::Line3 : Palette::Line2));
        if (i == selected)
        {
            const float inset = cell * 0.25f;
            draw->AddRectFilled(ImVec2(min.x + inset, min.y + inset), ImVec2(max.x - inset, max.y - inset),
                                ImGui::GetColorU32(Palette::Accent));
        }
    }

    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + 3 * cell + 2 * gap));
    ImGui::Dummy(ImVec2(3 * cell + 2 * gap, 0.0f));
    return clicked;
}

class ScreenAnchorEditor : public CustomEditor
{
public:
    const char* GetComponentName() const override { return "ScreenAnchor"; }

    bool WantsInspectorOverride(Deki::Component*) override { return true; }

    void OnInspectorGUI(Deki::Component* comp) override
    {
        auto* anchor = static_cast<Deki::ScreenAnchor*>(comp);
        auto& ui = EditorUI::Get();

        ui.PropertyRow("Anchor");
        const int clicked = PointGrid(static_cast<int>(anchor->anchor));
        if (clicked >= 0 && clicked != static_cast<int>(anchor->anchor))
        {
            if (const Deki::PropertyInfo* prop = FindProperty(comp, "anchor"))
            {
                CommandHistory::Instance().Execute(
                    std::make_unique<ModifyComponentPropertyCommand>(comp, *prop, nlohmann::json(clicked)));
            }
        }

        ui.PropertyField("offsetX");
        ui.PropertyField("offsetY");
        ui.PropertyField("update");
    }

    // A line from the anchor's point on the screen to the object, ending in a
    // small square at the point.
    void OnDrawGizmosSelected(Deki::Component* comp) override
    {
        auto* anchor = static_cast<Deki::ScreenAnchor*>(comp);
        SceneView& view = SceneView::Get();
        const float scale = view.GetWorldToScreenScale();
        const float pointX = view.GetScreenX() - anchor->offsetX * scale;
        const float pointY = view.GetScreenY() + anchor->offsetY * scale;
        const uint32_t color = SceneView::Rgba(58, 194, 255, 200);
        const float half = 3.0f * view.GetZoom();

        view.DrawLine(pointX, pointY, view.GetScreenX(), view.GetScreenY(), color);
        view.DrawRectFilled(pointX - half, pointY - half, pointX + half, pointY + half, color);
    }
};

}  // namespace

REGISTER_EDITOR(ScreenAnchorEditor)

}  // namespace DekiEditor

#endif  // DEKI_EDITOR
