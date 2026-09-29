// PropertyEditorFactory Multi-Selection Implementation
// Extracted from PropertyEditorFactory.cpp - DrawPropertyMulti

#include "PropertyEditorFactory.h"
#include <deki-editor/EditorTheme.h>
#include "InspectorUtils.h"
#include <deki-editor/EditorUI.h>
#include <deki-editor/CommandHistory.h>
#include "../../core/Commands.h"
#include "../../core/commands/PropertyTypeHandlers.h"
#include <deki/reflection/Property.h>
#include <deki/Component.h>
#include "imgui.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstring>

namespace DekiEditor
{

using InspectorUtils::BeginPropertyRow;

// Helper to check if all values are the same across components
template <typename T>
static bool AreValuesSame(const std::vector<Deki::Component*>& components, const Deki::PropertyInfo& prop, T* outValue = nullptr)
{
    if (components.empty()) return false;

    T firstValue = ::Deki::GetPropertyRef<T>(components[0], prop);
    for (size_t i = 1; i < components.size(); ++i)
    {
        T value = ::Deki::GetPropertyRef<T>(components[i], prop);
        if (value != firstValue)
            return false;
    }

    if (outValue) *outValue = firstValue;
    return true;
}

void PropertyEditorFactory::DrawReadOnlyMulti(const std::vector<Deki::Component*>& components,
                                              const Deki::PropertyInfo& prop)
{
    ImGui::TextDisabled("(%zu objects: value from the first, edit one at a time)", components.size());
    ImGui::BeginDisabled();
    DrawProperty(components[0], prop);
    ImGui::EndDisabled();
}

void PropertyEditorFactory::DrawPropertyMulti(const std::vector<Deki::Component*>& components,
                                              const Deki::PropertyInfo& prop)
{
    if (components.empty()) return;

    ImGui::PushID(prop.name);

    std::string displayName = InspectorUtils::GetDisplayName(prop);

    // Make the tooltip available to the row label, whichever widget path draws it.
    InspectorUtils::SetNextRowTooltip(prop.tooltip);

    // For most types, we'll check if values are mixed and show appropriate UI
    switch (prop.type)
    {
        case Deki::PropertyType::Int32:
        {
            int32_t value = 0;
            bool same = AreValuesSame<int32_t>(components, prop, &value);

            BeginPropertyRow(displayName.c_str());
            if (!same)
            {
                // Mixed values - show grey "-" that can be edited
                char buf[32] = "-";
                ImGui::PushStyleColor(ImGuiCol_Text, DekiEditor::Palette::Dim);
                if (ImGui::InputText("##value", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue))
                {
                    // Parse the new value
                    int32_t newValue = 0;
                    if (sscanf(buf, "%d", &newValue) == 1)
                    {
                        CommandHistory::Instance().ExecuteNoMerge(
                            std::make_unique<ModifyMultipleComponentPropertyCommand>(
                                components, prop, nlohmann::json(newValue)));
                    }
                }
                ImGui::PopStyleColor();
            }
            else
            {
                // Same value - normal editor
                int32_t capturedValue = value;
                if (prop.hasRange)
                {
                    if (ImGui::InputInt("##value", &value))
                    {
                        value = std::clamp(value,
                                           static_cast<int32_t>(prop.minValue),
                                           static_cast<int32_t>(prop.maxValue));
                    }
                }
                else
                {
                    DekiEditor::SchematicDragInt("##value", &value);
                }

                static int32_t s_OldValue = 0;
                if (ImGui::IsItemActivated())
                {
                    s_OldValue = capturedValue;
                }

                if (ImGui::IsItemDeactivatedAfterEdit() && value != s_OldValue)
                {
                    // Restore all components to old value before command
                    for (Deki::Component* comp : components)
                    {
                        ::Deki::GetPropertyRef<int32_t>(comp, prop) = s_OldValue;
                    }
                    CommandHistory::Instance().Execute(
                        std::make_unique<ModifyMultipleComponentPropertyCommand>(
                            components, prop, nlohmann::json(value)));
                }
            }
            break;
        }

        case Deki::PropertyType::Float:
        {
            float value = 0.0f;
            bool same = AreValuesSame<float>(components, prop, &value);

            BeginPropertyRow(displayName.c_str());
            if (!same)
            {
                // Mixed values
                char buf[32] = "-";
                ImGui::PushStyleColor(ImGuiCol_Text, DekiEditor::Palette::Dim);
                if (ImGui::InputText("##value", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue))
                {
                    float newValue = 0.0f;
                    if (sscanf(buf, "%f", &newValue) == 1)
                    {
                        CommandHistory::Instance().ExecuteNoMerge(
                            std::make_unique<ModifyMultipleComponentPropertyCommand>(
                                components, prop, nlohmann::json(newValue)));
                    }
                }
                ImGui::PopStyleColor();
            }
            else
            {
                // Same value - normal editor
                float capturedValue = value;
                if (prop.hasRange)
                {
                    if (ImGui::InputFloat("##value", &value))
                    {
                        value = std::clamp(value, prop.minValue, prop.maxValue);
                    }
                }
                else
                {
                    DekiEditor::SchematicDragFloat("##value", &value, 0.1f);
                }

                static float s_OldValue = 0.0f;
                if (ImGui::IsItemActivated())
                {
                    s_OldValue = capturedValue;
                }

                if (ImGui::IsItemDeactivatedAfterEdit() && value != s_OldValue)
                {
                    for (Deki::Component* comp : components)
                    {
                        ::Deki::GetPropertyRef<float>(comp, prop) = s_OldValue;
                    }
                    CommandHistory::Instance().Execute(
                        std::make_unique<ModifyMultipleComponentPropertyCommand>(
                            components, prop, nlohmann::json(value)));
                }
            }
            break;
        }

        case Deki::PropertyType::Bool:
        {
            bool value = false;
            bool same = AreValuesSame<bool>(components, prop, &value);

            if (!same)
            {
                // Mixed values - show an indeterminate checkbox + "-". This branch
                // is bespoke (the trailing dash), so it draws its own row, but uses
                // the same collapsed FramePadding as EditorUI::Checkbox so the row
                // height matches the unified compact checkbox rows.
                const ImVec2 fp = ImGui::GetStyle().FramePadding;
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(fp.x, ImGui::GetFontSize() * 0.1f));
                BeginPropertyRow(displayName.c_str());
                ImGui::PushStyleColor(ImGuiCol_CheckMark, DekiEditor::Palette::Dim);
                bool dummy = false;
                if (DekiEditor::SchematicCheckbox("##value", &dummy))
                {
                    // Set all to true
                    CommandHistory::Instance().ExecuteNoMerge(
                        std::make_unique<ModifyMultipleComponentPropertyCommand>(
                            components, prop, nlohmann::json(true)));
                }
                ImGui::SameLine();
                ImGui::TextDisabled("-");
                ImGui::PopStyleColor();
                ImGui::PopStyleVar();
            }
            else
            {
                bool oldValue = value;
                if (EditorUI::Get().Checkbox(displayName.c_str(), &value))
                {
                    // Restore before command
                    for (Deki::Component* comp : components)
                    {
                        ::Deki::GetPropertyRef<bool>(comp, prop) = oldValue;
                    }
                    CommandHistory::Instance().ExecuteNoMerge(
                        std::make_unique<ModifyMultipleComponentPropertyCommand>(
                            components, prop, nlohmann::json(!oldValue)));
                }
            }
            break;
        }

        case Deki::PropertyType::String:
        {
            std::string value;
            bool same = AreValuesSame<std::string>(components, prop, &value);

            char buf[1024];
            if (!same)
            {
                strcpy(buf, "-");
                ImGui::PushStyleColor(ImGuiCol_Text, DekiEditor::Palette::Dim);
            }
            else
            {
                strncpy(buf, value.c_str(), sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = '\0';
            }

            static std::string s_OldValue;
            std::string capturedValue = same ? value : "";

            BeginPropertyRow(displayName.c_str());
            if (ImGui::InputText("##value", buf, sizeof(buf)))
            {
                // Update all immediately for preview
                for (Deki::Component* comp : components)
                {
                    ::Deki::GetPropertyRef<std::string>(comp, prop) = buf;
                }
            }

            if (!same)
            {
                ImGui::PopStyleColor();
            }

            if (ImGui::IsItemActivated())
            {
                s_OldValue = capturedValue;
            }

            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                std::string newValue = buf;
                // Restore old values
                for (Deki::Component* comp : components)
                {
                    ::Deki::GetPropertyRef<std::string>(comp, prop) = s_OldValue;
                }
                CommandHistory::Instance().Execute(
                    std::make_unique<ModifyMultipleComponentPropertyCommand>(
                        components, prop, nlohmann::json(newValue)));
            }
            break;
        }

        case Deki::PropertyType::Color:
        {
            Deki::Color value{};
            const bool same = AreValuesSame<Deki::Color>(components, prop, &value);
            float color[4] = { value.r / 255.0f, value.g / 255.0f, value.b / 255.0f, value.a / 255.0f };
            if (!same)
                color[0] = color[1] = color[2] = 0.5f, color[3] = 1.0f;

            BeginPropertyRow(displayName.c_str());
            if (!same) ImGui::PushStyleColor(ImGuiCol_Text, DekiEditor::Palette::Dim);
            const bool edited = EditorUI::Get().ColorEdit4("##value", color);
            if (!same) ImGui::PopStyleColor();
            if (edited)
            {
                // Live preview on every selected component; the command below
                // records the real old values when the edit finishes.
                for (Deki::Component* comp : components)
                {
                    Deki::Color& ref = ::Deki::GetPropertyRef<Deki::Color>(comp, prop);
                    ref.r = static_cast<uint8_t>(color[0] * 255);
                    ref.g = static_cast<uint8_t>(color[1] * 255);
                    ref.b = static_cast<uint8_t>(color[2] * 255);
                    ref.a = static_cast<uint8_t>(color[3] * 255);
                }
            }

            static std::vector<Deki::Color> s_OldColors;
            if (ImGui::IsItemActivated())
            {
                s_OldColors.clear();
                for (Deki::Component* comp : components)
                    s_OldColors.push_back(::Deki::GetPropertyRef<Deki::Color>(comp, prop));
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && s_OldColors.size() == components.size())
            {
                const Deki::Color newColor{ static_cast<uint8_t>(color[0] * 255), static_cast<uint8_t>(color[1] * 255),
                                            static_cast<uint8_t>(color[2] * 255), static_cast<uint8_t>(color[3] * 255) };
                // Restore, then let the command apply so undo sees each old value.
                for (size_t i = 0; i < components.size(); ++i)
                    ::Deki::GetPropertyRef<Deki::Color>(components[i], prop) = s_OldColors[i];
                CommandHistory::Instance().Execute(
                    std::make_unique<ModifyMultipleComponentPropertyCommand>(
                        components, prop,
                        nlohmann::json::array({ newColor.r, newColor.g, newColor.b, newColor.a })));
            }
            break;
        }

        case Deki::PropertyType::Enum:
        {
            if (!prop.enumValues || prop.enumCount <= 0)
            {
                DrawReadOnlyMulti(components, prop);
                break;
            }
            const uint8_t enumSize = prop.enumSize > 0 ? prop.enumSize : 1;
            auto readIndex = [&](Deki::Component* comp) -> int
            {
                const void* p = reinterpret_cast<const char*>(comp) + prop.offset;
                switch (enumSize)
                {
                    case 1: return *reinterpret_cast<const uint8_t*>(p);
                    case 2: return *reinterpret_cast<const uint16_t*>(p);
                    default: return *reinterpret_cast<const int32_t*>(p);
                }
            };
            // readIndex returns the stored enumerator VALUE, not its row in the
            // tables; those differ on any enum that is not numbered 0, 1, 2...
            // Map value -> row for the combo and row -> value for the write.
            const int32_t value = readIndex(components[0]);
            bool same = true;
            for (size_t i = 1; i < components.size() && same; ++i)
                same = readIndex(components[i]) == value;
            int index = Deki::EnumIndexForValue(prop, value);
            if (index < 0) index = 0;  // a value the enum no longer declares

            BeginPropertyRow(displayName.c_str());
            if (!same) ImGui::PushStyleColor(ImGuiCol_Text, DekiEditor::Palette::Dim);
            const bool changed = EditorUI::Get().Combo("##value", &index, prop.enumValues, prop.enumCount);
            if (!same) ImGui::PopStyleColor();
            if (changed)
            {
                int32_t chosen = index;
                if (prop.enumInts && index >= 0 && index < prop.enumCount)
                    chosen = prop.enumInts[index];
                CommandHistory::Instance().ExecuteNoMerge(
                    std::make_unique<ModifyMultipleComponentPropertyCommand>(
                        components, prop, nlohmann::json(chosen)));
            }
            break;
        }

        // Types with no multi-edit path yet. Shown read-only from the first
        // component: this used to draw a LIVE editor for components[0] under a
        // "(N objects)" label, so an edit changed one object while the label
        // said it changed all of them.
        case Deki::PropertyType::Vector2:
        case Deki::PropertyType::Vector3:
        case Deki::PropertyType::Asset:
        case Deki::PropertyType::AssetRef:
        case Deki::PropertyType::ObjectRef:
        case Deki::PropertyType::Array:
        default:
            DrawReadOnlyMulti(components, prop);
            break;
    }

    ShowTooltip(prop);

    // Drop the one-shot tooltip if this field never drew a labeled row.
    InspectorUtils::SetNextRowTooltip(nullptr);

    ImGui::PopID();
}

}  // namespace DekiEditor
