// PropertyEditorFactory: array properties (DrawArrayProperty and
// DrawObjectRefArrayPicker).

#include "PropertyEditorFactory.h"
#include <deki-editor/EditorTheme.h>
#include "InspectorUtils.h"
#include "../../EditorApp.h"
#include <deki-editor/CommandHistory.h>
#include <deki-editor/Localization.h>
#include "../../core/Commands.h"
#include "../../core/commands/PropertyTypeHandlers.h"
#include <deki/reflection/Property.h>
#include <deki/reflection/ComponentRegistry.h>
#include <deki/Component.h>
#include <deki/Object.h>
#include <deki/Scene.h>
#include "imgui.h"
#include <nlohmann/json.hpp>
#include <cstring>
#include <functional>

namespace DekiEditor
{

using InspectorUtils::ComponentIsOrDerivesFrom;

void PropertyEditorFactory::DrawArrayProperty(Deki::Component* comp, const Deki::PropertyInfo& prop,
                                              const char* displayName)
{
    nlohmann::json currentValue = CaptureArray(comp, prop);

    // --- Header: a property row whose value is the size, then + Add ---
    // BeginPropertyRow takes the tooltip DrawProperty set for this row.
    const char* addLabel = TrLabel("+ Add");
    const ImGuiStyle& style = ImGui::GetStyle();
    InspectorUtils::BeginPropertyRow(displayName);
    const float addW = ImGui::CalcTextSize(addLabel, nullptr, true).x + style.FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(ImGui::CalcItemWidth() - addW - style.ItemSpacing.x);
    int arraySize = static_cast<int>(currentValue.size());
    if (DekiEditor::SchematicDragInt("##arraysize", &arraySize, 0.1f, 0, 9999))
    {
        if (arraySize >= 0 && arraySize != static_cast<int>(currentValue.size()))
        {
            nlohmann::json newValue = currentValue;
            while (static_cast<int>(newValue.size()) < arraySize)
            {
                switch (prop.elementType)
                {
                    case Deki::PropertyType::String: newValue.push_back(""); break;
                    case Deki::PropertyType::Int32:
                    case Deki::PropertyType::Int16:
                    case Deki::PropertyType::Int8:
                    case Deki::PropertyType::UInt32:
                    case Deki::PropertyType::UInt16:
                    case Deki::PropertyType::UInt8: newValue.push_back(0); break;
                    case Deki::PropertyType::Float: newValue.push_back(0.0f); break;
                    case Deki::PropertyType::Bool: newValue.push_back(false); break;
                    case Deki::PropertyType::ObjectRef: newValue.push_back(0); break;
                    default: newValue.push_back(nullptr); break;
                }
            }
            while (static_cast<int>(newValue.size()) > arraySize)
            {
                newValue.erase(newValue.size() - 1);
            }
            CommandHistory::Instance().ExecuteNoMerge(
                std::make_unique<ModifyComponentPropertyCommand>(comp, prop, newValue));
        }
    }

    // Undo for the size drag.
    static nlohmann::json s_OldSizeValue;
    if (ImGui::IsItemActivated())
    {
        s_OldSizeValue = currentValue;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        nlohmann::json nowValue = CaptureArray(comp, prop);
        if (nowValue != s_OldSizeValue)
        {
            ApplyArray(comp, prop, s_OldSizeValue);
            CommandHistory::Instance().ExecuteNoMerge(
                std::make_unique<ModifyComponentPropertyCommand>(comp, prop, nowValue));
        }
    }

    ImGui::SameLine();
    if (ImGui::Button(addLabel, ImVec2(addW, 0.0f)))
    {
        nlohmann::json newValue = currentValue;
        switch (prop.elementType)
        {
            case Deki::PropertyType::String: newValue.push_back(""); break;
            case Deki::PropertyType::Int32:
            case Deki::PropertyType::Int16:
            case Deki::PropertyType::Int8:
            case Deki::PropertyType::UInt32:
            case Deki::PropertyType::UInt16:
            case Deki::PropertyType::UInt8: newValue.push_back(0); break;
            case Deki::PropertyType::Float: newValue.push_back(0.0f); break;
            case Deki::PropertyType::Bool: newValue.push_back(false); break;
            case Deki::PropertyType::ObjectRef: newValue.push_back(0); break;
            default: break;
        }
        CommandHistory::Instance().ExecuteNoMerge(
            std::make_unique<ModifyComponentPropertyCommand>(comp, prop, newValue));
    }

    // The cards and the empty note sit in the rows' column: from the label's
    // left edge to the value controls' right edge.
    const float dpi = style.FontScaleDpi;
    const float columnX = InspectorUtils::SectionAlignedCursorX();
    const float columnW = ImGui::GetContentRegionMax().x - columnX - ::DekiEditor::Metrics::kInspectorRightPad * dpi;

    if (currentValue.empty())
    {
        ImGui::SetCursorPosX(columnX);
        ImGui::PushTextWrapPos(columnX + columnW);
        ImGui::PushStyleColor(ImGuiCol_Text, Palette::Dim);
        ImGui::TextUnformatted(Tr("Empty. Use + Add or set the size."));
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
    }

    int indexToRemove = -1;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    auto withAlpha = [](ImVec4 c, float a)
    {
        c.w = a;
        return ImGui::ColorConvertFloat4ToU32(c);
    };
    const float padY = 5.0f;
    const float rounding = style.FrameRounding;
    const float accentWidth = 3.0f;
    const ImU32 cardColor = ImGui::ColorConvertFloat4ToU32(Palette::Bg3);
    const ImU32 cardHoverColor = ImGui::ColorConvertFloat4ToU32(Palette::Bg2);
    const ImU32 cardBorderColor = ImGui::ColorConvertFloat4ToU32(Palette::Line2);
    const ImU32 accentColor = withAlpha(Palette::Accent, 0.30f);
    const ImU32 accentHoverColor = withAlpha(Palette::Accent, 0.55f);
    const ImU32 handleColor = ImGui::ColorConvertFloat4ToU32(Palette::Dim2);
    const ImU32 handleHoverColor = ImGui::ColorConvertFloat4ToU32(Palette::Fg);
    const ImU32 indexColor = ImGui::ColorConvertFloat4ToU32(Palette::Dim);
    const ImU32 removeColor = ImGui::ColorConvertFloat4ToU32(Palette::Dim2);
    const ImU32 removeHoverBg = withAlpha(Palette::Red, 0.18f);
    const ImU32 removeHoverColor = ImGui::ColorConvertFloat4ToU32(Palette::Red);
    const float handleWidth = 26.0f;
    const float indexWidth = 22.0f;
    const float removeWidth = 48.0f;
    const float cardSpacing = 1.0f;

    static int s_DragSourceIndex = -1;
    static float s_DragGrabOffsetY = 0.0f;
    static nlohmann::json s_DragOriginalArray;

    float cardHeight = ImGui::GetFrameHeight() + padY * 2;
    float arrayTopY = ImGui::GetCursorScreenPos().y;

    drawList->ChannelsSplit(2);

    for (size_t i = 0; i < currentValue.size(); i++)
    {
        ImGui::PushID(static_cast<int>(i));

        // --- Card dimensions ---
        ImGui::SetCursorPosX(columnX);
        ImVec2 cardStart = ImGui::GetCursorScreenPos();
        float availWidth = columnW;
        ImVec2 cardMin(cardStart.x, cardStart.y);
        ImVec2 cardMax(cardStart.x + availWidth, cardStart.y + cardHeight);

        bool isDragSource = (s_DragSourceIndex == static_cast<int>(i));

        // Positions before any drag offset.
        float originalCardMinY = cardMin.y;
        float originalCardMaxY = cardMax.y;

        // The dragged card follows the mouse vertically, within the array.
        if (isDragSource)
        {
            float mouseY = ImGui::GetMousePos().y;
            float floatingY = mouseY - s_DragGrabOffsetY;

            int n = static_cast<int>(currentValue.size());
            float minY = arrayTopY;
            float maxY = arrayTopY + (n - 1) * (cardHeight + cardSpacing);
            if (floatingY < minY)
            {
                floatingY = minY;
            }
            if (floatingY > maxY)
            {
                floatingY = maxY;
            }

            float dragOffsetY = floatingY - cardMin.y;

            // A faint placeholder where the card was.
            drawList->ChannelsSetCurrent(0);
            drawList->AddRectFilled(cardMin, cardMax, withAlpha(Palette::Bg2, 0.5f), rounding);
            drawList->AddRect(cardMin, cardMax, withAlpha(Palette::Line2, 0.5f), rounding, 0, 1.0f);

            cardMin.y += dragOffsetY;
            cardMax.y += dragOffsetY;

            // The floating card draws on the top channel.
            drawList->ChannelsSetCurrent(1);
        }
        else
        {
            drawList->ChannelsSetCurrent(0);
        }

        bool cardHovered = ImGui::IsMouseHoveringRect(cardMin, cardMax);
        bool showHover = (s_DragSourceIndex < 0) && cardHovered;
        ImU32 bgColor = isDragSource ? cardHoverColor : (showHover ? cardHoverColor : cardColor);
        ImU32 borderCol = isDragSource ? ImGui::ColorConvertFloat4ToU32(Palette::AccentDim) : cardBorderColor;

        if (isDragSource)
        {
            drawList->AddRectFilled(ImVec2(cardMin.x + 4, cardMin.y + 4), ImVec2(cardMax.x + 4, cardMax.y + 4),
                                    IM_COL32(0, 0, 0, 50), rounding);
        }
        drawList->AddRectFilled(cardMin, cardMax, bgColor, rounding);
        drawList->AddRect(cardMin, cardMax, borderCol, rounding);

        // Left accent stripe
        ImU32 accent = isDragSource ? ImGui::ColorConvertFloat4ToU32(Palette::Accent)
                                    : (showHover ? accentHoverColor : accentColor);
        drawList->AddRectFilled(ImVec2(cardMin.x, cardMin.y + 1), ImVec2(cardMin.x + accentWidth, cardMax.y - 1),
                                accent, rounding, ImDrawFlags_RoundCornersLeft);

        // --- Drag handle (left, after accent stripe) ---
        float handleX = cardMin.x + accentWidth;
        ImGui::SetCursorScreenPos(ImVec2(handleX, cardMin.y));
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##drag", ImVec2(handleWidth, cardHeight));
        bool handleHovered = ImGui::IsItemHovered();

        if (handleHovered && s_DragSourceIndex < 0)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }

        // Three horizontal lines as the grip.
        {
            float cx = handleX + handleWidth * 0.5f;
            float cy = cardMin.y + cardHeight * 0.5f;
            ImU32 gripColor = handleHovered ? handleHoverColor : handleColor;
            float lineHalfW = 6.0f;
            for (int line = -1; line <= 1; line++)
            {
                float ly = cy + line * 4.5f;
                drawList->AddLine(ImVec2(cx - lineHalfW, ly), ImVec2(cx + lineHalfW, ly), gripColor, 1.5f);
            }
        }

        if (s_DragSourceIndex < 0 && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f))
        {
            s_DragSourceIndex = static_cast<int>(i);
            s_DragGrabOffsetY = ImGui::GetMousePos().y - originalCardMinY;
            s_DragOriginalArray = currentValue;
        }

