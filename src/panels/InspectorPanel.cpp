#include "InspectorPanel.h"
#include "../PanelMaximize.h"
#include "inspector/InspectorUtils.h"
#include "inspector/PropertyEditorFactory.h"
#include "inspector/AssetPropertyEditor.h"
#include "inspector/ComponentManager.h"
#include "inspector/TextureInspector.h"
#include "inspector/AssetTypeInspector.h"
#include <deki-editor/AssetPicker.h>
#include "../EditorApp.h"
#include "../core/Units.h"
#include <deki-editor/EditorTheme.h>
#include <deki-editor/IconsTabler.h>
#include <deki-editor/EditorExtension.h>
#include <deki-editor/CustomEditor.h>
#include <deki-editor/EditorUI.h>
#include <deki-editor/AssetDatabase.h>
#include <deki-editor/EditorAssets.h>
#include "../services/ClipboardService.h"
#include "../services/SafeDispatch.h"
#include "AssetBrowserPanel.h"
#include <deki-editor/AssetPipeline.h>
#include <deki-editor/CommandHistory.h>
#include "../core/Commands.h"
#include "imgui.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <set>

// Engine includes
#include <deki/Object.h>
#include <deki/Engine.h>  // for DekiEngineSettings::Global() (transform unit suffix)
#include <deki/Math.h>    // for DekiMath::kRadToDeg / kDegToRad
#include <deki/Component.h>
#include <deki/Scene.h>
#include <deki/reflection/Property.h>
#include <deki/reflection/ComponentRegistry.h>
#include <deki/assets/AssetManager.h>

namespace DekiEditor
{

int InspectorPanel::s_GlobalLiveEditCount = 0;

namespace
{

// Look up a Deki::PropertyInfo by component serialized name + property name.
// Walks base-class properties too, mirroring how CollectOverrides diffs.
const Deki::PropertyInfo* FindPropertyInfo(const std::string& componentName,
                                         const std::string& propertyName)
{
    if (componentName.empty() || propertyName.empty()) return nullptr;

    auto& registry = Deki::ComponentRegistry::Instance();
    const Deki::ComponentMeta* meta = registry.GetMeta(componentName);
    while (meta)
    {
        for (int i = 0; i < meta->propertyCount; ++i)
        {
            const Deki::PropertyInfo& prop = meta->properties[i];
            if (prop.name && propertyName == prop.name)
                return &prop;
        }
        meta = (meta->baseTypeId != 0) ? registry.GetMeta(meta->baseTypeId) : nullptr;
    }
    return nullptr;
}

// Format a JSON value for display, using the property's type to pick a
// friendly representation (hex for colors, enum names, true/false, asset
// source label, etc.). Falls back to json.dump() for unknown shapes.
std::string FormatOverrideValue(const nlohmann::json& value,
                                const Deki::PropertyInfo* prop)
{
    if (value.is_null()) return "null";
    if (value.is_boolean()) return value.get<bool>() ? "true" : "false";

    if (prop)
    {
        switch (prop->type)
        {
            case Deki::PropertyType::Color:
            case Deki::PropertyType::UInt32:
                if (value.is_number_unsigned())
                {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "#%08X",
                             value.get<uint32_t>());
                    return buf;
                }
                break;

            case Deki::PropertyType::Enum:
                if (value.is_number_integer() && prop->enumValues && prop->enumCount > 0)
                {
                    int idx = value.get<int>();
                    if (idx >= 0 && idx < prop->enumCount && prop->enumValues[idx])
                        return prop->enumValues[idx];
                }
                break;

            case Deki::PropertyType::AssetRef:
                if (value.is_object())
                {
                    std::string src = value.value("source", "");
                    if (src.empty()) src = value.value("guid", "");
                    if (src.empty()) return "<none>";
                    if (::Deki::Scene::s_GuidToPathResolver)
                    {
                        std::string p = ::Deki::Scene::s_GuidToPathResolver(src);
                        if (!p.empty())
                            return std::filesystem::path(p).filename().string();
                    }
                    return src.size() > 8 ? src.substr(0, 8) + "\xe2\x80\xa6" : src;
                }
                break;

            default:
                break;
        }
    }

    if (value.is_string()) return "\"" + value.get<std::string>() + "\"";
    if (value.is_number_float())
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%g", value.get<double>());
        return buf;
    }
    if (value.is_number()) return std::to_string(value.get<int64_t>());

    return value.dump();
}

}  // anonymous namespace

InspectorPanel::InspectorPanel(EditorApp* app)
    : m_App(app), m_AssetPicker(std::make_unique<AssetPicker>()), m_TextureInspector(std::make_unique<TextureInspector>(app)), m_ComponentManager(std::make_unique<ComponentManager>(app))
{
    // Create subsystems with dependencies
    m_AssetPropertyEditor = std::make_unique<AssetPropertyEditor>(app, m_AssetPicker.get());
    m_PropertyEditor = std::make_unique<PropertyEditorFactory>(app, m_AssetPropertyEditor.get());
    m_AssetTypeInspector = std::make_unique<AssetTypeInspector>(app, m_TextureInspector.get());
}

InspectorPanel::~InspectorPanel() = default;

void InspectorPanel::ClearSceneReferences()
{
    m_DraggedTransformObject = nullptr;
    m_RenamingObject = nullptr;
    m_PropertyEditState = PropertyEditState{};
}

void InspectorPanel::ResetTransientUiState()
{
    ClearSceneReferences();
    if (m_PropertyEditor)
        m_PropertyEditor->ResetTransientState();
}

void InspectorPanel::InvalidatePreview()
{
    m_TextureInspector->InvalidatePreview();
}

