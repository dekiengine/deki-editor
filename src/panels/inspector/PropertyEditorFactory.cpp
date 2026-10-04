#include "PropertyEditorFactory.h"
#include "AssetPropertyEditor.h"
#include "InspectorUtils.h"
#include <deki-editor/EditorUI.h>
#include "../../EditorApp.h"
#include <deki-editor/EditorTheme.h>
#include <deki-editor/IconsTabler.h>
#include <deki-editor/CommandHistory.h>
#include "../../core/Commands.h"
#include "../../services/SafeDispatch.h"
#include "../../core/commands/PropertyTypeHandlers.h"
#include <deki/reflection/Property.h>
#include <deki/reflection/ComponentRegistry.h>
#include <deki/reflection/ComponentFactory.h>
#include <deki/Component.h>
#include <deki/Object.h>
#include <deki/Engine.h>
#include <deki/Math.h>
#include <deki/Scene.h>
#include <deki/Color.h>
#include "imgui.h"
#include <nlohmann/json.hpp>
#include <climits>
#include <cstring>
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace DekiEditor
{

using InspectorUtils::BeginPropertyRow;
using InspectorUtils::ComponentIsOrDerivesFrom;

// Physical-unit conversion (meters/radians -> px/degrees) and Distance pixel
// snapping now happen inside the EditorUI unit-aware *Field helpers, so this file
// no longer needs UnitDisplay/EditorPixelSnap directly.

namespace
{
// The EditorUI *Field helpers record undo via a byte-level ModifyValueCommand,
// which (unlike ModifyComponentPropertyCommand) does NOT notify the component.
// OnPropertyChanged is a virtual that components override to recompute derived
// state (e.g. ScrollComponent re-lays-out its children), and ApplyToggleObject
// handles object-active toggles. These build the per-shape callback structs that
// re-fire those side effects after the field writes the member, for live edits,
// undo, and redo alike, so behavior matches the old reflection command exactly.
void NotifyChanged(Deki::Component* comp, const std::string& name)
{
    comp->OnPropertyChanged(name.c_str());
    ApplyToggleObject(comp, name.c_str());
}

EditorUI::IntCallbacks NotifyInt(Deki::Component* comp, const char* name)
{
    EditorUI::IntCallbacks cb;
    const std::string n = name;
    auto notify = [comp, n](int64_t) { NotifyChanged(comp, n); };
    cb.onChange = notify;
    cb.onUndo = notify;
    cb.onRedo = notify;
    return cb;
}
EditorUI::FloatCallbacks NotifyFloat(Deki::Component* comp, const char* name)
{
    EditorUI::FloatCallbacks cb;
    const std::string n = name;
    auto notify = [comp, n](float) { NotifyChanged(comp, n); };
    cb.onChange = notify;
    cb.onUndo = notify;
    cb.onRedo = notify;
    return cb;
}
EditorUI::VecCallbacks NotifyVec(Deki::Component* comp, const char* name)
{
    EditorUI::VecCallbacks cb;
    const std::string n = name;
    auto notify = [comp, n](const float*) { NotifyChanged(comp, n); };
    cb.onChange = notify;
    cb.onUndo = notify;
    cb.onRedo = notify;
    return cb;
}

// --- "is this still the default?" ----------------------------------------
//
// The reference is a freshly built component of the same type. Building one is
// not free of risk: a game component's constructor and destructor are arbitrary
// project code, and running them on an instance that belongs to no object can
// fault. So it happens ONCE per type, inside the editor's exception barrier,
// and what survives is the captured VALUES, not the instance - nothing of the
// throwaway component outlives the snapshot, and a type whose construction
// faults is simply recorded as having no defaults instead of taking the panel
// down with it every frame.
//
// The snapshot is dropped whenever the factory's registration count changes,
// which is what a package or plugin reload does, so a rebuilt component's new
// defaults are picked up.
struct DefaultsSnapshot
{
    size_t factoryStamp = static_cast<size_t>(-1);
    // typeId -> (property offset -> value on a default-built component)
    std::unordered_map<uint32_t, std::unordered_map<size_t, nlohmann::json>> byType;
    std::unordered_set<uint32_t> unavailable;  // construction faulted, or no factory
};
DefaultsSnapshot g_Defaults;

// Current value of one property, as the undo system already captures it - so
// "differs from default" means exactly what "this edit changed something"
// means, for every type including arrays and sized enums.
nlohmann::json CaptureValue(const Deki::Component* comp, const Deki::PropertyInfo& prop)
{
    if (prop.type == Deki::PropertyType::Array)
    {
        return CaptureArray(comp, prop);
    }
    if (prop.type == Deki::PropertyType::Enum)
    {
        return CaptureEnumSized(comp, prop.offset, prop.enumSize);
    }
    return GetPropertyTypeHandler(prop.type).capture(comp, prop.offset);
}

void BuildDefaultsFor(uint32_t typeId)
{
    auto& registry = Deki::ComponentRegistry::Instance();
    auto& slot = g_Defaults.byType[typeId];

    bool built = false;
    const char* context = "inspector defaults";
    if (const Deki::ComponentMeta* meta = registry.GetMeta(typeId))
    {
        context = meta->name ? meta->name : context;
    }

    Guard(context,
          [&]
          {
              std::unique_ptr<Deki::Component> probe(Deki::ComponentFactory::Instance().Create(typeId));
              if (!probe)
              {
                  return;
              }

              // Own properties and every inherited one: the inspector draws both.
              const Deki::ComponentMeta* walk = registry.GetMeta(typeId);
              while (walk)
              {
                  for (int i = 0; i < walk->propertyCount; ++i)
                  {
                      const Deki::PropertyInfo& p = walk->properties[i];
                      slot[p.offset] = CaptureValue(probe.get(), p);
                  }
                  walk = walk->baseTypeId != 0 ? registry.GetMeta(walk->baseTypeId) : nullptr;
              }
              built = true;
          });

    if (!built)
    {
        g_Defaults.byType.erase(typeId);
        g_Defaults.unavailable.insert(typeId);
        DEKI_LOG_WARNING("Inspector: '%s' cannot be built on its own, so its rows carry no "
                         "modified marker and no reset.",
                         context);
    }
}

// The value this property has on a default-built component, or null when that
// type has no usable default (unregistered, or its construction faulted).
const nlohmann::json* DefaultValueFor(uint32_t typeId, const Deki::PropertyInfo& prop)
{
    const size_t stamp = Deki::ComponentFactory::Instance().GetRegisteredCount();
    if (g_Defaults.factoryStamp != stamp)
    {
        g_Defaults.byType.clear();
        g_Defaults.unavailable.clear();
        g_Defaults.factoryStamp = stamp;
    }

    if (g_Defaults.unavailable.count(typeId))
    {
        return nullptr;
    }
    if (g_Defaults.byType.find(typeId) == g_Defaults.byType.end())
    {
        BuildDefaultsFor(typeId);
    }

    auto typeIt = g_Defaults.byType.find(typeId);
    if (typeIt == g_Defaults.byType.end())
    {
        return nullptr;
    }
    auto valIt = typeIt->second.find(prop.offset);
    return valIt == typeIt->second.end() ? nullptr : &valIt->second;
}

// --- label scrubbing ------------------------------------------------------
//
// Dragging a row's label sideways changes its value, the way the field itself
// does. State is file-scope because exactly one drag exists at a time, whatever
// inspector or panel owns the row.
//
// The live value is kept here in double for the duration of the gesture: an int
// field moving a quarter unit per pixel would otherwise lose the fraction to
// truncation every frame and never move at all. The value is put back to where
// the drag started before the command is built, so the undo step spans the
// whole drag instead of its last frame (the trick DrawBoolProperty uses).
bool g_ScrubActive = false;
const void* g_ScrubOwner = nullptr;  // component, for identity only
size_t g_ScrubOffset = 0;            // property, for identity only
double g_ScrubStart = 0.0;
double g_ScrubValue = 0.0;

bool ScrubbingThis(const Deki::Component* comp, const Deki::PropertyInfo& prop)
{
    return g_ScrubActive && g_ScrubOwner == comp && g_ScrubOffset == prop.offset;
}

// True while the row's label is being dragged; writes the scrubbed value out.
// `speed` is value-per-pixel.
bool LabelScrub(const Deki::Component* comp, const Deki::PropertyInfo& prop, double current, float speed,
                double* outValue)
{
    if (InspectorUtils::RowLabelHovered() && !ImGui::IsAnyItemActive())
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }

    if (!InspectorUtils::RowLabelDragging())
    {
        return false;
    }

    if (!ScrubbingThis(comp, prop))
    {
        g_ScrubActive = true;
        g_ScrubOwner = comp;
        g_ScrubOffset = prop.offset;
        g_ScrubStart = current;
        g_ScrubValue = current;
    }

    g_ScrubValue += static_cast<double>(InspectorUtils::RowLabelDragX()) * speed;
    if (prop.hasRange)
    {
        if (g_ScrubValue < prop.minValue)
        {
            g_ScrubValue = prop.minValue;
        }
        if (g_ScrubValue > prop.maxValue)
        {
            g_ScrubValue = prop.maxValue;
        }
    }
    *outValue = g_ScrubValue;
    return true;
}

// The drag ended: restore the pre-drag value and let one command re-apply the
// result, so the whole gesture is a single undo step. Returns true if it did.
bool EndLabelScrub(Deki::Component* comp, const Deki::PropertyInfo& prop, const nlohmann::json& newValue,
                   const std::function<void(double)>& writeBack)
{
    if (!ScrubbingThis(comp, prop) || InspectorUtils::RowLabelDragging())
    {
        return false;
    }

    g_ScrubActive = false;
    if (g_ScrubStart == g_ScrubValue)
    {
        return false;
    }

    writeBack(g_ScrubStart);
    CommandHistory::Instance().ExecuteNoMerge(std::make_unique<ModifyComponentPropertyCommand>(comp, prop, newValue));
    NotifyChanged(comp, prop.name);
    return true;
}

}  // namespace