        // --- Index label ---
        {
            char indexBuf[8];
            snprintf(indexBuf, sizeof(indexBuf), "%d", static_cast<int>(i));
            ImVec2 idxSize = ImGui::CalcTextSize(indexBuf);
            float idxX = handleX + handleWidth;
            float idxY = cardMin.y + (cardHeight - idxSize.y) * 0.5f;
            drawList->AddText(ImVec2(idxX + (indexWidth - idxSize.x) * 0.5f, idxY), indexColor, indexBuf);
        }

        // --- Element widget (center) ---
        float widgetX = handleX + handleWidth + indexWidth;
        float widgetWidth = availWidth - accentWidth - handleWidth - indexWidth - removeWidth - 2.0f;
        ImGui::SetCursorScreenPos(ImVec2(widgetX, cardMin.y + padY));
        ImGui::SetNextItemWidth(widgetWidth);

        bool changed = false;
        nlohmann::json newValue = currentValue;

        switch (prop.elementType)
        {
            case Deki::PropertyType::String:
            {
                char buf[256];
                std::string str = currentValue[i].get<std::string>();
                strncpy(buf, str.c_str(), sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = '\0';
                if (ImGui::InputText("##item", buf, sizeof(buf)))
                {
                    newValue[i] = std::string(buf);
                    changed = true;
                }
                break;
            }
            case Deki::PropertyType::Int32:
            case Deki::PropertyType::Int16:
            case Deki::PropertyType::Int8:
            case Deki::PropertyType::UInt32:
            case Deki::PropertyType::UInt16:
            case Deki::PropertyType::UInt8:
            {
                int val = currentValue[i].get<int>();
                if (DekiEditor::SchematicDragInt("##item", &val))
                {
                    newValue[i] = val;
                    changed = true;
                }
                break;
            }
            case Deki::PropertyType::Float:
            {
                float val = currentValue[i].get<float>();
                if (DekiEditor::SchematicDragFloat("##item", &val, 0.1f))
                {
                    newValue[i] = val;
                    changed = true;
                }
                break;
            }
            case Deki::PropertyType::Bool:
            {
                bool val = currentValue[i].get<bool>();
                if (DekiEditor::SchematicCheckbox("##item", &val))
                {
                    newValue[i] = val;
                    changed = true;
                }
                break;
            }
            case Deki::PropertyType::ObjectRef:
            {
                uint32_t objId = 0;
                uint32_t typeId = 0;
                if (currentValue[i].is_object())
                {
                    objId = currentValue[i].value("id", 0u);
                    typeId = currentValue[i].value("typeId", 0u);
                }
                else
                {
                    objId = currentValue[i].get<uint32_t>();
                }
                Deki::Object* refObj = nullptr;
                ::Deki::Scene* scene = m_App->GetCurrentScene();
                if (scene && objId != 0)
                {
                    refObj = scene->FindDekiObject(objId);
                }
                std::string btnLabel = Tr("(None)");
                if (refObj)
                {
                    btnLabel = refObj->GetName();
                    if (typeId != 0)
                    {
                        auto& reg = Deki::ComponentRegistry::Instance();
                        const auto* meta = reg.GetMeta(typeId);
                        if (meta && meta->name)
                        {
                            btnLabel += " - " + std::string(meta->name);
                        }
                    }
                    else if (prop.componentRefType)
                    {
                        std::string compName = InspectorUtils::GetMatchingComponentName(refObj, prop.componentRefType);
                        if (!compName.empty())
                        {
                            btnLabel += " - " + compName;
                        }
                    }
                }
                btnLabel += "##objref" + std::to_string(i);
                // FrameBg colours, like the other reference fields.
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_FrameBg]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyle().Colors[ImGuiCol_FrameBgHovered]);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyle().Colors[ImGuiCol_FrameBgActive]);
                if (ImGui::Button(btnLabel.c_str(), ImVec2(widgetWidth, 0)))
                {
                    m_ObjectRefPickerPosX = ImGui::GetWindowPos().x;
                    m_ObjectRefPickerPosY = ImGui::GetItemRectMax().y;
                    m_ObjectRefPickerWidth = ImGui::GetWindowSize().x;
                    OpenObjectRefPicker(comp, &prop, static_cast<int>(i));
                }
                ImGui::PopStyleColor(3);
                break;
            }
            default: ImGui::TextDisabled("%s", Tr("(unsupported element type)")); break;
        }

        if (changed)
        {
            ApplyArray(comp, prop, newValue);
        }

        // Undo command when the edit finishes.
        static nlohmann::json s_OldArrayValue;
        if (ImGui::IsItemActivated())
        {
            s_OldArrayValue = currentValue;
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && newValue != s_OldArrayValue)
        {
            ApplyArray(comp, prop, s_OldArrayValue);
            CommandHistory::Instance().Execute(std::make_unique<ModifyComponentPropertyCommand>(comp, prop, newValue));
        }

        // --- Remove button (right) ---
        float removeBtnX = cardMax.x - removeWidth;
        ImGui::SetCursorScreenPos(ImVec2(removeBtnX, cardMin.y));
        ImGui::InvisibleButton("##remove", ImVec2(removeWidth, cardHeight));
        bool rmHovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked())
        {
            indexToRemove = static_cast<int>(i);
        }
        {
            float insetX = 3.0f;
            float insetY = 4.0f;
            float insetR = 5.0f;
            ImVec2 hMin(removeBtnX + insetX, cardMin.y + insetY);
            ImVec2 hMax(cardMax.x - insetR, cardMax.y - insetY);
            if (rmHovered)
            {
                drawList->AddRectFilled(hMin, hMax, removeHoverBg, rounding);
            }
            float cx = (hMin.x + hMax.x) * 0.5f;
            float cy = (hMin.y + hMax.y) * 0.5f;
            float half = 5.0f;
            ImU32 xColor = rmHovered ? removeHoverColor : removeColor;
            drawList->AddLine(ImVec2(cx - half, cy - half), ImVec2(cx + half, cy + half), xColor, 2.0f);
            drawList->AddLine(ImVec2(cx + half, cy - half), ImVec2(cx - half, cy + half), xColor, 2.0f);
        }

        // Past the card's original position, not the dragged one.
        ImGui::SetCursorScreenPos(ImVec2(cardStart.x, originalCardMaxY + cardSpacing));

        ImGui::PopID();
    }

    drawList->ChannelsMerge();

    // Real-time reorder during drag
    if (s_DragSourceIndex >= 0)
    {
        float mouseY = ImGui::GetMousePos().y;
        float floatingY = mouseY - s_DragGrabOffsetY;
        float floatingCenterY = floatingY + cardHeight * 0.5f;
        int n = static_cast<int>(currentValue.size());

        // Swap when the floating card's centre passes a neighbour's centre.
        if (s_DragSourceIndex > 0)
        {
            float aboveCenter = arrayTopY + (s_DragSourceIndex - 1) * (cardHeight + cardSpacing) + cardHeight * 0.5f;
            if (floatingCenterY < aboveCenter)
            {
                nlohmann::json arr = CaptureArray(comp, prop);
                std::swap(arr[s_DragSourceIndex], arr[s_DragSourceIndex - 1]);
                ApplyArray(comp, prop, arr);
                s_DragSourceIndex--;
            }
        }
        if (s_DragSourceIndex < n - 1)
        {
            float belowCenter = arrayTopY + (s_DragSourceIndex + 1) * (cardHeight + cardSpacing) + cardHeight * 0.5f;
            if (floatingCenterY > belowCenter)
            {
                nlohmann::json arr = CaptureArray(comp, prop);
                std::swap(arr[s_DragSourceIndex], arr[s_DragSourceIndex + 1]);
                ApplyArray(comp, prop, arr);
                s_DragSourceIndex++;
            }
        }

        // On release, the reorder becomes one undo command.
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            nlohmann::json finalArray = CaptureArray(comp, prop);
            if (finalArray != s_DragOriginalArray)
            {
                ApplyArray(comp, prop, s_DragOriginalArray);
                CommandHistory::Instance().ExecuteNoMerge(
                    std::make_unique<ModifyComponentPropertyCommand>(comp, prop, finalArray));
            }
            s_DragSourceIndex = -1;
        }
    }

    if (indexToRemove >= 0)
    {
        nlohmann::json newValue = currentValue;
        newValue.erase(indexToRemove);
        CommandHistory::Instance().ExecuteNoMerge(
            std::make_unique<ModifyComponentPropertyCommand>(comp, prop, newValue));
    }

    ImGui::Dummy(ImVec2(0, 4));
}

}  // namespace DekiEditor