void InspectorPanel::Draw()
{
    if (!m_Open)
        return;

    // The filter belongs to the single-object view, which republishes it below
    // once it has drawn its field. Cleared here so the other views (assets,
    // multi-select) are never quietly filtered by a box they do not show.
    InspectorUtils::SetPropertyFilter("");

    ImGui::Begin("INSPECTOR###Inspector", &m_Open, DekiEditor::MaximizedWindowFlags("INSPECTOR###Inspector"));

    // Under-tab toolbar. PIN freezes the inspector on its current target by
    // driving the lock-to-selection state used throughout this function.
    {
        const bool wasPinned = m_Locked;
        DekiEditor::PanelToolbar(&m_Locked);
        if (m_Locked && !wasPinned)
        {
            m_LockedAssetPath = m_App->GetSelectedAssetPath();
            m_LockedAssetGuid = m_App->GetSelectedAssetGuid();
            m_LockedAssetCachePath = m_App->GetSelectedAssetCachePath();
            m_LockedAssetHasCached = m_App->GetSelectedAssetHasCached();
            m_LockedObjects = m_App->GetSelectedObjects();
        }
    }

    // The shared property-list context (body indent, row gap, field padding),
    // the same one package tool windows open, so a row here and a row there are
    // laid out identically. Closed with EndPropertyContext before each End
    // below. bodyIndent is kept for the few places that step outside the indent
    // and back (the asset path strip).
    const float bodyIndent = m_App->Dpi(Metrics::InspectorIndent);
    DekiEditor::BeginPropertyContext();

    // When locked, temporarily override the app selection so subsystems read the locked state
    std::string savedAssetPath, savedCachePath, savedGuid;
    bool savedHasCached = false;
    if (m_Locked && !m_LockedAssetPath.empty())
    {
        savedAssetPath = m_App->GetSelectedAssetPath();
        savedCachePath = m_App->GetSelectedAssetCachePath();
        savedGuid = m_App->GetSelectedAssetGuid();
        savedHasCached = m_App->GetSelectedAssetHasCached();
        m_App->SetSelectedAsset(m_LockedAssetPath, m_LockedAssetCachePath, m_LockedAssetHasCached, m_LockedAssetGuid);
    }

    // Check if an asset is selected (from asset browser or deploy analysis)
    const std::string& selectedAsset = m_App->GetSelectedAssetPath();
    bool isBakedCacheAsset = selectedAsset.empty() && m_App->GetSelectedAssetHasCached() && !m_App->GetSelectedAssetCachePath().empty();
    bool hasAssetSelection = !selectedAsset.empty() || isBakedCacheAsset;

    // Asset header: filename (bold). Pin/lock now lives in the under-tab toolbar.
    if (hasAssetSelection)
    {
        std::string headerName = isBakedCacheAsset
                                     ? m_App->GetSelectedAssetGuid()
                                     : std::filesystem::path(selectedAsset).filename().string();
        if (g_FontUIMed) ImGui::PushFont(g_FontUIMed);
        ImGui::TextUnformatted(headerName.c_str());
        if (g_FontUIMed) ImGui::PopFont();
    }

    if (hasAssetSelection)
    {
        ImGui::Separator();
    }

    if (hasAssetSelection)
    {
        m_AssetTypeInspector->Draw();

        // Restore original selection if locked
        if (m_Locked && !m_LockedAssetPath.empty())
            m_App->SetSelectedAsset(savedAssetPath, savedCachePath, savedHasCached, savedGuid);

        DekiEditor::EndPropertyContext();
        ImGui::End();
        return;
    }

    // Restore if locked but no asset was showing
    if (m_Locked && !m_LockedAssetPath.empty())
        m_App->SetSelectedAsset(savedAssetPath, savedCachePath, savedHasCached, savedGuid);

    // Check if scene objects are selected
    const auto& selectedObjects = m_Locked && !m_LockedObjects.empty() ? m_LockedObjects : m_App->GetSelectedObjects();
    if (selectedObjects.empty())
    {
        // A scene is always loaded (see EditorApp::GetCurrentScene); nothing to
        // show here until an object is selected.
        DekiEditor::EndPropertyContext();
        ImGui::End();
        return;
    }

    // Multi-selection: show unified inspector
    if (selectedObjects.size() > 1)
    {
        DrawMultiObjectInspector(selectedObjects);
        m_PropertyEditor->DrawPendingPopups();
        DekiEditor::EndPropertyContext();
        ImGui::End();
        return;
    }

    // Single selection
    Deki::Object* obj = selectedObjects[0];

    // Schematic selection banner - small stamp + large display-font heading
    // showing `<parent>/<selected>`. Provides at-a-glance context above the
    // existing instance header and component sections.
    {
        // Small left padding (aligns under the PIN bar's icon) instead of flush
        // to the edge. Zero the vertical item spacing so the equal top/bottom
        // padding actually centers the path between the bar and the separator
        // (ImGui otherwise adds an extra ItemSpacing below the text).
        const float pathLeftPad = m_App->Dpi(16.0f);
        ImGui::Unindent(bodyIndent - pathLeftPad);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
        ImGui::Dummy(ImVec2(0, m_App->Dpi(10.0f)));  // top padding

        bool partOfInstance = obj->IsPartOfInstance();
        if (partOfInstance)
        {
            if (g_FontUISmall) ImGui::PushFont(g_FontUISmall);
            ImGui::PushStyleColor(ImGuiCol_Text, Palette::Dim2);
            ImGui::TextUnformatted("Part of scene instance");
            ImGui::PopStyleColor();
            if (g_FontUISmall) ImGui::PopFont();
        }

        Deki::Object* parent = obj->GetParent();
        ImFont* display = g_DisplayFont;  // SpaceGrotesk-Medium (weight 500)
        // Render at an explicit size: the one-arg PushFont keeps the inherited
        // ~13px base instead of the display size, so the heading looked small.
        if (display) ImGui::PushFont(display, DekiEditor::Fonts::CssPx(display, 20.0f));
        if (parent)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, Palette::Fg);
            ImGui::TextUnformatted(parent->GetName().c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine(0, 6);
            ImGui::PushStyleColor(ImGuiCol_Text, Palette::Dim2);
            ImGui::TextUnformatted("/");
            ImGui::PopStyleColor();
            ImGui::SameLine(0, 6);
        }
        // The heading IS the name field: click it to rename, the same gesture a
        // node title takes in the graph editor. A separate "Name" row was a
        // second place to edit what the header was already showing, and the one
        // you were looking at was the one that could not be edited.
        if (m_RenamingObject != obj)
            m_RenamingObject = nullptr;  // a rename never outlives its object

        if (!m_RenamingObject)
        {
            const ImVec2 namePos = ImGui::GetCursorScreenPos();
            const ImVec2 nameSize = ImGui::CalcTextSize(obj->GetName().c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, Palette::Accent);
            ImGui::TextUnformatted(obj->GetName().c_str());
            ImGui::PopStyleColor();
            const ImVec2 afterName = ImGui::GetCursorScreenPos();

            // Hit box over the text, submitted after it so it takes the hover
            // (plain text is not interactive), then the cursor is put back so
            // the row is not counted twice.
            ImGui::SetCursorScreenPos(namePos);
            if (ImGui::InvisibleButton("##obj_rename_hit",
                                       ImVec2(std::max(nameSize.x, 1.0f), std::max(nameSize.y, 1.0f))))
            {
                m_RenamingObject = obj;
                m_RenameOldName = obj->GetName();
                strncpy(m_RenameBuffer, m_RenameOldName.c_str(), sizeof(m_RenameBuffer) - 1);
                m_RenameBuffer[sizeof(m_RenameBuffer) - 1] = 0;
                m_RenameFocusPending = true;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
                // Body font for the hint: the display font is still pushed here
                // and a tooltip inherits it, which would set a one-line hint at
                // heading size.
                if (g_FontUI) ImGui::PushFont(g_FontUI, DekiEditor::Fonts::CssPx(g_FontUI, 13.0f));
                DekiEditor::Tooltip("Click to rename");
                if (g_FontUI) ImGui::PopFont();
            }
            ImGui::SetCursorScreenPos(afterName);
        }
        else
        {
            const float avail = ImGui::GetContentRegionAvail().x - m_App->Dpi(Metrics::InspectorRightPad);
            ImGui::SetNextItemWidth(std::max(avail, m_App->Dpi(80.0f)));
            // No frame padding: the field is exactly as tall as the text it
            // replaces and starts on the same pixel, so opening a rename does
            // not jog the header.
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, Palette::Accent);
            if (m_RenameFocusPending)
            {
                ImGui::SetKeyboardFocusHere();
                m_RenameFocusPending = false;
            }
            if (ImGui::InputText("##obj_rename", m_RenameBuffer, sizeof(m_RenameBuffer)))
                obj->SetName(m_RenameBuffer);  // live, so the hierarchy follows along
            const bool edited = ImGui::IsItemDeactivatedAfterEdit();
            const bool finished = ImGui::IsItemDeactivated();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();

            if (edited)
            {
                const std::string typed = m_RenameBuffer;
                obj->SetName(m_RenameOldName);  // rewind the live preview...
                // ...so the command records the real before and after, and one
                // rename is one undo step. An object with no name cannot be
                // picked out of the hierarchy, so an empty field is treated as
                // "never mind" rather than as a nameless object.
                if (!typed.empty() && typed != m_RenameOldName)
                {
                    CommandHistory::Instance().Execute(
                        std::make_unique<RenameObjectCommand>(obj, typed));
                }
            }
            // Closes on Enter or on clicking anywhere else, typed in or not.
            if (finished)
                m_RenamingObject = nullptr;
        }
        if (display) ImGui::PopFont();

        ImGui::Dummy(ImVec2(0, m_App->Dpi(10.0f)));  // bottom padding
        ImGui::PopStyleVar();
        DekiEditor::FullBleedSeparator();
        ImGui::Indent(bodyIndent - pathLeftPad);  // restore the body indent for the rest
    }

    // Show play mode indicator
    if (m_App->IsPlaying() || m_App->IsPaused())
    {
        ImGui::TextColored(DekiEditor::Palette::Amber, "Play Mode - Changes won't persist");
        ImGui::Separator();
    }

    // Instance header
    bool isInstance = obj->IsSceneInstance();
    bool isPartOfInstance = obj->IsPartOfInstance();

    if (isInstance)
    {
        // Resolve source scene name for display
        std::string sourceName;
        std::string sourcePath;
        if (Deki::Scene::s_GuidToPathResolver)
        {
            sourcePath = Deki::Scene::s_GuidToPathResolver(obj->GetSourceSceneGuid());
            if (!sourcePath.empty())
                sourceName = std::filesystem::path(sourcePath).filename().string();
        }

        ImGui::PushStyleColor(ImGuiCol_Text, DekiEditor::Palette::Accent);
        ImGui::Text("Scene Linked Instance -");
        ImGui::PopStyleColor();

        // Clickable scene name that reveals in asset browser
        if (!sourceName.empty())
        {
            ImGui::SameLine(0, 4);
            ImGui::PushStyleColor(ImGuiCol_Text, DekiEditor::Palette::Accent);
            if (ImGui::SmallButton(sourceName.c_str()))
            {
                auto* pipeline = m_App->GetAssetPipeline();
                auto* browser = m_App->GetAssetBrowserPanel();
                if (pipeline && browser)
                {
                    const AssetInfo* info = pipeline->GetAssetInfoByGuid(obj->GetSourceSceneGuid());
                    if (info)
                        browser->RevealAsset(pipeline->GetAbsolutePath(info->path));
                }
            }
            ImGui::PopStyleColor();
        }

        // Apply / Revert buttons - push instance changes to the source scene
        // asset, or discard them and re-expand from source. Both support undo.
        if (::Deki::Scene* scene = m_App->GetCurrentScene())
        {
            // Apply only makes sense when this instance actually differs from
            // its source. CollectOverrides is expensive (loads source from
            // disk, walks the tree), so cache the result keyed on the object
            // + command history version - recompute only on selection change
            // or after a new command/undo/redo.
            const size_t undoCount = CommandHistory::Instance().GetUndoCount();
            if (obj != m_OverrideCacheObj || undoCount != m_OverrideCacheUndoCount)
            {
                m_OverrideCacheObj = obj;
                m_OverrideCacheUndoCount = undoCount;
                nlohmann::json diff = nlohmann::json::array();
                scene->CollectOverrides(obj, obj, diff);
                m_OverrideCacheHasOverrides = !diff.empty();
            }

            if (m_OverrideCacheHasOverrides)
            {
                if (ImGui::SmallButton("Apply"))
                {
                    // Stash the pending Apply and open a confirmation modal
                    // showing exactly what will be written to the source
                    // scene. Actual execution happens on Confirm; see
                    // DrawApplyConfirmPopup.
                    std::vector<std::string> otherScenePaths;
                    if (auto* pipeline = m_App->GetAssetPipeline())
                    {
                        const auto currentPath = std::filesystem::path(
                                                     m_App->GetCurrentScenePath())
                                                     .lexically_normal();
                        for (const auto& [relPath, info] : pipeline->GetAllAssets())
                        {
                            const auto ext = std::filesystem::path(relPath)
                                                 .extension()
                                                 .string();
                            if (ext != ".scene") continue;
                            std::string abs = pipeline->GetAbsolutePath(info.path);
                            if (abs.empty()) continue;
                            if (std::filesystem::path(abs).lexically_normal() == currentPath)
                                continue;
                            otherScenePaths.push_back(std::move(abs));
                        }
                    }

                    nlohmann::json diff = nlohmann::json::array();
                    scene->CollectOverrides(obj, obj, diff);

                    m_ApplyPopupEntries.clear();
                    for (const auto& entry : diff)
                    {
                        ApplyPopupEntry e;
                        e.raw = entry;
                        e.selected = true;
                        e.path = entry.value("objectPath", "");

                        const nlohmann::json& newVal =
                            entry.contains("value") ? entry["value"] : nlohmann::json();
                        const nlohmann::json& oldVal =
                            entry.contains("sourceValue") ? entry["sourceValue"] : nlohmann::json();

                        if (entry.contains("objectProperty"))
                        {
                            // Object-level override (currently only "active").
                            e.component.clear();
                            e.label = entry["objectProperty"].get<std::string>();
                            e.oldStr = FormatOverrideValue(oldVal, nullptr);
                            e.newStr = FormatOverrideValue(newVal, nullptr);
                        }
                        else
                        {
                            // Override stores the component's serializedName
                            // (GUID, for scene format stability). Resolve it
                            // through the registry so we display the class
                            // name the user recognizes.
                            const std::string compGuid = entry.value("component",
                                                                     std::string());
                            const Deki::ComponentMeta* cmeta =
                                Deki::ComponentRegistry::Instance().GetMeta(compGuid);
                            e.component = cmeta ? cmeta->GetDisplayName() : compGuid;

                            const std::string propName = entry.value("property",
                                                                     std::string());
                            const Deki::PropertyInfo* pi = FindPropertyInfo(compGuid,
                                                                          propName);
                            e.label = pi ? InspectorUtils::GetDisplayName(*pi) : propName;
                            e.oldStr = FormatOverrideValue(oldVal, pi);
                            e.newStr = FormatOverrideValue(newVal, pi);
                        }
                        m_ApplyPopupEntries.push_back(std::move(e));
                    }

                    // Group entries so identical object paths render under one
                    // header, and properties of the same component stay together.
                    std::sort(m_ApplyPopupEntries.begin(), m_ApplyPopupEntries.end(),
                              [](const ApplyPopupEntry& a, const ApplyPopupEntry& b)
                              {
                                  if (a.path != b.path) return a.path < b.path;
                                  if (a.component != b.component) return a.component < b.component;
                                  return a.label < b.label;
                              });

                    std::string sourceName;
                    if (!sourcePath.empty())
                        sourceName = std::filesystem::path(sourcePath).filename().string();
                    if (sourceName.empty())
                        sourceName = "source scene";

                    m_ApplyPopupSourceName = std::move(sourceName);
                    m_ApplyPopupInstance = obj;
                    m_ApplyPopupScene = scene;
                    m_ApplyPopupOtherPaths = std::move(otherScenePaths);
                    m_ApplyPopupOpen = true;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Revert"))
                {
                    CommandHistory::Instance().Execute(
                        std::make_unique<RevertInstanceOverridesCommand>(scene, obj));
                }
            }
        }

        ImGui::Separator();
    }
    else if (isPartOfInstance)
    {
        // Find the instance root for display
        Deki::Object* instanceRoot = obj->GetParent();
        while (instanceRoot && !instanceRoot->IsSceneInstance())
            instanceRoot = instanceRoot->GetParent();

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
        ImGui::Text("Part of Scene Instance%s",
                    instanceRoot ? (": " + instanceRoot->GetName()).c_str() : "");
        ImGui::PopStyleColor();
        ImGui::Separator();
    }

    // Name lives on the header now (click the heading). Active stays here, but
    // as a row like every other boolean in the inspector - label in the left
    // column, box in the value column - instead of a labeled checkbox starting
    // wherever its own text happened to end.
    //
    // It gets its own band between the header hairline and the filter strip, and
    // that band has to pad itself: FullBleedSeparator charges no spacing above a
    // row (a boundary owns no air) while the row charges RowSpacing below, so
    // left to the default flow the row hugs the hairline and the whole band
    // reads top-heavy. Pad it explicitly on both sides with half the row gap
    // each - the band keeps exactly the height it has today, but the air is
    // split evenly, which puts the checkbox on the band's center line.
    const float activeBandPad = m_App->Dpi(Metrics::RowSpacing * 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
    ImGui::Dummy(ImVec2(0, activeBandPad));

    bool active = obj->IsActive();
    InspectorUtils::BeginPropertyRow("Active");
    if (DekiEditor::SchematicCheckbox("##Active", &active))
    {
        // Undoable, and marks the scene dirty, like every other field here.
        CommandHistory::Instance().ExecuteNoMerge(std::make_unique<SetActiveCommand>(obj, active));
    }

    ImGui::Dummy(ImVec2(0, activeBandPad));
    ImGui::PopStyleVar();

    // Property filter. One field over the whole object: on a component with
    // thirty fields, typing three letters beats scrolling, and it spans
    // components so you do not have to know which one owns the property. It
    // takes component names too, so the field answers both questions you arrive
    // with - "where is that property" and "where is that component".
    //
    // Drawn as a band of its own - hairline above, hairline below, no row
    // spacing inside - so it reads as panel chrome the way a toolbar strip
    // does, rather than as one more property row that happens to have a
    // magnifier. Its bottom hairline IS the divider the Transform band hangs
    // from, so that boundary stays one line instead of two.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
    DekiEditor::FullBleedSeparator();
    ImGui::SetCursorPosX(InspectorUtils::SectionAlignedCursorX());
    DekiEditor::SearchField("##inspector_filter", m_PropertyFilter, sizeof(m_PropertyFilter),
                            m_App->Dpi(Metrics::InspectorRightPad), /*bottomBorder*/ true,
                            nullptr, nullptr, nullptr, nullptr,
                            "Filter properties and components");
    InspectorUtils::SetPropertyFilter(m_PropertyFilter);
    ImGui::PopStyleVar();

    // Transform
    DrawTransform(obj);

    DekiEditor::FullBleedSeparator();

    // Components - use ComponentManager with a callback
    auto& registry = Deki::ComponentRegistry::Instance();
    m_ComponentManager->DrawComponentList(obj, [this](Deki::Component* comp, const Deki::ComponentMeta* meta)
                                          { DrawComponent(comp, meta); });

    // A query that matched nothing says so. Without this the panel just ends
    // under the Transform separator, which reads as an object that has lost its
    // components rather than as a filter with no hits.
    if (InspectorUtils::PropertyFilterActive() && InspectorUtils::FilterHits() == 0)
    {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, DekiEditor::Palette::Dim);
        ImGui::TextWrapped("Nothing on this object matches \"%s\".",
                           InspectorUtils::PropertyFilter().c_str());
        ImGui::PopStyleColor();
    }

    // Add Component button - not available for instance objects
    if (!isInstance && !isPartOfInstance)
    {
        ImGui::Spacing();
        // The button's own rect, measured around the call: the add button ends
        // with a zero-size item (so a panel that ends there does not trip
        // ImGui's cursor check), which means GetItemRect* afterwards describes
        // THAT, not the button.
        const ImVec2 addTop = ImGui::GetCursorScreenPos();
        const bool addClicked = DekiEditor::SchematicAddButton(ICON_TI_PLUS "  Add Component");
        const float addBottom = ImGui::GetCursorScreenPos().y;

        // Tell the picker where to hang from, every frame rather than on the
        // click alone, so it has a real anchor on the very first frame it opens
        // - the frame ImGui has no size for it and its own placement has
        // nothing to work from.
        {
            ComponentManager::AddPopupAnchor anchor;
            anchor.x = addTop.x;
            anchor.top = addTop.y;
            anchor.bottom = addBottom;
            const ImGuiViewport* vp = ImGui::GetWindowViewport();
            if (vp)
            {
                anchor.workTop = vp->WorkPos.y;
                anchor.workBottom = vp->WorkPos.y + vp->WorkSize.y;
                anchor.workLeft = vp->WorkPos.x;
                anchor.workRight = vp->WorkPos.x + vp->WorkSize.x;
                anchor.valid = true;
            }
            m_ComponentManager->SetAddPopupAnchor(anchor);
        }

        if (addClicked)
        {
            ImGui::OpenPopup("AddComponentPopup");
        }

        m_ComponentManager->DrawAddComponentMenu(obj);
    }

    m_PropertyEditor->DrawPendingPopups();
    DrawApplyConfirmPopup();
    DekiEditor::EndPropertyContext();
    ImGui::End();
}