PropertyEditorFactory::PropertyEditorFactory(EditorApp* app, AssetPropertyEditor* assetEditor)
    : m_App(app),
      m_AssetEditor(assetEditor)
{
}

void PropertyEditorFactory::ShowTooltip(const Deki::PropertyInfo& prop)
{
    if (prop.tooltip && ImGui::IsItemHovered())
    {
        DekiEditor::Tooltip("%s", prop.tooltip);
    }
}

void PropertyEditorFactory::DrawProperty(Deki::Component* comp, const Deki::PropertyInfo& prop)
{
    ImGui::PushID(prop.name);

    std::string displayName = InspectorUtils::GetDisplayName(prop);

    // The inspector's filter is honoured HERE so every path obeys it: own
    // properties, inherited ones, and whatever a custom editor asks to draw.
    if (!InspectorUtils::PropertyMatchesFilter(displayName.c_str()))
    {
        ImGui::PopID();
        return;
    }
    InspectorUtils::NoteFilterHit();

    // Make the tooltip available to the row label, whichever widget path draws it.
    InspectorUtils::SetNextRowTooltip(prop.tooltip);

    // What the row's marker means: this value has moved since the scene was
    // last written to disk. That is the question you have in front of an object
    // ("what have I changed and not saved?"), so it gets the visible bar; how
    // the value compares to the TYPE's default is a rarer question and lives on
    // the row menu below as an action rather than as a mark.
    // Capturing the value as JSON and comparing it against the saved and the
    // default value ran for every visible property every frame. The answer can
    // only change when a command runs (undo, redo, an edit committing), the
    // saved baseline moves, or an edit is in flight on this very row, so it is
    // cached against the command generation and the saved-state epoch.
    bool isUnsaved = false;
    bool differsFromDefault = false;
    {
        const uint64_t gen = CommandHistory::Instance().GetGeneration();
        const uint64_t epoch = InspectorUtils::SavedStateEpoch();
        RowState& row = m_RowStates[RowKey{ comp, prop.offset }];
        if (row.generation != gen || row.epoch != epoch || row.pendingRefresh)
        {
            const nlohmann::json current = CaptureValue(comp, prop);
            const nlohmann::json* savedValue = InspectorUtils::SavedValueFor(comp, prop);
            row.isUnsaved = savedValue && current != *savedValue;
            const nlohmann::json* defaultValue = DefaultValueFor(comp->GetType(), prop);
            row.differsFromDefault = defaultValue && current != *defaultValue;
            row.generation = gen;
            row.epoch = epoch;
            row.pendingRefresh = false;
        }
        isUnsaved = row.isUnsaved;
        differsFromDefault = row.differsFromDefault;
    }
    InspectorUtils::SetNextRowUnsaved(isUnsaved);

    switch (prop.type)
    {
        case Deki::PropertyType::Int32: DrawInt32Property(comp, prop, displayName.c_str()); break;

        case Deki::PropertyType::UInt32: DrawUInt32Property(comp, prop, displayName.c_str()); break;

        case Deki::PropertyType::UInt8: DrawUInt8Property(comp, prop, displayName.c_str()); break;

        case Deki::PropertyType::Float: DrawFloatProperty(comp, prop, displayName.c_str()); break;

        case Deki::PropertyType::Bool: DrawBoolProperty(comp, prop, displayName.c_str()); break;

        case Deki::PropertyType::String: DrawStringProperty(comp, prop, displayName.c_str()); break;

        case Deki::PropertyType::Color: DrawColorProperty(comp, prop, displayName.c_str()); break;

        case Deki::PropertyType::Vector2: DrawVector2Property(comp, prop, displayName.c_str()); break;

        case Deki::PropertyType::Vector3: DrawVector3Property(comp, prop, displayName.c_str()); break;

        case Deki::PropertyType::Asset:
            m_AssetEditor->DrawAssetProperty(comp, prop, displayName.c_str());
            ShowTooltip(prop);
            break;

        case Deki::PropertyType::AssetRef:
            m_AssetEditor->DrawAssetRefProperty(comp, prop, displayName.c_str());
            ShowTooltip(prop);
            break;

        case Deki::PropertyType::Enum: DrawEnumProperty(comp, prop, displayName.c_str()); break;

        case Deki::PropertyType::ObjectRef: DrawObjectRefProperty(comp, prop, displayName.c_str()); break;

        case Deki::PropertyType::Array: DrawArrayProperty(comp, prop, displayName.c_str()); break;

        default: ImGui::TextDisabled("%s: (unsupported type)", displayName.c_str()); break;
    }

    // Drop the one-shots if this field never drew a labeled row.
    InspectorUtils::SetNextRowTooltip(nullptr);
    InspectorUtils::SetNextRowUnsaved(false);

    // Row menu, opened by right-clicking the label (the row helper records the
    // click; the menu is built here, where the property and its default are
    // known). Reset writes the default through the same command every edit
    // uses, so it undoes like one.
    if (InspectorUtils::TakeRowMenuClick())
    {
        ImGui::OpenPopup("##rowmenu");
    }
    PushContextMenuPopupStyle();
    if (ImGui::BeginPopup("##rowmenu"))
    {
        PushContextMenuItemStyle();
        // The values are looked up on the click (cheap map finds); the
        // per-frame work above only keeps the two enable flags.
        if (ImGui::MenuItem("Revert to Saved", nullptr, false, isUnsaved))
        {
            if (const nlohmann::json* savedValue = InspectorUtils::SavedValueFor(comp, prop))
            {
                CommandHistory::Instance().ExecuteNoMerge(
                    std::make_unique<ModifyComponentPropertyCommand>(comp, prop, *savedValue));
                NotifyChanged(comp, prop.name);
            }
        }
        if (ImGui::MenuItem("Reset to Default", nullptr, false, differsFromDefault))
        {
            if (const nlohmann::json* defaultValue = DefaultValueFor(comp->GetType(), prop))
            {
                CommandHistory::Instance().ExecuteNoMerge(
                    std::make_unique<ModifyComponentPropertyCommand>(comp, prop, *defaultValue));
                NotifyChanged(comp, prop.name);
            }
        }
        ImGui::PopStyleVar();  // ItemSpacing (PushContextMenuItemStyle)
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();  // WindowPadding (PushContextMenuPopupStyle)

    ImGui::PopID();
}

void PropertyEditorFactory::DrawInt32Property(Deki::Component* comp, const Deki::PropertyInfo& prop,
                                              const char* displayName)
{
    int32_t& value = ::Deki::GetPropertyRef<int32_t>(comp, prop);
    const EditorUI::IntCallbacks cb = NotifyInt(comp, prop.name);
    if (prop.hasRange)
    {
        EditorUI::Get().IntField(displayName, &value, 1.0f, static_cast<int64_t>(prop.minValue),
                                 static_cast<int64_t>(prop.maxValue), cb);
    }
    else
    {
        EditorUI::Get().IntField(displayName, &value, 1.0f, 0, 0, cb);
    }

    // Same label scrub as the float rows, a quarter unit per pixel.
    {
        double scrubbed = value;
        if (LabelScrub(comp, prop, value, 0.25f, &scrubbed))
        {
            value = static_cast<int32_t>(scrubbed);
            NotifyChanged(comp, prop.name);
        }
        EndLabelScrub(comp, prop, nlohmann::json(value), [&value](double v) { value = static_cast<int32_t>(v); });
    }
    ShowTooltip(prop);
}

void PropertyEditorFactory::DrawUInt32Property(Deki::Component* comp, const Deki::PropertyInfo& prop,
                                               const char* displayName)
{
    uint32_t& value = ::Deki::GetPropertyRef<uint32_t>(comp, prop);
    const EditorUI::IntCallbacks cb = NotifyInt(comp, prop.name);
    if (prop.hasRange)
    {
        EditorUI::Get().IntField(displayName, &value, 1.0f, static_cast<int64_t>(prop.minValue),
                                 static_cast<int64_t>(prop.maxValue), cb);
    }
    else
    {
        EditorUI::Get().IntField(displayName, &value, 1.0f, 0, INT_MAX, cb);
    }
    ShowTooltip(prop);
}

void PropertyEditorFactory::DrawUInt8Property(Deki::Component* comp, const Deki::PropertyInfo& prop,
                                              const char* displayName)
{
    uint8_t& value = ::Deki::GetPropertyRef<uint8_t>(comp, prop);
    const EditorUI::IntCallbacks cb = NotifyInt(comp, prop.name);
    if (prop.hasRange)
    {
        EditorUI::Get().IntField(displayName, &value, 1.0f, static_cast<int64_t>(prop.minValue),
                                 static_cast<int64_t>(prop.maxValue), cb);
    }
    else
    {
        EditorUI::Get().IntField(displayName, &value, 1.0f, 0, 255, cb);
    }
    ShowTooltip(prop);
}

void PropertyEditorFactory::DrawFloatProperty(Deki::Component* comp, const Deki::PropertyInfo& prop,
                                              const char* displayName)
{
    float& value = ::Deki::GetPropertyRef<float>(comp, prop);
    const EditorUI::FloatCallbacks cb = NotifyFloat(comp, prop.name);
    const EditorUI::Unit unit = static_cast<EditorUI::Unit>(static_cast<int>(prop.physicalUnit));
    if (unit != EditorUI::Unit::None)
    {
        // Unit-aware field converts canonical meters/radians to px/degrees and
        // snaps Distance to whole pixels in Pixels mode (matches the gizmo math).
        EditorUI::Get().FloatField(displayName, &value, unit, cb);
    }
    else if (prop.hasRange && prop.useSlider)
    {
        // DEKI_SLIDER asked for a slider. DEKI_RANGE sets the same min/max but
        // leaves useSlider false, so the two macros have to be told apart here -
        // checking hasRange alone made DEKI_SLIDER a no-op that rendered an
        // ordinary drag field.
        EditorUI::Get().SliderField(displayName, &value, prop.minValue, prop.maxValue, "%.3f", cb);
    }
    else if (prop.hasRange)
    {
        EditorUI::Get().FloatField(displayName, &value, 0.1f, prop.minValue, prop.maxValue, "%.3f", cb);
    }
    else
    {
        EditorUI::Get().FloatField(displayName, &value, 0.1f, 0.0f, 0.0f, "%.3f", cb);
    }

    // Dragging the row's LABEL scrubs the value too, so the widest, most
    // clickable part of the row is not dead space. Speed follows the range when
    // the property has one, so a 0..1 field crosses its range in a sane
    // distance instead of taking a screen and a half.
    {
        const float speed = prop.hasRange ? (prop.maxValue - prop.minValue) / 200.0f : 0.1f;
        double scrubbed = value;
        if (LabelScrub(comp, prop, value, speed, &scrubbed))
        {
            value = static_cast<float>(scrubbed);
            NotifyChanged(comp, prop.name);
        }
        EndLabelScrub(comp, prop, nlohmann::json(value), [&value](double v) { value = static_cast<float>(v); });
    }
    ShowTooltip(prop);
}

void PropertyEditorFactory::DrawBoolProperty(Deki::Component* comp, const Deki::PropertyInfo& prop,
                                             const char* displayName)
{
    bool& value = ::Deki::GetPropertyRef<bool>(comp, prop);
    bool oldValue = value;

    if (EditorUI::Get().Checkbox(displayName, &value))
    {
        // Checkbox is immediate - create command right away
        value = oldValue;  // Restore so command captures old value
        CommandHistory::Instance().ExecuteNoMerge(
            std::make_unique<ModifyComponentPropertyCommand>(comp, prop, nlohmann::json(!oldValue)));
    }

    ShowTooltip(prop);
}

void PropertyEditorFactory::DrawStringProperty(Deki::Component* comp, const Deki::PropertyInfo& prop,
                                               const char* displayName)
{
    std::string& value = ::Deki::GetPropertyRef<std::string>(comp, prop);
    std::string capturedValue = value;  // Capture BEFORE widget modifies it
    char buf[1024];
    strncpy(buf, value.c_str(), sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    static std::string s_OldValue;

    // Use multiline for "text" property (TextComponent)
    if (strcmp(prop.name, "text") == 0)
    {
        ImGui::Text("%s", displayName);
        ShowTooltip(prop);
        if (ImGui::InputTextMultiline("##text_multiline", buf, sizeof(buf), ImVec2(-1, ImGui::GetTextLineHeight() * 4),
                                      ImGuiInputTextFlags_AllowTabInput))
        {
            value = buf;
        }

        // Check activation AFTER widget
        if (ImGui::IsItemActivated())
        {
            s_OldValue = capturedValue;
        }

        // Create undo command when editing finishes
        if (ImGui::IsItemDeactivatedAfterEdit() && value != s_OldValue)
        {
            std::string newValue = value;
            value = s_OldValue;
            CommandHistory::Instance().Execute(
                std::make_unique<ModifyComponentPropertyCommand>(comp, prop, nlohmann::json(newValue)));
        }
    }
    else
    {
        BeginPropertyRow(displayName);
        if (EditorUI::Get().InputText("##value", buf, sizeof(buf)))
        {
            value = buf;
        }

        // Check activation AFTER widget
        if (ImGui::IsItemActivated())
        {
            s_OldValue = capturedValue;
        }

        // Create undo command when editing finishes
        if (ImGui::IsItemDeactivatedAfterEdit() && value != s_OldValue)
        {
            std::string newValue = value;
            value = s_OldValue;
            CommandHistory::Instance().Execute(
                std::make_unique<ModifyComponentPropertyCommand>(comp, prop, nlohmann::json(newValue)));
        }

        ShowTooltip(prop);
    }
}

void PropertyEditorFactory::DrawColorProperty(Deki::Component* comp, const Deki::PropertyInfo& prop,
                                              const char* displayName)
{
    // Colors are stored as Deki::Color struct (r, g, b, a bytes)
    Deki::Color& colorRef = ::Deki::GetPropertyRef<Deki::Color>(comp, prop);
    Deki::Color capturedColor = colorRef;  // Capture BEFORE widget modifies it

    // Convert to float array for ImGui
    float color[4] = { colorRef.r / 255.0f, colorRef.g / 255.0f, colorRef.b / 255.0f, colorRef.a / 255.0f };

    BeginPropertyRow(displayName);
    if (EditorUI::Get().ColorEdit4("##value", color))
    {
        colorRef.r = static_cast<uint8_t>(color[0] * 255);
        colorRef.g = static_cast<uint8_t>(color[1] * 255);
        colorRef.b = static_cast<uint8_t>(color[2] * 255);
        colorRef.a = static_cast<uint8_t>(color[3] * 255);
    }

    // Check activation AFTER widget
    static Deki::Color s_OldColor;
    if (ImGui::IsItemActivated())
    {
        s_OldColor = capturedColor;
    }

    // Create undo command when editing finishes
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        bool changed = (colorRef.r != s_OldColor.r || colorRef.g != s_OldColor.g || colorRef.b != s_OldColor.b ||
                        colorRef.a != s_OldColor.a);
        if (changed)
        {
            Deki::Color newColor = colorRef;
            colorRef = s_OldColor;
            // Store color as array [r, g, b, a] in JSON
            nlohmann::json colorJson = nlohmann::json::array({ newColor.r, newColor.g, newColor.b, newColor.a });
            CommandHistory::Instance().Execute(std::make_unique<ModifyComponentPropertyCommand>(comp, prop, colorJson));
        }
    }

    ShowTooltip(prop);
}

void PropertyEditorFactory::DrawVector2Property(Deki::Component* comp, const Deki::PropertyInfo& prop,
                                                const char* displayName)
{
    Deki::Vector2& stored = ::Deki::GetPropertyRef<Deki::Vector2>(comp, prop);
    const EditorUI::VecCallbacks cb = NotifyVec(comp, prop.name);
    const EditorUI::Unit unit = static_cast<EditorUI::Unit>(static_cast<int>(prop.physicalUnit));
    // Deki::Vector2 is two contiguous floats, so &stored.x is a valid float[2].
    if (unit != EditorUI::Unit::None)
    {
        EditorUI::Get().Vector2Field(displayName, &stored.x, unit, cb);
    }
    else
    {
        EditorUI::Get().Vector2Field(displayName, &stored.x, 0.1f, 0.0f, 0.0f, "%.3f", cb);
    }
    ShowTooltip(prop);
}

void PropertyEditorFactory::DrawVector3Property(Deki::Component* comp, const Deki::PropertyInfo& prop,
                                                const char* displayName)
{
    Deki::Vector3& stored = ::Deki::GetPropertyRef<Deki::Vector3>(comp, prop);
    const EditorUI::VecCallbacks cb = NotifyVec(comp, prop.name);
    // Deki::Vector3 is three contiguous floats, so &stored.x is a valid float[3].
    EditorUI::Get().Vector3Field(displayName, &stored.x, 0.1f, 0.0f, 0.0f, "%.3f", cb);
    ShowTooltip(prop);
}

void PropertyEditorFactory::DrawEnumProperty(Deki::Component* comp, const Deki::PropertyInfo& prop,
                                             const char* displayName)
{
    // Use enumSize from property metadata to read/write the correct number of bytes
    uint8_t enumSize = prop.enumSize > 0 ? prop.enumSize : 1;
    void* valuePtr = reinterpret_cast<char*>(comp) + prop.offset;

    // Read current value
    int currentIndex = 0;
    switch (enumSize)
    {
        case 1: currentIndex = static_cast<int>(*reinterpret_cast<uint8_t*>(valuePtr)); break;
        case 2: currentIndex = static_cast<int>(*reinterpret_cast<uint16_t*>(valuePtr)); break;
        case 4:
        default: currentIndex = *reinterpret_cast<int32_t*>(valuePtr); break;
    }

    int oldIndex = currentIndex;

    if (prop.enumValues && prop.enumCount > 0)
    {
        // What was read out of the field is the enumerator's VALUE, which is
        // not the row it occupies in the tables: an enum may start off zero,
        // leave a gap, or be a flags set. So map value -> row to drive the
        // combo and row -> value to write back. This used to pass the value
        // straight in as an index and clamp it into range, which silently
        // rewrote the field on any enum that was not numbered 0, 1, 2, ...
        int comboRow = Deki::EnumIndexForValue(prop, currentIndex);
        if (comboRow < 0)
        {
            comboRow = 0;  // a value the enum no longer declares
        }
        oldIndex = currentIndex;

        BeginPropertyRow(displayName);
        if (EditorUI::Get().Combo("##value", &comboRow, prop.enumValues, prop.enumCount))
        {
            int32_t chosen = comboRow;
            if (prop.enumInts && comboRow >= 0 && comboRow < prop.enumCount)
            {
                chosen = prop.enumInts[comboRow];
            }
            // Combo is immediate; the command reads the component's old value.
            CommandHistory::Instance().ExecuteNoMerge(
                std::make_unique<ModifyComponentPropertyCommand>(comp, prop, nlohmann::json(chosen)));
        }
    }
    else
    {
        int capturedIndex = currentIndex;  // Capture BEFORE widget modifies it

        BeginPropertyRow(displayName);
        if (EditorUI::Get().DragInt("##value", &currentIndex))
        {
            // Write back using correct size for live preview
            switch (enumSize)
            {
                case 1: *reinterpret_cast<uint8_t*>(valuePtr) = static_cast<uint8_t>(currentIndex); break;
                case 2: *reinterpret_cast<uint16_t*>(valuePtr) = static_cast<uint16_t>(currentIndex); break;
                case 4:
                default: *reinterpret_cast<int32_t*>(valuePtr) = currentIndex; break;
            }
        }

        // Check activation AFTER widget
        static int s_OldEnumValue = 0;
        if (ImGui::IsItemActivated())
        {
            s_OldEnumValue = capturedIndex;
        }

        // Create undo command when editing finishes
        if (ImGui::IsItemDeactivatedAfterEdit() && currentIndex != s_OldEnumValue)
        {
            // Restore old value so command captures it
            switch (enumSize)
            {
                case 1: *reinterpret_cast<uint8_t*>(valuePtr) = static_cast<uint8_t>(s_OldEnumValue); break;
                case 2: *reinterpret_cast<uint16_t*>(valuePtr) = static_cast<uint16_t>(s_OldEnumValue); break;
                case 4:
                default: *reinterpret_cast<int32_t*>(valuePtr) = s_OldEnumValue; break;
            }
            CommandHistory::Instance().Execute(
                std::make_unique<ModifyComponentPropertyCommand>(comp, prop, nlohmann::json(currentIndex)));
        }
    }
    ShowTooltip(prop);
}

// Helper to build hierarchy path string for an object (e.g. "Root/Parent/Child")
static std::string GetHierarchyPath(Deki::Object* obj)
{
    std::vector<std::string> parts;
    Deki::Object* cur = obj->GetParent();
    while (cur)
    {
        parts.push_back(cur->GetName());
        cur = cur->GetParent();
    }
    std::string path;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it)
    {
        path += *it;
        path += "/";
    }
    return path;
}

