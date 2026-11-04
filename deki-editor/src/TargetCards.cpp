#include <deki-editor/TargetCards.h>

#include <deki-editor/EditorTheme.h>
#include <deki-editor/IconsTabler.h>
#include <deki-editor/Localization.h>
#include <deki-editor/build/PlatformConfig.h>
#include "imgui.h"
#include "imgui_internal.h"  // ImGuiSelectableFlags_SpanAvailWidth, as SchematicCombo uses

#include <algorithm>

namespace DekiEditor
{

namespace
{

float Dpi(float logicalPixels)
{
    return logicalPixels * ImGui::GetWindowDpiScale();
}

}  // namespace

std::vector<const PlatformConfig*> ProjectTargets(const std::string& projectPath)
{
    // The registry holds this project's platforms: ReloadProjectPlatforms
    // fills it when the project opens and when its packages change.
    (void)projectPath;
    std::vector<const PlatformConfig*> out;
    for (const auto& [id, name] : PlatformRegistry::Instance().GetPlatformDisplayList())
    {
        if (const PlatformConfig* p = PlatformRegistry::Instance().GetPlatform(id))
        {
            out.push_back(p);
        }
    }
    return out;
}

bool TargetCardsBegin()
{
    FullBleedSeparator();
    if (!SchematicSectionBegin("Targets"))
    {
        return false;
    }
    ImGui::Spacing();
    return true;
}

void TargetCardsEnd()
{
    SchematicSectionEnd();
}

void TargetCard(const TargetCardInfo& info, bool& open,
                const std::function<void(float labelX, float valueX, float valueW)>& body)
{
    const float dpi = Dpi(1.0f);
    const float cardGap = 8.0f * dpi;
    const float pad = 10.0f * dpi;
    const float rounding = 3.0f * dpi;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImGui::PushID(info.id);
    const float left = ImGui::GetCursorScreenPos().x;
    const float right = left + ImGui::GetContentRegionAvail().x - Dpi(Metrics::kInspectorRightPad);
    const float innerL = left + pad, innerR = right - pad;
    const ImVec2 top = ImGui::GetCursorScreenPos();

    ImDrawListSplitter split;
    split.Split(dl, 2);
    split.SetCurrentChannel(dl, 1);

    // Header: chevron, dot, name, and the note on the right. The whole header
    // is the fold toggle.
    const float headerH = ImGui::GetFrameHeight();
    ImGui::SetCursorScreenPos(ImVec2(left, top.y + pad * 0.5f));
    if (ImGui::InvisibleButton("##header", ImVec2(right - left, headerH)))
    {
        open = !open;
    }
    const bool headerHovered = ImGui::IsItemHovered();
    const float textY = top.y + pad * 0.5f + (headerH - ImGui::GetTextLineHeight()) * 0.5f;
    dl->AddText(ImVec2(innerL, textY), ImGui::GetColorU32(Palette::Dim),
                open ? ICON_TI_CHEVRON_DOWN : ICON_TI_CHEVRON_RIGHT);
    float x = innerL + ImGui::CalcTextSize(ICON_TI_CHEVRON_DOWN).x + 6.0f * dpi;
    if (info.overridden)
    {
        dl->AddText(ImVec2(x, textY), ImGui::GetColorU32(Palette::Accent), ICON_TI_POINT_FILLED);
        x += ImGui::CalcTextSize(ICON_TI_POINT_FILLED).x + 3.0f * dpi;
    }
    const float noteW = info.note ? ImGui::CalcTextSize(info.note).x : 0.0f;
    const float nameRoom = innerR - x - (info.note ? noteW + 8.0f * dpi : 0.0f);
    std::string shown = info.name;
    if (ImGui::CalcTextSize(shown.c_str()).x > nameRoom)
    {
        while (!shown.empty() && ImGui::CalcTextSize((shown + "...").c_str()).x > nameRoom)
        {
            shown.pop_back();
        }
        shown += "...";
    }
    dl->AddText(ImVec2(x, textY), ImGui::GetColorU32(info.dimName ? Palette::Dim : Palette::Fg), shown.c_str());
    if (info.note)
    {
        dl->AddText(ImVec2(innerR - noteW, textY), ImGui::GetColorU32(Palette::Dim), info.note);
    }
    if (headerHovered)
    {
        const float mx = ImGui::GetIO().MousePos.x;
        if (info.note && info.noteTip && mx >= innerR - noteW)
        {
            Tooltip("%s", info.noteTip);
        }
        else if (info.nameTip)
        {
            Tooltip("%s", info.nameTip);
        }
    }

    const float labelX = innerL + ImGui::CalcTextSize(ICON_TI_CHEVRON_DOWN).x + 6.0f * dpi;
    if (!open)
    {
        // Folded: one dim line saying what the target stores.
        if (!info.summary.empty())
        {
            ImGui::SetCursorScreenPos(ImVec2(labelX, ImGui::GetCursorScreenPos().y));
            ImGui::TextDisabled("%s", info.summary.c_str());
        }
    }
    else
    {
        // Controls line up with the inspector's value column (the other
        // sections' controls), never closer than a label's width.
        const float sectionLabelX = SectionAlignedCursorX();
        const float columnX =
            ImGui::GetWindowPos().x + sectionLabelX + PropertyLabelWidth(sectionLabelX);
        const float valueX = std::max(labelX + 70.0f * dpi, columnX);
        body(labelX, valueX, std::max(40.0f * dpi, innerR - valueX));
    }
    ImGui::SetCursorScreenPos(ImVec2(left, ImGui::GetCursorScreenPos().y));
    ImGui::Dummy(ImVec2(0.0f, pad * 0.5f));
    const float bottom = ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y;

    // The card behind it, now that its height is known.
    split.SetCurrentChannel(dl, 0);
    dl->AddRectFilled(top, ImVec2(right, bottom), ImGui::GetColorU32(headerHovered ? Palette::Bg2 : Palette::Panel),
                      rounding);
    dl->AddRect(top, ImVec2(right, bottom), ImGui::GetColorU32(Palette::Line2), rounding);
    split.Merge(dl);

    ImGui::SetCursorScreenPos(ImVec2(left, bottom + cardGap));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    ImGui::PopID();
}

void TargetCardRow(const char* label, float labelX, float valueX)
{
    ImGui::SetCursorScreenPos(ImVec2(labelX, ImGui::GetCursorScreenPos().y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", Tr(label));
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::SetCursorScreenPos(ImVec2(valueX, ImGui::GetCursorScreenPos().y));
}

bool TargetOverrideCombo(const char* id, float width, const char* preview, bool fromDefault,
                         const std::string& defaultLabel, const std::function<const char*(int)>& label, int count,
                         int firstIndex, int& index)
{
    ImGui::SetNextItemWidth(width);
    if (fromDefault)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, Palette::Dim);
    }
    const bool open = SchematicBeginCombo(id, preview);
    if (fromDefault)
    {
        ImGui::PopStyleColor();
    }
    bool changed = false;
    if (open)
    {
        if (ImGui::Selectable(defaultLabel.c_str(), fromDefault, ImGuiSelectableFlags_SpanAvailWidth) && !fromDefault)
        {
            index = firstIndex - 1;
            changed = true;
        }
        for (int i = firstIndex; i < count; ++i)
        {
            const bool selected = !fromDefault && i == index;
            if (ImGui::Selectable(Tr(label(i)), selected, ImGuiSelectableFlags_SpanAvailWidth) && !selected)
            {
                index = i;
                changed = true;
            }
            if (selected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }
        SchematicEndCombo();
    }
    return changed;
}

}  // namespace DekiEditor