void InspectorPanel::ClearApplyPopupState()
{
    m_ApplyPopupOpen = false;
    m_ApplyPopupInstance = nullptr;
    m_ApplyPopupScene = nullptr;
    m_ApplyPopupOtherPaths.clear();
    m_ApplyPopupEntries.clear();
    m_ApplyPopupSourceName.clear();
}

void InspectorPanel::DrawApplyConfirmPopup()
{
    if (m_ApplyPopupOpen)
    {
        ImGui::OpenPopup("Apply to Scene");
        m_ApplyPopupOpen = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Apply to Scene", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        const size_t total = m_ApplyPopupEntries.size();
        const size_t chosen = static_cast<size_t>(std::count_if(
            m_ApplyPopupEntries.begin(), m_ApplyPopupEntries.end(),
            [](const ApplyPopupEntry& e)
            { return e.selected; }));

        ImGui::Text("Applying %zu of %zu change%s to",
                    chosen, total, total == 1 ? "" : "s");
        ImGui::SameLine();
        ImGui::TextColored(DekiEditor::Palette::Accent, "%s",
                           m_ApplyPopupSourceName.c_str());
        ImGui::TextDisabled("Other scenes referencing this scene will be refreshed too.");

        // Select all / Deselect all affordances above the list.
        if (ImGui::SmallButton(ICON_TI_LIST_CHECK "  Select all"))
        {
            for (auto& e : m_ApplyPopupEntries)
                e.selected = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_TI_LIST_DETAILS "  Deselect all"))
        {
            for (auto& e : m_ApplyPopupEntries)
                e.selected = false;
        }
        ImGui::Separator();

        ImGui::BeginChild("apply_changes",
                          ImVec2(720.0f, 460.0f), true);

        if (m_ApplyPopupEntries.empty())
        {
            ImGui::TextDisabled("(no overrides to apply)");
        }
        else
        {
            const ImVec4 pathColor(1.00f, 0.83f, 0.40f, 1.00f);
            const ImVec4 compColor(0.70f, 0.85f, 1.00f, 1.00f);
            const ImVec4 oldColor(0.55f, 0.55f, 0.55f, 1.00f);
            const ImVec4 arrowColor(0.60f, 0.60f, 0.60f, 1.00f);
            const ImVec4 newColor(0.55f, 0.95f, 0.55f, 1.00f);

            std::string prevPath = "<sentinel>\x01";
            std::string prevComp = "<sentinel>\x01";

            for (size_t i = 0; i < m_ApplyPopupEntries.size(); ++i)
            {
                auto& e = m_ApplyPopupEntries[i];

                if (e.path != prevPath)
                {
                    if (i != 0) ImGui::Spacing();
                    const char* pathText = e.path.empty() ? "Root" : e.path.c_str();
                    ImGui::TextColored(pathColor, ICON_TI_BOX "  %s", pathText);
                    prevPath = e.path;
                    prevComp = "<sentinel>\x01";
                }

                if (e.component != prevComp)
                {
                    if (!e.component.empty())
                    {
                        ImGui::Indent();
                        ImGui::TextColored(compColor, ICON_TI_COMPONENTS "  %s",
                                           e.component.c_str());
                        ImGui::Unindent();
                    }
                    prevComp = e.component;
                }

                // Property row:  [checkbox]  label:   oldStr  ->  newStr
                const float indent = e.component.empty() ? 16.0f : 32.0f;
                ImGui::Indent(indent);

                char id[32];
                snprintf(id, sizeof(id), "##apply_%zu", i);
                DekiEditor::SchematicCheckbox(id, &e.selected);
                ImGui::SameLine();

                ImGui::Text("%s:", e.label.c_str());
                ImGui::SameLine(220.0f);
                ImGui::TextColored(oldColor, "%s", e.oldStr.c_str());
                ImGui::SameLine();
                ImGui::TextColored(arrowColor, ICON_TI_ARROW_RIGHT);
                ImGui::SameLine();
                ImGui::TextColored(newColor, "%s", e.newStr.c_str());

                ImGui::Unindent(indent);
            }
        }

        ImGui::EndChild();

        ImGui::Separator();
        ImGui::BeginDisabled(chosen == 0);
        if (ImGui::Button("Confirm", ImVec2(120.0f, 0.0f)))
        {
            if (m_ApplyPopupScene && m_ApplyPopupInstance &&
                m_ApplyPopupInstance->IsSceneInstance())
            {
                nlohmann::json selected = nlohmann::json::array();
                for (const auto& e : m_ApplyPopupEntries)
                    if (e.selected) selected.push_back(e.raw);

                CommandHistory::Instance().Execute(
                    std::make_unique<ApplyInstanceToSceneCommand>(
                        m_ApplyPopupScene,
                        m_ApplyPopupInstance,
                        std::move(m_ApplyPopupOtherPaths),
                        std::move(selected)));
            }
            ClearApplyPopupState();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
        {
            ClearApplyPopupState();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void InspectorPanel::DrawTransform(Deki::Object* obj)
{
    // Transform draws its rows by hand rather than through DrawProperty, so the
    // filter is applied here: with one typed, the section appears only when one
    // of its three fields - or its own name, as for any other component - is
    // what you are looking for.
    const bool filtering = InspectorUtils::PropertyFilterActive();

    // Filter cleared: put the section back the way it was found.
    if (!filtering && m_TransformStateKnown)
    {
        ImGui::GetStateStorage()->SetInt(static_cast<ImGuiID>(m_TransformHeaderId),
                                         m_TransformPreFilterOpen ? 1 : 0);
        m_TransformStateKnown = false;
    }

    if (filtering &&
        !InspectorUtils::TextMatchesFilter("Transform") &&
        !InspectorUtils::PropertyMatchesFilter("Position") &&
        !InspectorUtils::PropertyMatchesFilter("Rotation") &&
        !InspectorUtils::PropertyMatchesFilter("Scale"))
    {
        return;
    }
    if (filtering)
        InspectorUtils::NoteFilterHit();

    // Matched under a filter, so it is shown open - a hit behind a collapsed
    // header reads as no hit. State recorded the first frame, forced after.
    if (filtering && m_TransformStateKnown)
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);

    // Section, not a bare header: the body slides shut instead of vanishing.
    // Note the flag is the SECTION's, not the header's - it stays true while the
    // section is closing, which is when the body still has to be submitted.
    ImGuiID transformHeaderId = 0;
    const bool transformBody =
        SchematicSectionBegin("Transform", ImGuiTreeNodeFlags_DefaultOpen, &transformHeaderId);
    if (filtering && !m_TransformStateKnown && transformHeaderId != 0)
    {
        m_TransformHeaderId = static_cast<unsigned int>(transformHeaderId);
        m_TransformPreFilterOpen = ImGui::GetStateStorage()->GetInt(transformHeaderId, 1) != 0;
        m_TransformStateKnown = true;
    }
    if (transformBody)
    {
        // Context menu for Transform
        PushContextMenuPopupStyle();
        if (ImGui::BeginPopupContextItem("TransformContextMenu"))
        {
            PushContextMenuItemStyle();
            auto& clipboard = ClipboardService::Instance();

            if (ImGui::MenuItem("Copy Transform"))
            {
                clipboard.CopyTransform(obj);
            }

            bool canPaste = clipboard.HasTransform();
            if (ImGui::MenuItem("Paste Transform", nullptr, false, canPaste))
            {
                auto transformOpt = clipboard.GetTransformData();
                if (transformOpt.has_value())
                {
                    const auto& selectedObjects = m_App->GetSelectedObjects();
                    std::vector<Deki::Object*> targets(selectedObjects.begin(), selectedObjects.end());

                    CommandHistory::Instance().Execute(
                        std::make_unique<PasteTransformCommand>(targets, transformOpt.value()));
                }
            }

            ImGui::PopStyleVar();  // context menu ItemSpacing
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();  // context menu WindowPadding

        // Transform through the shared EditorUI fields in VALUE mode: pass the
        // current value (no pointer to obj's x/y), apply via callbacks. The field
        // records undo itself; onChange/onUndo/onRedo call the proper setter (which
        // also marks the transform dirty so children follow), onStartChange/
        // onEndChange bracket the live edit. Unit::Distance handles px/m + snapping,
        // Unit::Angle handles degrees<->radians.
        const char* posLabel = obj->GetParent() ? "Local Position" : "Position";
        {
            EditorUI::VecCallbacks cb;
            cb.onStartChange = [this](const float*)
            { BeginLiveEdit(); };
            cb.onChange = [obj](const float* v)
            { obj->SetLocalPosition(v[0], v[1]); };
            cb.onUndo = [obj](const float* v)
            { obj->SetLocalPosition(v[0], v[1]); };
            cb.onRedo = [obj](const float* v)
            { obj->SetLocalPosition(v[0], v[1]); };
            cb.onEndChange = [this](const float*, const float*)
            { EndLiveEdit(); };
            EditorUI::Get().Vector2Field(posLabel, obj->GetX(), obj->GetY(), EditorUI::Unit::Distance, cb);
        }
        {
            EditorUI::FloatCallbacks cb;
            cb.onStartChange = [this](float)
            { BeginLiveEdit(); };
            cb.onChange = [obj](float v)
            { obj->SetRotation(v); };  // radians (Unit::Angle shows degrees)
            cb.onUndo = [obj](float v)
            { obj->SetRotation(v); };
            cb.onRedo = [obj](float v)
            { obj->SetRotation(v); };
            cb.onEndChange = [this](float, float)
            { EndLiveEdit(); };
            EditorUI::Get().FloatField("Rotation", obj->GetLocalRotation(), EditorUI::Unit::Angle, cb);
        }
        {
            EditorUI::VecCallbacks cb;
            cb.onStartChange = [this](const float*)
            { BeginLiveEdit(); };
            cb.onChange = [obj](const float* v)
            { obj->SetScale(v[0], v[1]); };
            cb.onUndo = [obj](const float* v)
            { obj->SetScale(v[0], v[1]); };
            cb.onRedo = [obj](const float* v)
            { obj->SetScale(v[0], v[1]); };
            cb.onEndChange = [this](const float*, const float*)
            { EndLiveEdit(); };
            EditorUI::Get().Vector2Field("Scale", obj->GetScaleX(), obj->GetScaleY(),
                                         0.01f, 0.01f, 10.0f, "%.3f", cb);
        }
#ifdef DEKI_TRANSFORM_3D
        // The third axis, only when the project declares transform_3d: a 2D
        // device drops these on load, so offering them to a 2D project would
        // be offering something the board will not honour.
        if (m_App->ProjectDeclaresDepth())
        {
            {
                EditorUI::FloatCallbacks cb;
                cb.onStartChange = [this](float)
                { BeginLiveEdit(); };
                cb.onChange = [obj](float v)
                { obj->SetZ(v); };
                cb.onUndo = [obj](float v)
                { obj->SetZ(v); };
                cb.onRedo = [obj](float v)
                { obj->SetZ(v); };
                cb.onEndChange = [this](float, float)
                { EndLiveEdit(); };
                EditorUI::Get().FloatField("Z", obj->GetZ(), EditorUI::Unit::Distance, cb);
            }
            {
                EditorUI::FloatCallbacks cb;
                cb.onStartChange = [this](float)
                { BeginLiveEdit(); };
                cb.onChange = [obj](float v)
                { obj->SetRotationX(v); };
                cb.onUndo = [obj](float v)
                { obj->SetRotationX(v); };
                cb.onRedo = [obj](float v)
                { obj->SetRotationX(v); };
                cb.onEndChange = [this](float, float)
                { EndLiveEdit(); };
                EditorUI::Get().FloatField("Rotation X", obj->GetRotationX(), EditorUI::Unit::Angle, cb);
            }
            {
                EditorUI::FloatCallbacks cb;
                cb.onStartChange = [this](float)
                { BeginLiveEdit(); };
                cb.onChange = [obj](float v)
                { obj->SetRotationY(v); };
                cb.onUndo = [obj](float v)
                { obj->SetRotationY(v); };
                cb.onRedo = [obj](float v)
                { obj->SetRotationY(v); };
                cb.onEndChange = [this](float, float)
                { EndLiveEdit(); };
                EditorUI::Get().FloatField("Rotation Y", obj->GetRotationY(), EditorUI::Unit::Angle, cb);
            }
            {
                EditorUI::FloatCallbacks cb;
                cb.onStartChange = [this](float)
                { BeginLiveEdit(); };
                cb.onChange = [obj](float v)
                { obj->SetScaleZ(v); };
                cb.onUndo = [obj](float v)
                { obj->SetScaleZ(v); };
                cb.onRedo = [obj](float v)
                { obj->SetScaleZ(v); };
                cb.onEndChange = [this](float, float)
                { EndLiveEdit(); };
                EditorUI::Get().FloatField("Scale Z", obj->GetScaleZ(), 0.01f, 0.01f, 10.0f, "%.3f", cb);
            }
        }
#endif
        SchematicSectionEnd();
    }
}

void InspectorPanel::DrawComponent(Deki::Component* comp, const Deki::ComponentMeta* meta)
{
    if (!meta)
    {
        ImGui::TextDisabled("(No properties)");
        return;
    }

    auto& registry = Deki::ComponentRegistry::Instance();

    // First, check CustomEditorRegistry (new API using EditorUI singleton)
    // Use GetEditorForComponent to get a per-component instance with fresh state
    auto* customEditor = CustomEditorRegistry::Instance().GetEditorForComponent(meta->name, comp);
    bool wantsOverride = false;
    if (customEditor)
    {
        DekiEditor::Guard(meta->name, [&]
                          { wantsOverride = customEditor->WantsInspectorOverride(comp); });
    }
    if (customEditor && wantsOverride)
    {
        // Set up EditorUI singleton state for this component
        // Parameter order: drawByName, drawByOffset, drawDefault, resolveAsset, getFontAtlas
        EditorUI::Get().BeginComponent(comp, meta,
                                       // DrawPropertyByName callback
                                       [this, comp, meta, &registry](const char* name)
                                       {
                // Search this component's properties first
                for (int i = 0; i < meta->propertyCount; ++i)
                {
                    if (std::strcmp(meta->properties[i].name, name) == 0)
                    {
                        m_PropertyEditor->DrawProperty(comp, meta->properties[i]);
                        return;
                    }
                }
                // Search base class properties
                if (meta->baseTypeId != 0)
                {
                    const Deki::ComponentMeta* baseMeta = registry.GetMeta(meta->baseTypeId);
                    if (baseMeta)
                    {
                        for (int i = 0; i < baseMeta->propertyCount; ++i)
                        {
                            if (std::strcmp(baseMeta->properties[i].name, name) == 0)
                            {
                                m_PropertyEditor->DrawProperty(comp, baseMeta->properties[i]);
                                return;
                            }
                        }
                    }
                } },
                                       // DrawPropertyByOffset callback
                                       [this, comp, meta](size_t offset)
                                       {
                for (int i = 0; i < meta->propertyCount; ++i)
                {
                    if (meta->properties[i].offset == offset)
                    {
                        m_PropertyEditor->DrawProperty(comp, meta->properties[i]);
                        break;
                    }
                } },
                                       // DrawDefaultProperties callback
                                       [this, comp, meta, &registry]()
                                       {
                // Draw base class properties first. DEKI_VISIBLE_WHEN applies
                // here as in the plain inspector: a custom editor that draws
                // the defaults gets the same fields shown and hidden.
                if (meta->baseTypeId != 0)
                {
                    const Deki::ComponentMeta* baseMeta = registry.GetMeta(meta->baseTypeId);
                    if (baseMeta && baseMeta->propertyCount > 0)
                    {
                        for (int i = 0; i < baseMeta->propertyCount; ++i)
                        {
                            if (InspectorUtils::EvaluateVisibility(comp, baseMeta->properties[i], baseMeta))
                                m_PropertyEditor->DrawProperty(comp, baseMeta->properties[i]);
                        }
                    }
                }
                // Draw this component's properties
                for (int i = 0; i < meta->propertyCount; ++i)
                {
                    if (InspectorUtils::EvaluateVisibility(comp, meta->properties[i], meta))
                        m_PropertyEditor->DrawProperty(comp, meta->properties[i]);
                } },
                                       // ResolveAssetName callback
                                       [this](const std::string& guid) -> std::string
                                       {
                auto* pipeline = m_App->GetAssetPipeline();
                if (pipeline) {
                    const AssetInfo* info = pipeline->GetAssetInfoByGuid(guid);
                    if (info) return info->path;
                }
                return guid; },
                                       // GetFontAtlasTexture callback (uses on-demand font compilation via EditorAssets)
                                       [](const std::string& fontGuid, int fontSize, uint32_t* outWidth, uint32_t* outHeight) -> uint32_t
                                       { return EditorAssets::Get()->LoadFontAtlasWithBaking(fontGuid, fontSize, outWidth, outHeight); },
                                       // ModifyAsset callback
                                       [this](const std::string& assetPath, const std::string& assetGuid, const std::string& oldJson, const std::string& newJson)
                                       {
                try {
                    // Parse JSON strings
                    nlohmann::json oldData = nlohmann::json::parse(oldJson);
                    nlohmann::json newData = nlohmann::json::parse(newJson);

                    // Create and execute command with callback for asset reimport
                    auto command = std::make_unique<ModifyAssetCommand>(
                        assetPath, assetGuid, oldData, newData,
                        [this, assetPath, assetGuid]() {
                            // Trigger asset reimport (recompiles DTEX on disk)
                            AssetDatabase::ImportAsset(AssetDatabase::GUIDToAssetPath(assetGuid));

                            // Clear component AssetRef pointers first (prevents dangling pointers)
                            m_App->GetSceneViewPanel()->InvalidateAllAssets();

                            // Evict stale sprite from engine cache so reload picks up new file
                            Deki::AssetManager::Get()->InvalidateAsset("Sprite", assetGuid);

                            // Invalidate GPU texture caches
                            EditorAssets::Get()->InvalidateTexture(assetGuid);
                            m_App->GetSceneViewPanel()->InvalidateTexture(assetGuid);
                        }
                    );

                    CommandHistory::Instance().Execute(std::move(command));
                } catch (...) {
                    // Silently ignore JSON parse errors
                } });

        // Call the custom editor - uses EditorUI::Get() for drawing.
        // Guarded so a faulty CustomEditor (e.g. stale package-DLL state)
        // doesn't terminate the editor; EndComponent still runs to keep
        // the EditorUI singleton state balanced.
        DekiEditor::Guard(meta->name, [&]
                          { customEditor->OnInspectorGUI(comp); });

        EditorUI::Get().EndComponent();
        return;
    }

    // Draw base class properties first (e.g., sortingOrder from RendererComponent)
    if (meta->baseTypeId != 0)
    {
        const Deki::ComponentMeta* baseMeta = registry.GetMeta(meta->baseTypeId);
        if (baseMeta && baseMeta->propertyCount > 0)
        {
            for (int i = 0; i < baseMeta->propertyCount; ++i)
            {
                m_PropertyEditor->DrawProperty(comp, baseMeta->properties[i]);
            }
        }
    }

    if (meta->propertyCount == 0)
    {
        return;
    }

    // Draw all properties with conditional visibility based on metadata
    const char* currentGroup = nullptr;
    for (int i = 0; i < meta->propertyCount; ++i)
    {
        const Deki::PropertyInfo& prop = meta->properties[i];

        // Check visibility condition from metadata
        if (!InspectorUtils::EvaluateVisibility(comp, prop, meta))
        {
            continue;
        }

        // Render group header when group changes
        if (prop.group && (!currentGroup || strcmp(currentGroup, prop.group) != 0))
        {
            // On the same left edge as the rows it titles, in the theme's
            // sub-header treatment (see SchematicSubHeader): one rung below the
            // component's own band, so the hierarchy reads from the type alone.
            ImGui::Spacing();
            ImGui::SetCursorPosX(InspectorUtils::SectionAlignedCursorX());
            DekiEditor::SchematicSubHeader(prop.group);
            currentGroup = prop.group;
        }
        else if (!prop.group && currentGroup)
        {
            currentGroup = nullptr;
        }

        m_PropertyEditor->DrawProperty(comp, prop);
    }

    // Package-specific inspector extensions (e.g., tween duration display)
    // are handled by the packages themselves via custom inspector registration.
}

void InspectorPanel::DrawMultiObjectInspector(const std::vector<Deki::Object*>& objects)
{
    // Header showing selection count
    ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f),
                       "%zu Objects Selected", objects.size());
    ImGui::Separator();

    // Play mode indicator
    if (m_App->IsPlaying() || m_App->IsPaused())
    {
        ImGui::TextColored(DekiEditor::Palette::Amber,
                           "Play Mode - Changes won't persist");
        ImGui::Separator();
    }

    // Transform section for multi-selection
    if (SchematicSectionBegin("Transform"))
    {
        // Context menu
        PushContextMenuPopupStyle();
        if (ImGui::BeginPopupContextItem("TransformContextMenuMulti"))
        {
            PushContextMenuItemStyle();
            auto& clipboard = ClipboardService::Instance();

            if (ImGui::MenuItem("Copy Transform"))
            {
                clipboard.CopyTransform(objects[0]);
            }

            bool canPaste = clipboard.HasTransform();
            if (ImGui::MenuItem("Paste Transform", nullptr, false, canPaste))
            {
                auto transformOpt = clipboard.GetTransformData();
                if (transformOpt.has_value())
                {
                    std::vector<Deki::Object*> targets(objects.begin(), objects.end());
                    CommandHistory::Instance().Execute(
                        std::make_unique<PasteTransformCommand>(targets, transformOpt.value()));
                }
            }

            ImGui::PopStyleVar();  // context menu ItemSpacing
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();  // context menu WindowPadding

        ImGui::TextDisabled("(Multiple objects - individual editing not shown)");
        SchematicSectionEnd();
    }

    ImGui::Separator();

    // Find common components across all selected objects
    auto commonComponents = GetCommonComponents(objects);

    if (commonComponents.empty())
    {
        ImGui::TextDisabled("No common components");
        return;
    }

    // Draw each common component type
    for (const auto& group : commonComponents)
    {
        DrawMultiComponentSection(group);
    }
}

std::vector<InspectorPanel::ComponentGroup> InspectorPanel::GetCommonComponents(
    const std::vector<Deki::Object*>& objects)
{
    std::vector<ComponentGroup> result;
    if (objects.empty())
        return result;

    auto& registry = Deki::ComponentRegistry::Instance();

    // Build a map of component type name -> list of components from first object
    std::map<std::string, ComponentGroup> componentMap;

    // Initialize with first object's components
    for (Deki::Component* comp : objects[0]->GetComponents())
    {
        const Deki::ComponentMeta* meta = registry.GetMeta(comp->GetType());
        if (meta && meta->name)
        {
            std::string typeName = meta->name;
            if (componentMap.find(typeName) == componentMap.end())
            {
                componentMap[typeName] = ComponentGroup{ typeName, meta, {} };
            }
            componentMap[typeName].components.push_back(comp);
        }
    }

    // Check remaining objects - only keep types present in all
    for (size_t i = 1; i < objects.size(); ++i)
    {
        std::set<std::string> presentTypes;

        for (Deki::Component* comp : objects[i]->GetComponents())
        {
            const Deki::ComponentMeta* meta = registry.GetMeta(comp->GetType());
            if (meta && meta->name)
            {
                std::string typeName = meta->name;
                auto it = componentMap.find(typeName);
                if (it != componentMap.end())
                {
                    it->second.components.push_back(comp);
                    presentTypes.insert(typeName);
                }
            }
        }

        // Remove types not present in this object
        for (auto it = componentMap.begin(); it != componentMap.end();)
        {
            if (presentTypes.find(it->first) == presentTypes.end())
                it = componentMap.erase(it);
            else
                ++it;
        }
    }

    // Convert to result format - only include types with exactly objects.size() components
    for (auto& [typeName, group] : componentMap)
    {
        if (group.components.size() == objects.size())
        {
            result.push_back(std::move(group));
        }
    }

    return result;
}

void InspectorPanel::DrawMultiComponentSection(const ComponentGroup& group)
{
    if (!group.meta)
        return;

    const bool open = SchematicSectionBegin(group.typeName.c_str(),
                                            ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);

    // Right-aligned `EDIT ->` action link for package/project components.
    if (group.meta && !group.components.empty() && ComponentManager::IsEditableComponent(m_App, group.meta->typeId))
    {
        if (SchematicActionLink(("EDIT##multi_comp_edit_" + group.typeName).c_str()))
        {
            ComponentManager::OpenComponentSourceFile(m_App, group.meta);
        }
    }

    if (!open)
        return;

    // Draw base class properties first
    auto& registry = Deki::ComponentRegistry::Instance();
    if (group.meta->baseTypeId != 0)
    {
        const Deki::ComponentMeta* baseMeta = registry.GetMeta(group.meta->baseTypeId);
        if (baseMeta && baseMeta->propertyCount > 0)
        {
            for (int i = 0; i < baseMeta->propertyCount; ++i)
            {
                m_PropertyEditor->DrawPropertyMulti(group.components, baseMeta->properties[i]);
            }
        }
    }

    // Draw own properties
    const char* currentGroup = nullptr;
    for (int i = 0; i < group.meta->propertyCount; ++i)
    {
        const Deki::PropertyInfo& prop = group.meta->properties[i];

        // Check visibility (use first component for visibility checks)
        if (!InspectorUtils::EvaluateVisibility(group.components[0], prop, group.meta))
        {
            continue;
        }

        // Render group header when group changes
        if (prop.group && (!currentGroup || strcmp(currentGroup, prop.group) != 0))
        {
            // On the same left edge as the rows it titles, in the theme's
            // sub-header treatment (see SchematicSubHeader): one rung below the
            // component's own band, so the hierarchy reads from the type alone.
            ImGui::Spacing();
            ImGui::SetCursorPosX(InspectorUtils::SectionAlignedCursorX());
            DekiEditor::SchematicSubHeader(prop.group);
            currentGroup = prop.group;
        }
        else if (!prop.group && currentGroup)
        {
            currentGroup = nullptr;
        }

        // Use PropertyEditorFactory's multi-edit method
        m_PropertyEditor->DrawPropertyMulti(group.components, prop);
    }

    SchematicSectionEnd();
}

}  // namespace DekiEditor