// Case-insensitive substring match
static bool MatchesSearch(const std::string& text, const char* search)
{
    if (!search || search[0] == '\0')
    {
        return true;
    }
    std::string lowerText = text;
    std::string lowerSearch = search;
    for (auto& c : lowerText)
    {
        c = static_cast<char>(tolower(c));
    }
    for (auto& c : lowerSearch)
    {
        c = static_cast<char>(tolower(c));
    }
    return lowerText.find(lowerSearch) != std::string::npos;
}

void PropertyEditorFactory::OpenObjectRefPicker(Deki::Component* comp, const Deki::PropertyInfo* prop, int arrayIndex)
{
    m_ObjectRefPickerComp = comp;
    m_ObjectRefPickerProp = prop;
    m_ObjectRefPickerIndex = arrayIndex;
    m_ObjectRefSearchBuf[0] = '\0';
    m_ObjectRefPickerJustOpened = true;
    m_ObjectRefPickerNeedsScroll = true;
    m_ObjectRefPickerPendingOpen = true;
}

void PropertyEditorFactory::DrawPendingPopups()
{
    if (m_ObjectRefPickerPendingOpen)
    {
        ImGui::OpenPopup("ObjectRefPicker");
        m_ObjectRefPickerPendingOpen = false;
    }

    // Anchor below the field at the inspector's left edge, locked to the
    // inspector's width so rows can never extend past the popup, with the
    // unified context-menu inset (same as the Add Component picker).
    const float popupW = m_ObjectRefPickerWidth > 0 ? m_ObjectRefPickerWidth : m_App->Dpi(500.0f);
    ImGui::SetNextWindowPos(ImVec2(m_ObjectRefPickerPosX, m_ObjectRefPickerPosY), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(popupW, 0.0f), ImVec2(popupW, FLT_MAX));

    PushContextMenuPopupStyle();
    if (ImGui::BeginPopup("ObjectRefPicker"))
    {
        DrawObjectRefPicker();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();  // context menu WindowPadding
}

void PropertyEditorFactory::ResetTransientState()
{
    m_RowStates.clear();
    m_ObjectRefPickerProp = nullptr;
    m_ObjectRefPickerComp = nullptr;
    m_ObjectRefPickerIndex = -1;
    m_ObjectRefPickerJustOpened = false;
    m_ObjectRefPickerPendingOpen = false;
    m_ObjectRefPickerNeedsScroll = false;
    m_ObjectRefSearchBuf[0] = '\0';
}

void PropertyEditorFactory::DrawObjectRefProperty(Deki::Component* comp, const Deki::PropertyInfo& prop,
                                                  const char* displayName)
{
    Deki::ObjectRefBase& ref = *reinterpret_cast<Deki::ObjectRefBase*>(reinterpret_cast<char*>(comp) + prop.offset);

    // Get display name of referenced object
    std::string refDisplayText = "(None)";
    // A reference whose target is gone is not a value, it is damage: it reads
    // like any other name in a plain field, so it gets amber text and a border
    // to match, plus "Clear Reference" on the row menu. Being able to see it
    // and fix it in place beats hunting for which field went stale.
    bool refBroken = false;
    ::Deki::Scene* scene = m_App->GetCurrentScene();
    if (ref.id != 0 && scene)
    {
        Deki::Object* targetObj = scene->FindDekiObject(ref.id);
        if (targetObj)
        {
            refDisplayText = targetObj->GetName();
            if (ref.componentTypeId != 0)
            {
                auto& reg = Deki::ComponentRegistry::Instance();
                const auto* meta = reg.GetMeta(ref.componentTypeId);
                if (meta && meta->name)
                {
                    refDisplayText += " - " + std::string(meta->name);
                }
            }
            else if (prop.componentRefType)
            {
                std::string compName = InspectorUtils::GetMatchingComponentName(targetObj, prop.componentRefType);
                if (!compName.empty())
                {
                    refDisplayText += " - " + compName;
                }
            }
        }
        else
        {
            refDisplayText = ICON_TI_ALERT_TRIANGLE "  Missing object (id " + std::to_string(ref.id) + ")";
            refBroken = true;
        }
    }

    BeginPropertyRow(displayName);

    // Combined name + chevron field, matching the AssetRef rows (FrameBg-colored
    // left-aligned name with a picker chevron flush on the right). Inherit the
    // right gutter set by BeginPropertyRow so it lines up with the other rows.
    float fieldWidth = ImGui::CalcItemWidth();
    float pickerBtnWidth = ImGui::GetFrameHeight();
    float nameFieldWidth = fieldWidth - pickerBtnWidth;

    // Position popup at the window's left edge, below the clicked field
    auto openPicker = [&]()
    {
        m_ObjectRefPickerPosX = ImGui::GetWindowPos().x;
        m_ObjectRefPickerPosY = ImGui::GetItemRectMax().y;
        m_ObjectRefPickerWidth = ImGui::GetWindowSize().x;
        OpenObjectRefPicker(comp, &prop);
    };

    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_FrameBg]);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyle().Colors[ImGuiCol_FrameBgHovered]);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyle().Colors[ImGuiCol_FrameBgActive]);
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
    if (refBroken)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, DekiEditor::Palette::Amber);
        ImGui::PushStyleColor(ImGuiCol_Border, DekiEditor::Palette::Amber);
    }

    std::string nameBtnId = refDisplayText + "##objrefname_" + prop.name;
    if (ImGui::Button(nameBtnId.c_str(), ImVec2(nameFieldWidth, 0)))
    {
        openPicker();
    }
    if (refBroken && ImGui::IsItemHovered())
    {
        DekiEditor::Tooltip("The object this points at is not in the scene any more.\n"
                            "Pick a new one, or clear it from the row menu (right-click the label).");
    }

    if (refBroken)
    {
        ImGui::PopStyleColor(2);
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);

    // Drag-drop target for objects from Hierarchy panel
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_OBJECT"))
        {
            Deki::Object* droppedObj = *(Deki::Object**)payload->Data;
            if (droppedObj)
            {
                Deki::Component* matchedComp = nullptr;
                int matchCount = 0;
                bool isValid = (prop.componentRefType == nullptr);
                if (prop.componentRefType)
                {
                    for (auto* c : droppedObj->GetComponents())
                    {
                        if (ComponentIsOrDerivesFrom(c, prop.componentRefType))
                        {
                            isValid = true;
                            matchedComp = c;
                            matchCount++;
                        }
                    }
                }

                if (isValid)
                {
                    nlohmann::json newVal;
                    if (matchedComp)
                    {
                        newVal = { { "id", droppedObj->GetId() }, { "typeId", matchedComp->GetType() } };
                    }
                    else
                    {
                        newVal = droppedObj->GetId();
                    }
                    CommandHistory::Instance().ExecuteNoMerge(
                        std::make_unique<ModifyComponentPropertyCommand>(comp, prop, newVal));
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    ShowTooltip(prop);

    // Right: picker chevron, flush against the name field (one combined control)
    ImGui::SameLine(0, 0);
    std::string pickerBtnId = std::string(ICON_TI_CHEVRON_DOWN "##objrefpicker_") + prop.name;
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
    if (ImGui::Button(pickerBtnId.c_str(), ImVec2(pickerBtnWidth, 0)))
    {
        openPicker();
    }
    ImGui::PopStyleVar();
}

void PropertyEditorFactory::DrawObjectRefPicker()
{
    if (!m_ObjectRefPickerProp || !m_ObjectRefPickerComp)
    {
        return;
    }

    const Deki::PropertyInfo& prop = *m_ObjectRefPickerProp;
    Deki::Component* comp = m_ObjectRefPickerComp;
    int arrayIdx = m_ObjectRefPickerIndex;
    bool isArray = (arrayIdx >= 0);

    // Get current selection
    uint32_t currentObjId = 0;
    uint32_t currentTypeId = 0;
    nlohmann::json currentArrayValue;

    if (isArray)
    {
        currentArrayValue = CaptureArray(comp, prop);
        if (arrayIdx < static_cast<int>(currentArrayValue.size()))
        {
            if (currentArrayValue[arrayIdx].is_object())
            {
                currentObjId = currentArrayValue[arrayIdx].value("id", 0u);
                currentTypeId = currentArrayValue[arrayIdx].value("typeId", 0u);
            }
            else
            {
                currentObjId = currentArrayValue[arrayIdx].get<uint32_t>();
            }
        }
    }
    else
    {
        Deki::ObjectRefBase& ref = *reinterpret_cast<Deki::ObjectRefBase*>(reinterpret_cast<char*>(comp) + prop.offset);
        currentObjId = ref.id;
        currentTypeId = ref.componentTypeId;
    }

    // Resolve componentTypeId if not set but we have a type filter
    if (currentObjId != 0 && currentTypeId == 0 && prop.componentRefType)
    {
        ::Deki::Scene* scene = m_App->GetCurrentScene();
        if (scene)
        {
            Deki::Object* targetObj = scene->FindDekiObject(currentObjId);
            if (targetObj)
            {
                for (auto* c : targetObj->GetComponents())
                {
                    if (ComponentIsOrDerivesFrom(c, prop.componentRefType))
                    {
                        currentTypeId = c->GetType();
                        break;
                    }
                }
            }
        }
    }

    // Focus search on first frame
    if (m_ObjectRefPickerJustOpened)
    {
        ImGui::SetKeyboardFocusHere();
        m_ObjectRefPickerJustOpened = false;
    }

    // Schematic search band (dim magnifier + borderless input) with a
    // full-width hairline beneath it, like the Add Component picker.
    SearchField("##objref_search", m_ObjectRefSearchBuf, sizeof(m_ObjectRefSearchBuf), 0.0f, /*bottomBorder*/ true);

    // Row metrics. Rows are custom-drawn (name + dim component type on one
    // line, dim hierarchy path underneath) and every text draw is clipped to
    // the row rect, so long names stop at the popup edge instead of spilling
    // past it.
    const float padX = ImGui::GetStyle().FramePadding.x;
    const float padY = m_App->Dpi(5.0f);
    const float lineGap = m_App->Dpi(2.0f);
    const float lineH = ImGui::GetTextLineHeight();
    ImFont* pathFont = g_FontUI ? g_FontUI : ImGui::GetFont();
    const float pathSize = Fonts::CssPx(pathFont, 11.5f) * m_App->Dpi(1.0f);
    const ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 dimCol = ImGui::GetColorU32(Palette::Dim);

    // One picker row: primary-colored name, dim `suffix` (component type)
    // continuing on the same line, dim smaller `path` below. Returns true
    // when the row is clicked. Callers wrap in PushID for unique row ids.
    auto drawRow = [&](const char* name, const std::string& suffix, const std::string& path, bool isSelected) -> bool
    {
        const bool twoLines = !path.empty();
        const float rowH = padY * 2.0f + lineH + (twoLines ? lineGap + pathSize : 0.0f);

        if (isSelected)
        {
            ImGui::PushStyleColor(ImGuiCol_Header, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
        }
        // Size.x = 0 spans the remaining row width
        const bool clicked = ImGui::Selectable("##item", isSelected, ImGuiSelectableFlags_None, ImVec2(0, rowH));
        if (isSelected)
        {
            ImGui::PopStyleColor();
        }
        if (isSelected && m_ObjectRefPickerNeedsScroll)
        {
            ImGui::SetScrollHereY();
        }

        const ImVec2 itemMin = ImGui::GetItemRectMin();
        const ImVec2 itemMax = ImGui::GetItemRectMax();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(itemMin, itemMax, true);

        const float textX = itemMin.x + padX;
        const float line1Y = itemMin.y + padY;
        dl->AddText(ImVec2(textX, line1Y), textCol, name);
        if (!suffix.empty())
        {
            const float nameW = ImGui::CalcTextSize(name).x;
            dl->AddText(ImVec2(textX + nameW + m_App->Dpi(8.0f), line1Y), dimCol, suffix.c_str());
        }
        if (twoLines)
        {
            dl->AddText(pathFont, pathSize, ImVec2(textX, line1Y + lineH + lineGap), dimCol, path.c_str());
        }
        dl->PopClipRect();
        return clicked;
    };

    // Compact list: snug for short content, scrolls once it reaches the cap.
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, m_App->Dpi(340.0f)));
    ImGui::BeginChild("##objref_list", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, m_App->Dpi(2.0f)));

    auto selectNone = [&]()
    {
        if (isArray)
        {
            nlohmann::json newValue = currentArrayValue;
            newValue[arrayIdx] = 0;
            CommandHistory::Instance().ExecuteNoMerge(
                std::make_unique<ModifyComponentPropertyCommand>(comp, prop, newValue));
        }
        else
        {
            CommandHistory::Instance().ExecuteNoMerge(
                std::make_unique<ModifyComponentPropertyCommand>(comp, prop, nlohmann::json(0)));
        }
        ImGui::CloseCurrentPopup();
    };

    // "(None)" option
    if (MatchesSearch("None", m_ObjectRefSearchBuf))
    {
        bool noneSelected = (currentObjId == 0);
        ImGui::PushID("none");
        if (drawRow("(None)", std::string(), std::string(), noneSelected))
        {
            if (currentObjId != 0)
            {
                selectNone();
            }
            else
            {
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::PopID();
    }

    // List all matching objects
    ::Deki::Scene* scene = m_App->GetCurrentScene();
    if (scene)
    {
        auto& registry = Deki::ComponentRegistry::Instance();
        int itemIdx = 0;

        std::function<void(Deki::Object*)> showObject = [&](Deki::Object* sceneObj)
        {
            std::string objName = sceneObj->GetName();
            std::string hierarchyPath = GetHierarchyPath(sceneObj);

            if (prop.componentRefType)
            {
                for (auto* c : sceneObj->GetComponents())
                {
                    if (!ComponentIsOrDerivesFrom(c, prop.componentRefType))
                    {
                        continue;
                    }

                    const auto* meta = registry.GetMeta(c->GetType());
                    std::string compName = (meta && meta->name) ? meta->name : "Unknown";

                    // Search against name, path, and component
                    std::string searchText = objName + " " + hierarchyPath + " " + compName;
                    if (!MatchesSearch(searchText, m_ObjectRefSearchBuf))
                    {
                        itemIdx++;
                        continue;
                    }

                    bool isSelected = (currentObjId == sceneObj->GetId() && currentTypeId == c->GetType());

                    ImGui::PushID(itemIdx++);
                    if (drawRow(objName.c_str(), compName, hierarchyPath, isSelected))
                    {
                        nlohmann::json newVal = { { "id", sceneObj->GetId() }, { "typeId", c->GetType() } };
                        if (isArray)
                        {
                            nlohmann::json newArr = currentArrayValue;
                            newArr[arrayIdx] = newVal;
                            CommandHistory::Instance().ExecuteNoMerge(
                                std::make_unique<ModifyComponentPropertyCommand>(comp, prop, newArr));
                        }
                        else
                        {
                            CommandHistory::Instance().ExecuteNoMerge(
                                std::make_unique<ModifyComponentPropertyCommand>(comp, prop, newVal));
                        }
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::PopID();
                }
            }
            else
            {
                // No component filter - show object name
                std::string searchText = objName + " " + hierarchyPath;
                if (MatchesSearch(searchText, m_ObjectRefSearchBuf))
                {
                    bool isSelected = (currentObjId == sceneObj->GetId());

                    ImGui::PushID(itemIdx++);
                    if (drawRow(objName.c_str(), std::string(), hierarchyPath, isSelected))
                    {
                        nlohmann::json newVal = sceneObj->GetId();
                        if (isArray)
                        {
                            nlohmann::json newArr = currentArrayValue;
                            newArr[arrayIdx] = newVal;
                            CommandHistory::Instance().ExecuteNoMerge(
                                std::make_unique<ModifyComponentPropertyCommand>(comp, prop, newArr));
                        }
                        else
                        {
                            CommandHistory::Instance().ExecuteNoMerge(
                                std::make_unique<ModifyComponentPropertyCommand>(comp, prop, newVal));
                        }
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::PopID();
                }
                else
                {
                    itemIdx++;
                }
            }

            // Recurse into children
            for (Deki::Object* child : sceneObj->GetChildren())
            {
                showObject(child);
            }
        };

        for (Deki::Object* rootObj : scene->GetObjects())
        {
            if (rootObj->GetParent() == nullptr)
            {
                showObject(rootObj);
            }
        }
    }

    ImGui::PopStyleVar();  // list ItemSpacing
    ImGui::EndChild();
    m_ObjectRefPickerNeedsScroll = false;
}

}  // namespace DekiEditor
