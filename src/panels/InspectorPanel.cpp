#include "InspectorPanel.h"
#include "core/commands/HistoryGuards.h"
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
#include "../services/AgentCursor.h"
#include "../services/ClipboardService.h"
#include "../services/SafeDispatch.h"
#include "AssetBrowserPanel.h"
#include <deki-editor/AssetPipeline.h>
#include <deki-editor/CommandHistory.h>
#include <deki-editor/Localization.h>
#include "../core/Commands.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <functional>
#include <unordered_set>
#include <cstring>
#include <filesystem>
#include <set>

// Engine includes
#include <deki/Object.h>
#include <deki/ScreenAnchor.h>
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
const Deki::PropertyInfo* FindPropertyInfo(const std::string& componentName, const std::string& propertyName)
{
    if (componentName.empty() || propertyName.empty())
    {
        return nullptr;
    }

    auto& registry = Deki::ComponentRegistry::Instance();
    const Deki::ComponentMeta* meta = registry.GetMeta(componentName);
    while (meta)
    {
        for (int i = 0; i < meta->propertyCount; ++i)
        {
            const Deki::PropertyInfo& prop = meta->properties[i];
            if (prop.name && propertyName == prop.name)
            {
                return &prop;
            }
        }
        meta = (meta->baseTypeId != 0) ? registry.GetMeta(meta->baseTypeId) : nullptr;
    }
    return nullptr;
}

// Format a JSON value for display, using the property's type to pick a
// friendly representation (hex for colors, enum names, true/false, asset
// source label, etc.). Falls back to json.dump() for unknown shapes.
std::string FormatOverrideValue(const nlohmann::json& value, const Deki::PropertyInfo* prop)
{
    if (value.is_null())
    {
        return "null";
    }
    if (value.is_boolean())
    {
        return value.get<bool>() ? "true" : "false";
    }

    if (prop)
    {
        switch (prop->type)
        {
            case Deki::PropertyType::Color:
            case Deki::PropertyType::UInt32:
                if (value.is_number_unsigned())
                {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "#%08X", value.get<uint32_t>());
                    return buf;
                }
                break;

            case Deki::PropertyType::Enum:
                if (value.is_number_integer() && prop->enumValues && prop->enumCount > 0)
                {
                    int idx = value.get<int>();
                    if (idx >= 0 && idx < prop->enumCount && prop->enumValues[idx])
                    {
                        return prop->enumValues[idx];
                    }
                }
                break;

            case Deki::PropertyType::AssetRef:
                if (value.is_object())
                {
                    std::string src = value.value("source", "");
                    if (src.empty())
                    {
                        src = value.value("guid", "");
                    }
                    if (src.empty())
                    {
                        return Tr("<none>");
                    }
                    if (::Deki::Scene::s_GuidToPathResolver)
                    {
                        std::string p = ::Deki::Scene::s_GuidToPathResolver(src);
                        if (!p.empty())
                        {
                            return std::filesystem::path(p).filename().string();
                        }
                    }
                    return src.size() > 8 ? src.substr(0, 8) + "\xe2\x80\xa6" : src;
                }
                break;

            default: break;
        }
    }

    if (value.is_string())
    {
        return "\"" + value.get<std::string>() + "\"";
    }
    if (value.is_number_float())
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%g", value.get<double>());
        return buf;
    }
    if (value.is_number())
    {
        return std::to_string(value.get<int64_t>());
    }

    return value.dump();
}

}  // anonymous namespace

InspectorPanel::InspectorPanel(EditorApp* app)
    : m_App(app),
      m_AssetPicker(std::make_unique<AssetPicker>()),
      m_TextureInspector(std::make_unique<TextureInspector>(app)),
      m_ComponentManager(std::make_unique<ComponentManager>(app))
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

    // A pin keeps only the objects the current scene still has: the scene
    // changed (open, new, reload, Stop), and a pinned object of the old one
    // would be drawn from freed memory next frame. Matched by address only,
    // so nothing stale is dereferenced.
    if (!m_LockedObjects.empty())
    {
        std::unordered_set<const Deki::Object*> live;
        if (Deki::Scene* scene = m_App->GetCurrentScene())
        {
            std::function<void(const Deki::Object*)> collect = [&](const Deki::Object* o)
            {
                live.insert(o);
                for (const Deki::Object* c : o->GetChildren())
                {
                    collect(c);
                }
            };
            for (const Deki::Object* root : scene->GetObjects())
            {
                collect(root);
            }
        }
        std::erase_if(m_LockedObjects, [&](Deki::Object* o) { return !live.contains(o); });
        if (m_LockedObjects.empty())
        {
            locked = false;
        }
    }
}

void InspectorPanel::OnObjectsDestroyed(const std::vector<Deki::Object*>& dying)
{
    if (m_LockedObjects.empty())
    {
        return;
    }
    std::erase_if(m_LockedObjects,
                  [&](Deki::Object* o) { return std::find(dying.begin(), dying.end(), o) != dying.end(); });
    if (m_LockedObjects.empty())
    {
        locked = false;
    }
}

void InspectorPanel::ResetTransientUiState()
{
    ClearSceneReferences();
    if (m_PropertyEditor)
    {
        m_PropertyEditor->ResetTransientState();
    }
}

void InspectorPanel::InvalidatePreview()
{
    m_TextureInspector->InvalidatePreview();
}

void InspectorPanel::Draw()
{
    // A live edit only lasts while a field is held. One left open with nothing
    // held (its field vanished mid-edit: object deleted, panel closed) is
    // closed here, or the Play View would re-render every frame for good.
    if (m_LiveEditCount > 0 && ImGui::GetActiveID() == 0)
    {
        while (m_LiveEditCount > 0)
        {
            EndLiveEdit();
        }
    }

    if (!isOpen)
    {
        return;
    }

    // The filter belongs to the single-object view, which republishes it below
    // once it has drawn its field. Cleared here so the other views (assets,
    // multi-select) are never quietly filtered by a box they do not show.
    InspectorUtils::SetPropertyFilter("");

    ImGui::Begin(TrLabel("INSPECTOR###Inspector"), &isOpen, DekiEditor::MaximizedWindowFlags("INSPECTOR###Inspector"));

    // Under-tab toolbar. PIN freezes the inspector on its current target by
    // driving the lock-to-selection state used throughout this function.
    {
        const bool wasPinned = locked;
        DekiEditor::PanelToolbar(&locked);
        if (locked && !wasPinned)
        {
            m_LockedAssetPath = m_App->GetSelectedAssetPath();
            m_LockedAssetGuid = m_App->GetSelectedAssetGuid();
            m_LockedAssetCachePath = m_App->GetSelectedAssetCachePath();
            m_LockedAssetHasCached = m_App->GetSelectedAssetHasCached();
            m_LockedObjects = m_App->GetSelectedObjects();
        }
    }

    // The shared property-list context (body indent, row gap, field padding),
    // the one package tool windows open too, so rows lay out the same in both.
    // Closed with EndPropertyContext before each End below. bodyIndent is for
    // the few places that step outside the indent and back (the asset path
    // strip).
    const float bodyIndent = m_App->Dpi(Metrics::kInspectorIndent);
    DekiEditor::BeginPropertyContext();

    // When locked, temporarily override the app selection so subsystems read the locked state
    std::string savedAssetPath, savedCachePath, savedGuid;
    bool savedHasCached = false;
    if (locked && !m_LockedAssetPath.empty())
    {
        savedAssetPath = m_App->GetSelectedAssetPath();
        savedCachePath = m_App->GetSelectedAssetCachePath();
        savedGuid = m_App->GetSelectedAssetGuid();
        savedHasCached = m_App->GetSelectedAssetHasCached();
        m_App->SetSelectedAsset(m_LockedAssetPath, m_LockedAssetCachePath, m_LockedAssetHasCached, m_LockedAssetGuid);
    }

    // Check if an asset is selected (from asset browser or deploy analysis)
    const std::string& selectedAsset = m_App->GetSelectedAssetPath();
    bool isBakedCacheAsset =
        selectedAsset.empty() && m_App->GetSelectedAssetHasCached() && !m_App->GetSelectedAssetCachePath().empty();
    bool hasAssetSelection = !selectedAsset.empty() || isBakedCacheAsset;

    // Asset header: the file name as a title ("dirt_a.png" reads "Dirt A"),
    // set like the object heading below. Pin/lock lives in the under-tab toolbar.
    if (hasAssetSelection)
    {
        const std::string headerName =
            isBakedCacheAsset
                ? m_App->GetSelectedAssetGuid()
                : InspectorUtils::NicifyName(std::filesystem::path(selectedAsset).stem().string().c_str());
        const float pathLeftPad = m_App->Dpi(16.0f);
        ImGui::Unindent(bodyIndent - pathLeftPad);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
        ImGui::Dummy(ImVec2(0, m_App->Dpi(10.0f)));  // top padding
        ImFont* display = g_FontDisplay;
        if (display)
        {
            ImGui::PushFont(display, DekiEditor::Fonts::CssPx(display, 20.0f));
        }
        ImGui::PushStyleColor(ImGuiCol_Text, Palette::Accent);
        ImGui::TextUnformatted(headerName.c_str());
        ImGui::PopStyleColor();
        if (display)
        {
            ImGui::PopFont();
        }
        ImGui::Dummy(ImVec2(0, m_App->Dpi(10.0f)));  // bottom padding
        ImGui::PopStyleVar();
        DekiEditor::FullBleedSeparator();
        ImGui::Indent(bodyIndent - pathLeftPad);
    }

    if (hasAssetSelection)
    {
        m_AssetTypeInspector->Draw();

        // Restore original selection if locked
        if (locked && !m_LockedAssetPath.empty())
        {
            m_App->SetSelectedAsset(savedAssetPath, savedCachePath, savedHasCached, savedGuid);
        }

        DekiEditor::EndPropertyContext();
        ImGui::End();
        return;
    }

    // Restore if locked but no asset was showing
    if (locked && !m_LockedAssetPath.empty())
    {
        m_App->SetSelectedAsset(savedAssetPath, savedCachePath, savedHasCached, savedGuid);
    }

    // Check if scene objects are selected
    const auto& selectedObjects = locked && !m_LockedObjects.empty() ? m_LockedObjects : m_App->GetSelectedObjects();
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

    // Selection banner: a small stamp and a large display-font heading
    // showing `<parent>/<selected>`, above the instance header and the
    // component sections.
    {
        // Small left padding (aligns under the PIN bar's icon) instead of flush
        // to the edge. Zero the vertical item spacing so the equal top/bottom
        // padding actually centers the path between the bar and the separator
        // (ImGui otherwise adds an extra ItemSpacing below the text).
        const float pathLeftPad = m_App->Dpi(16.0f);
        ImGui::Unindent(bodyIndent - pathLeftPad);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
        ImGui::Dummy(ImVec2(0, m_App->Dpi(10.0f)));  // top padding

        bool partOfInstance = obj->IsPartOfInstance();
        if (partOfInstance)
        {
            if (g_FontUISmall)
            {
                ImGui::PushFont(g_FontUISmall);
            }
            ImGui::PushStyleColor(ImGuiCol_Text, Palette::Dim2);
            ImGui::TextUnformatted(Tr("Part of scene instance"));
            ImGui::PopStyleColor();
            if (g_FontUISmall)
            {
                ImGui::PopFont();
            }
        }

        Deki::Object* parent = obj->GetParent();
        ImFont* display = g_FontDisplay;  // SpaceGrotesk-Medium (weight 500)
        // Render at an explicit size: the one-argument PushFont keeps the
        // inherited ~13px base size, not the display size.
        if (display)
        {
            ImGui::PushFont(display, DekiEditor::Fonts::CssPx(display, 20.0f));
        }
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
        // The heading is the name field: click it to rename, as with a node
        // title in the graph editor.
        if (m_RenamingObject != obj)
        {
            m_RenamingObject = nullptr;  // a rename never outlives its object
        }

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
                if (g_FontUI)
                {
                    ImGui::PushFont(g_FontUI, DekiEditor::Fonts::CssPx(g_FontUI, 13.0f));
                }
                DekiEditor::Tooltip("Click to rename");
                if (g_FontUI)
                {
                    ImGui::PopFont();
                }
            }
            ImGui::SetCursorScreenPos(afterName);
        }
        else
        {
            const float avail = ImGui::GetContentRegionAvail().x - m_App->Dpi(Metrics::kInspectorRightPad);
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
            {
                obj->SetName(m_RenameBuffer);  // live, so the hierarchy follows along
            }
            const bool edited = ImGui::IsItemDeactivatedAfterEdit();
            const bool finished = ImGui::IsItemDeactivated();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();

            if (edited)
            {
                const std::string typed = m_RenameBuffer;
                obj->SetName(m_RenameOldName);  // rewind the live preview...
                // ...so the command records the real before and after, and one
                // rename is one undo step. An empty field means "never mind":
                // an object with no name cannot be picked out in the hierarchy.
                if (!typed.empty() && typed != m_RenameOldName)
                {
                    CommandHistory::Instance().Execute(std::make_unique<RenameObjectCommand>(obj, typed));
                }
            }
            // Closes on Enter or on clicking anywhere else, typed in or not.
            if (finished)
            {
                m_RenamingObject = nullptr;
            }
        }
        if (display)
        {
            ImGui::PopFont();
        }

        ImGui::Dummy(ImVec2(0, m_App->Dpi(10.0f)));  // bottom padding
        ImGui::PopStyleVar();
        DekiEditor::FullBleedSeparator();
        ImGui::Indent(bodyIndent - pathLeftPad);  // restore the body indent for the rest
    }

    // Show play mode indicator
    if (m_App->IsPlaying() || m_App->IsPaused())
    {
        ImGui::TextColored(DekiEditor::Palette::Amber, "%s", Tr("Play Mode - Changes won't persist"));
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
            {
                sourceName = std::filesystem::path(sourcePath).filename().string();
            }
        }

        ImGui::PushStyleColor(ImGuiCol_Text, DekiEditor::Palette::Accent);
        ImGui::TextUnformatted(Tr("Scene Linked Instance -"));
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
                    {
                        browser->RevealAsset(pipeline->GetAbsolutePath(info->path));
                    }
                }
            }
            ImGui::PopStyleColor();
        }

        // Apply / Revert buttons - push instance changes to the source scene
        // asset, or discard them and re-expand from source. Both support undo.
        if (::Deki::Scene* scene = m_App->GetCurrentScene())
        {
            // Apply shows only when this instance differs from its source.
            // CollectOverrides is expensive (loads the source from disk, walks
            // the tree), so the result is cached per object and undo count:
            // recomputed on selection change or after a command, undo or redo.
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
                if (ImGui::SmallButton(TrLabel("Apply")))
                {
                    // Stash the pending Apply and open a confirmation modal
                    // showing exactly what will be written to the source
                    // scene. Actual execution happens on Confirm; see
                    // DrawApplyConfirmPopup.
                    std::vector<std::string> otherScenePaths;
                    if (auto* pipeline = m_App->GetAssetPipeline())
                    {
                        const auto currentPath = std::filesystem::path(m_App->GetCurrentScenePath()).lexically_normal();
                        for (const auto& [relPath, info] : pipeline->GetAllAssets())
                        {
                            const auto ext = std::filesystem::path(relPath).extension().string();
                            if (ext != ".scene")
                            {
                                continue;
                            }
                            std::string abs = pipeline->GetAbsolutePath(info.path);
                            if (abs.empty())
                            {
                                continue;
                            }
                            if (std::filesystem::path(abs).lexically_normal() == currentPath)
                            {
                                continue;
                            }
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

                        const nlohmann::json& newVal = entry.contains("value") ? entry["value"] : nlohmann::json();
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
                            // The override stores the component's serialized
                            // name. Resolve it through the registry to show
                            // the display name the user knows.
                            const std::string compGuid = entry.value("component", std::string());
                            const Deki::ComponentMeta* cmeta = Deki::ComponentRegistry::Instance().GetMeta(compGuid);
                            e.component = cmeta ? cmeta->GetDisplayName() : compGuid;

                            const std::string propName = entry.value("property", std::string());
                            const Deki::PropertyInfo* pi = FindPropertyInfo(compGuid, propName);
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
                                  if (a.path != b.path)
                                  {
                                      return a.path < b.path;
                                  }
                                  if (a.component != b.component)
                                  {
                                      return a.component < b.component;
                                  }
                                  return a.label < b.label;
                              });

                    std::string sourceName;
                    if (!sourcePath.empty())
                    {
                        sourceName = std::filesystem::path(sourcePath).filename().string();
                    }
                    if (sourceName.empty())
                    {
                        sourceName = Tr("source scene");
                    }

                    m_ApplyPopupSourceName = std::move(sourceName);
                    m_ApplyPopupInstance = obj;
                    m_ApplyPopupScene = scene;
                    m_ApplyPopupOtherPaths = std::move(otherScenePaths);
                    m_ApplyPopupOpen = true;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(TrLabel("Revert")))
                {
                    CommandHistory::Instance().Execute(std::make_unique<RevertInstanceOverridesCommand>(scene, obj));
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
        {
            instanceRoot = instanceRoot->GetParent();
        }

        ImGui::PushStyleColor(ImGuiCol_Text, DekiEditor::Palette::Dim);
        if (instanceRoot)
        {
            ImGui::Text(Tr("Part of Scene Instance: %s"), instanceRoot->GetName().c_str());
        }
        else
        {
            ImGui::TextUnformatted(Tr("Part of Scene Instance"));
        }
        ImGui::PopStyleColor();
        ImGui::Separator();
    }

    // The name is edited on the header. Active is a row like every other
    // boolean: label in the left column, box in the value column.
    //
    // It has its own band between the header hairline and the filter strip,
    // and the band pads itself: FullBleedSeparator adds no space above a row
    // while the row adds kRowSpacing below, so by default the row hugs the
    // hairline. Half the row gap on each side keeps the band's height and
    // centres the checkbox in it.
    const float activeBandPad = m_App->Dpi(Metrics::kRowSpacing * 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
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

    // Property filter: one field over the whole object, across components,
    // matching property names and component names.
    //
    // Drawn as a band of its own (hairline above and below, no row spacing
    // inside) so it reads as panel chrome, like a toolbar strip. Its bottom
    // hairline is the divider the Transform band hangs from, so that boundary
    // is one line, not two.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
    DekiEditor::FullBleedSeparator();
    ImGui::SetCursorPosX(InspectorUtils::SectionAlignedCursorX());
    DekiEditor::SearchField("##inspector_filter", m_PropertyFilter, sizeof(m_PropertyFilter),
                            m_App->Dpi(Metrics::kInspectorRightPad), /*bottomBorder*/ true, nullptr, nullptr, nullptr,
                            nullptr, TrMark("Filter properties and components"));
    InspectorUtils::SetPropertyFilter(m_PropertyFilter);
    ImGui::PopStyleVar();

    // Transform
    DrawTransform(obj);

    DekiEditor::FullBleedSeparator();

    // Components - use ComponentManager with a callback
    auto& registry = Deki::ComponentRegistry::Instance();
    m_ComponentManager->DrawComponentList(obj, [this](Deki::Component* comp, const Deki::ComponentMeta* meta)
                                          { DrawComponent(comp, meta); });

    // Say so when the filter matches nothing. Otherwise the panel ends under
    // the Transform separator and looks like an object with no components.
    if (InspectorUtils::PropertyFilterActive() && InspectorUtils::FilterHits() == 0)
    {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, DekiEditor::Palette::Dim);
        ImGui::TextWrapped(Tr("Nothing on this object matches \"%s\"."), InspectorUtils::PropertyFilter().c_str());
        ImGui::PopStyleColor();
    }

    // Add Component button - not available for instance objects
    if (!isInstance && !isPartOfInstance)
    {
        ImGui::Spacing();
        // The button's own rect, measured around the call: the add button ends
        // with a zero-size item (so a panel that ends there passes ImGui's
        // cursor check), so GetItemRect* afterwards describes that item, not
        // the button.
        const ImVec2 addTop = ImGui::GetCursorScreenPos();
        const bool addClicked = DekiEditor::SchematicAddButton(Tr(ICON_TI_PLUS "  Add Component"));
        const float addBottom = ImGui::GetCursorScreenPos().y;

        // Give the picker its anchor every frame, not only on the click, so it
        // has one on the first frame it opens, when ImGui has no size for it
        // yet.
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
        ImGui::OpenPopup(TrLabel("Apply to Scene"));
        m_ApplyPopupOpen = false;
    }

    if (DekiEditor::BeginSchematicModal(TrLabel("Apply to Scene"), 760.0f))
    {
        const size_t total = m_ApplyPopupEntries.size();
        const size_t chosen = static_cast<size_t>(std::count_if(m_ApplyPopupEntries.begin(), m_ApplyPopupEntries.end(),
                                                                [](const ApplyPopupEntry& e) { return e.selected; }));

        ImGui::Text(total == 1 ? Tr("Applying %zu of %zu change to") : Tr("Applying %zu of %zu changes to"), chosen,
                    total);
        ImGui::SameLine();
        ImGui::TextColored(DekiEditor::Palette::Accent, "%s", m_ApplyPopupSourceName.c_str());
        ImGui::TextColored(DekiEditor::Palette::Dim, "%s", Tr("Other scenes that use this scene are refreshed too."));

        // Select all / Deselect all affordances above the list.
        if (ImGui::SmallButton(TrLabel(ICON_TI_LIST_CHECK "  Select all")))
        {
            for (auto& e : m_ApplyPopupEntries)
            {
                e.selected = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(TrLabel(ICON_TI_LIST_DETAILS "  Deselect all")))
        {
            for (auto& e : m_ApplyPopupEntries)
            {
                e.selected = false;
            }
        }
        ImGui::Spacing();

        ImGui::BeginChild("apply_changes", ImVec2(0.0f, 420.0f * ImGui::GetStyle().FontScaleDpi), true);

        if (m_ApplyPopupEntries.empty())
        {
            ImGui::TextDisabled("%s", Tr("(no overrides to apply)"));
        }
        else
        {
            const ImVec4 pathColor = DekiEditor::Palette::Amber;
            const ImVec4 compColor = DekiEditor::Palette::Accent;
            const ImVec4 oldColor = DekiEditor::Palette::Dim;
            const ImVec4 arrowColor = DekiEditor::Palette::Dim;
            const ImVec4 newColor = DekiEditor::Palette::Green;

            std::string prevPath = "<sentinel>\x01";
            std::string prevComp = "<sentinel>\x01";

            for (size_t i = 0; i < m_ApplyPopupEntries.size(); ++i)
            {
                auto& e = m_ApplyPopupEntries[i];

                if (e.path != prevPath)
                {
                    if (i != 0)
                    {
                        ImGui::Spacing();
                    }
                    const char* pathText = e.path.empty() ? Tr("Root") : e.path.c_str();
                    ImGui::TextColored(pathColor, ICON_TI_BOX "  %s", pathText);
                    prevPath = e.path;
                    prevComp = "<sentinel>\x01";
                }

                if (e.component != prevComp)
                {
                    if (!e.component.empty())
                    {
                        ImGui::Indent();
                        ImGui::TextColored(compColor, ICON_TI_COMPONENTS "  %s", e.component.c_str());
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

        const int button = DekiEditor::SchematicDialogButtons(
            { { "Cancel" }, { "Apply", DekiEditor::DialogButton::Primary, chosen > 0 } });
        if (button == 1)
        {
            if (m_ApplyPopupScene && m_ApplyPopupInstance && m_ApplyPopupInstance->IsSceneInstance())
            {
                nlohmann::json selected = nlohmann::json::array();
                for (const auto& e : m_ApplyPopupEntries)
                {
                    if (e.selected)
                    {
                        selected.push_back(e.raw);
                    }
                }

                CommandHistory::Instance().Execute(std::make_unique<ApplyInstanceToSceneCommand>(
                    m_ApplyPopupScene, m_ApplyPopupInstance, std::move(m_ApplyPopupOtherPaths), std::move(selected)));
            }
            ClearApplyPopupState();
            ImGui::CloseCurrentPopup();
        }
        else if (button == 0)
        {
            ClearApplyPopupState();
            ImGui::CloseCurrentPopup();
        }
        DekiEditor::EndSchematicModal();
    }
}

void InspectorPanel::DrawTransform(Deki::Object* obj)
{
    // Transform draws its rows by hand, not through DrawProperty, so the filter
    // is applied here: with a filter typed, the section shows only when one of
    // its three fields, or its own name, matches.
    const bool filtering = InspectorUtils::PropertyFilterActive();

    // Filter cleared: put the section back the way it was found.
    if (!filtering && m_TransformStateKnown)
    {
        ImGui::GetStateStorage()->SetInt(static_cast<ImGuiID>(m_TransformHeaderId), m_TransformPreFilterOpen ? 1 : 0);
        m_TransformStateKnown = false;
    }

    if (filtering && !InspectorUtils::TextMatchesFilter("Transform") &&
        !InspectorUtils::PropertyMatchesFilter("Position") && !InspectorUtils::PropertyMatchesFilter("Rotation") &&
        !InspectorUtils::PropertyMatchesFilter("Scale"))
    {
        return;
    }
    if (filtering)
    {
        InspectorUtils::NoteFilterHit();
    }

    // Matched under a filter, so it is shown open - a hit behind a collapsed
    // header reads as no hit. State recorded the first frame, forced after.
    if (filtering && m_TransformStateKnown)
    {
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    }

    // Section, not a bare header: the body slides shut instead of vanishing.
    // Note the flag is the SECTION's, not the header's - it stays true while the
    // section is closing, which is when the body still has to be submitted.
    ImGuiID transformHeaderId = 0;
    const bool transformBody = SchematicSectionBegin("Transform", ImGuiTreeNodeFlags_DefaultOpen, &transformHeaderId);

    // The agent pointer's Transform row (set_position, set_rotation,
    // set_scale): the header stands in while the section is shut, each row is
    // measured where it is drawn below.
    const AgentIntent* agentWant = AgentCursor::Get().Wanted();
    if (agentWant && (agentWant->target != AgentIntent::Target::Transform || agentWant->objectId != obj->GetId()))
    {
        agentWant = nullptr;
    }
    if (agentWant)
    {
        AgentCursor::Get().Report(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), AgentCursor::Precision::Section);
    }
    auto reportAgentRow = [agentWant](const char* field, const ImVec2& start)
    {
        if (!agentWant || agentWant->property != field)
        {
            return;
        }
        const ImVec2 end(start.x + ImGui::GetContentRegionAvail().x,
                         std::max(ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y,
                                  start.y + ImGui::GetFrameHeight()));
        AgentCursor::Get().Report(start, end, AgentCursor::Precision::Exact);
        if (AgentCursor::Get().WantsReveal() && !ImGui::IsRectVisible(start, end))
        {
            ImGui::SetScrollFromPosY(start.y - ImGui::GetWindowPos().y, 0.5f);
        }
    };
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

            if (ImGui::MenuItem(TrLabel("Copy Transform")))
            {
                clipboard.CopyTransform(obj);
            }

            bool canPaste = clipboard.HasTransform();
            if (ImGui::MenuItem(TrLabel("Paste Transform"), nullptr, false, canPaste))
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

        // The Transform rows' edits record the object, so the undo history can
        // drop them when it is freed (they apply through callbacks holding it).
        HistoryGuards::EditOwnerScope transformOwner(obj);

        // Transform through the shared EditorUI fields in VALUE mode: pass the
        // current value (no pointer to obj's x/y), apply via callbacks. The field
        // records undo itself; onChange/onUndo/onRedo call the proper setter (which
        // also marks the transform dirty so children follow), onStartChange/
        // onEndChange bracket the live edit. Unit::Distance handles px/m + snapping,
        // Unit::Angle handles degrees<->radians.
        const char* posLabel = obj->GetParent() ? TrMark("Local Position") : TrMark("Position");
        ImVec2 agentRowStart = ImGui::GetCursorScreenPos();
        {
            EditorUI::VecCallbacks cb;
            cb.onStartChange = [this](const float*) { BeginLiveEdit(); };
            cb.onChange = [obj](const float* v) { obj->SetLocalPosition(v[0], v[1]); };
            cb.onUndo = [obj](const float* v) { obj->SetLocalPosition(v[0], v[1]); };
            cb.onRedo = [obj](const float* v) { obj->SetLocalPosition(v[0], v[1]); };
            cb.onEndChange = [this](const float*, const float*) { EndLiveEdit(); };
            // A Screen Anchor sets the position, so it shows here but is edited there.
            const bool anchored = obj->HasComponent<Deki::ScreenAnchor>();
            ImGui::BeginDisabled(anchored);
            ImGui::BeginGroup();
            EditorUI::Get().Vector2Field(posLabel, obj->GetX(), obj->GetY(), EditorUI::Unit::Distance, cb);
            ImGui::EndGroup();
            ImGui::EndDisabled();
            if (anchored && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                DekiEditor::Tooltip("Set by Screen Anchor");
            }
        }
        reportAgentRow("position", agentRowStart);
        agentRowStart = ImGui::GetCursorScreenPos();
        {
            EditorUI::FloatCallbacks cb;
            cb.onStartChange = [this](float) { BeginLiveEdit(); };
            cb.onChange = [obj](float v) { obj->SetRotation(v); };  // radians (Unit::Angle shows degrees)
            cb.onUndo = [obj](float v) { obj->SetRotation(v); };
            cb.onRedo = [obj](float v) { obj->SetRotation(v); };
            cb.onEndChange = [this](float, float) { EndLiveEdit(); };
            EditorUI::Get().FloatField(TrMark("Rotation"), obj->GetLocalRotation(), EditorUI::Unit::Angle, cb);
        }
        reportAgentRow("rotation", agentRowStart);
        agentRowStart = ImGui::GetCursorScreenPos();
        {
            EditorUI::VecCallbacks cb;
            cb.onStartChange = [this](const float*) { BeginLiveEdit(); };
            cb.onChange = [obj](const float* v) { obj->SetScale(v[0], v[1]); };
            cb.onUndo = [obj](const float* v) { obj->SetScale(v[0], v[1]); };
            cb.onRedo = [obj](const float* v) { obj->SetScale(v[0], v[1]); };
            cb.onEndChange = [this](const float*, const float*) { EndLiveEdit(); };
            EditorUI::Get().Vector2Field(TrMark("Scale"), obj->GetScaleX(), obj->GetScaleY(), 0.01f, 0.01f, 10.0f,
                                         "%.3f", cb);
        }
        reportAgentRow("scale", agentRowStart);
#ifdef DEKI_TRANSFORM_3D
        // The third axis, only when the project declares transform_3d: a 2D
        // device drops these values on load.
        if (m_App->ProjectDeclaresDepth())
        {
            {
                EditorUI::FloatCallbacks cb;
                cb.onStartChange = [this](float) { BeginLiveEdit(); };
                cb.onChange = [obj](float v) { obj->SetZ(v); };
                cb.onUndo = [obj](float v) { obj->SetZ(v); };
                cb.onRedo = [obj](float v) { obj->SetZ(v); };
                cb.onEndChange = [this](float, float) { EndLiveEdit(); };
                EditorUI::Get().FloatField("Z", obj->GetZ(), EditorUI::Unit::Distance, cb);
            }
            {
                EditorUI::FloatCallbacks cb;
                cb.onStartChange = [this](float) { BeginLiveEdit(); };
                cb.onChange = [obj](float v) { obj->SetRotationX(v); };
                cb.onUndo = [obj](float v) { obj->SetRotationX(v); };
                cb.onRedo = [obj](float v) { obj->SetRotationX(v); };
                cb.onEndChange = [this](float, float) { EndLiveEdit(); };
                EditorUI::Get().FloatField(TrMark("Rotation X"), obj->GetRotationX(), EditorUI::Unit::Angle, cb);
            }
            {
                EditorUI::FloatCallbacks cb;
                cb.onStartChange = [this](float) { BeginLiveEdit(); };
                cb.onChange = [obj](float v) { obj->SetRotationY(v); };
                cb.onUndo = [obj](float v) { obj->SetRotationY(v); };
                cb.onRedo = [obj](float v) { obj->SetRotationY(v); };
                cb.onEndChange = [this](float, float) { EndLiveEdit(); };
                EditorUI::Get().FloatField(TrMark("Rotation Y"), obj->GetRotationY(), EditorUI::Unit::Angle, cb);
            }
            {
                EditorUI::FloatCallbacks cb;
                cb.onStartChange = [this](float) { BeginLiveEdit(); };
                cb.onChange = [obj](float v) { obj->SetScaleZ(v); };
                cb.onUndo = [obj](float v) { obj->SetScaleZ(v); };
                cb.onRedo = [obj](float v) { obj->SetScaleZ(v); };
                cb.onEndChange = [this](float, float) { EndLiveEdit(); };
                EditorUI::Get().FloatField(TrMark("Scale Z"), obj->GetScaleZ(), 0.01f, 0.01f, 10.0f, "%.3f", cb);
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
        ImGui::TextDisabled("%s", Tr("(No properties)"));
        return;
    }

    auto& registry = Deki::ComponentRegistry::Instance();

    // A custom editor (drawn through the EditorUI singleton) comes first.
    // GetEditorForComponent gives a per-component instance with fresh state.
    auto* customEditor = CustomEditorRegistry::Instance().GetEditorForComponent(meta->name, comp);
    bool wantsOverride = false;
    if (customEditor)
    {
        DekiEditor::Guard(meta->name, [&] { wantsOverride = customEditor->WantsInspectorOverride(comp); });
    }
    if (customEditor && wantsOverride)
    {
        // Set up EditorUI singleton state for this component
        // Parameter order: drawByName, drawByOffset, drawDefault, resolveAsset, getFontAtlas
        EditorUI::Get().BeginComponent(
            comp, meta,
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
                }
            },
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
                }
            },
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
                            {
                                m_PropertyEditor->DrawProperty(comp, baseMeta->properties[i]);
                            }
                        }
                    }
                }
                // Draw this component's properties
                for (int i = 0; i < meta->propertyCount; ++i)
                {
                    if (InspectorUtils::EvaluateVisibility(comp, meta->properties[i], meta))
                    {
                        m_PropertyEditor->DrawProperty(comp, meta->properties[i]);
                    }
                }
            },
            // ResolveAssetName callback
            [this](const std::string& guid) -> std::string
            {
                auto* pipeline = m_App->GetAssetPipeline();
                if (pipeline)
                {
                    const AssetInfo* info = pipeline->GetAssetInfoByGuid(guid);
                    if (info)
                    {
                        return info->path;
                    }
                }
                return guid;
            },
            // GetFontAtlasTexture callback (uses on-demand font compilation via EditorAssets)
            [](const std::string& fontGuid, int fontSize, uint32_t* outWidth, uint32_t* outHeight) -> uint32_t
            { return EditorAssets::Get()->LoadFontAtlasWithBaking(fontGuid, fontSize, outWidth, outHeight); },
            // ModifyAsset callback
            [this](const std::string& assetPath, const std::string& assetGuid, const std::string& oldJson,
                   const std::string& newJson)
            {
                try
                {
                    nlohmann::json oldData = nlohmann::json::parse(oldJson);
                    nlohmann::json newData = nlohmann::json::parse(newJson);

                    // The callback reimports the asset and drops every cached copy
                    auto command = std::make_unique<ModifyAssetCommand>(
                        assetPath, assetGuid, oldData, newData,
                        [this, assetPath, assetGuid]()
                        {
                            // Trigger asset reimport (recompiles DTEX on disk)
                            AssetDatabase::ImportAsset(AssetDatabase::GUIDToAssetPath(assetGuid));

                            // Clear component AssetRef pointers first (prevents dangling pointers)
                            m_App->GetSceneViewPanel()->InvalidateAllAssets();

                            // Evict stale sprite from engine cache so reload picks up new file
                            Deki::AssetManager::Get()->InvalidateAsset("Sprite", assetGuid);

                            // Invalidate GPU texture caches
                            EditorAssets::Get()->InvalidateTexture(assetGuid);
                            m_App->GetSceneViewPanel()->InvalidateTexture(assetGuid);
                        });

                    CommandHistory::Instance().Execute(std::move(command));
                }
                catch (...)
                {
                    // Silently ignore JSON parse errors
                }
            });

        // Call the custom editor - uses EditorUI::Get() for drawing.
        // Guarded so a faulty CustomEditor (e.g. stale package-DLL state)
        // doesn't terminate the editor; EndComponent still runs to keep
        // the EditorUI singleton state balanced.
        DekiEditor::Guard(meta->name, [&] { customEditor->OnInspectorGUI(comp); });

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

    // Packages add their own inspector extras (e.g. a tween's duration) by
    // registering custom inspectors.
}

void InspectorPanel::DrawMultiObjectInspector(const std::vector<Deki::Object*>& objects)
{
    // Header in the same band as a single object's: a display-font heading
    // with the names below it.
    {
        const float bodyIndent = m_App->Dpi(Metrics::kInspectorIndent);
        const float pathLeftPad = m_App->Dpi(16.0f);
        ImGui::Unindent(bodyIndent - pathLeftPad);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
        ImGui::Dummy(ImVec2(0, m_App->Dpi(10.0f)));
        ImFont* display = g_FontDisplay;
        if (display)
        {
            ImGui::PushFont(display, DekiEditor::Fonts::CssPx(display, 20.0f));
        }
        ImGui::PushStyleColor(ImGuiCol_Text, Palette::Accent);
        ImGui::Text(Tr("%zu objects"), objects.size());
        ImGui::PopStyleColor();
        if (display)
        {
            ImGui::PopFont();
        }

        std::string names;
        const size_t shown = std::min<size_t>(objects.size(), 3);
        for (size_t i = 0; i < shown; ++i)
        {
            names += (i ? ", " : "") + objects[i]->GetName();
        }
        if (objects.size() > shown)
        {
            char more[1024];
            snprintf(more, sizeof(more), Tr("%s and %zu more"), names.c_str(), objects.size() - shown);
            names = more;
        }
        ImGui::Dummy(ImVec2(0, m_App->Dpi(4.0f)));
        ImGui::PushStyleColor(ImGuiCol_Text, Palette::Dim);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(names.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, m_App->Dpi(10.0f)));
        ImGui::PopStyleVar();
        DekiEditor::FullBleedSeparator();
        ImGui::Indent(bodyIndent - pathLeftPad);
    }

    // Play mode indicator
    if (m_App->IsPlaying() || m_App->IsPaused())
    {
        ImGui::TextColored(DekiEditor::Palette::Amber, "%s", Tr("Play Mode - Changes won't persist"));
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

            if (ImGui::MenuItem(TrLabel("Copy Transform")))
            {
                clipboard.CopyTransform(objects[0]);
            }

            bool canPaste = clipboard.HasTransform();
            if (ImGui::MenuItem(TrLabel("Paste Transform"), nullptr, false, canPaste))
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

        DrawMultiTransform(objects);
        SchematicSectionEnd();
    }

    // Find common components across all selected objects
    auto commonComponents = GetCommonComponents(objects);

    if (commonComponents.empty())
    {
        ImGui::TextDisabled("%s", Tr("No common components"));
        return;
    }

    // Draw each common component type
    for (const auto& group : commonComponents)
    {
        DrawMultiComponentSection(group);
    }
}

// Transform rows for several objects: each box shows the value they share, or
// "-" when they differ; changing it sets that value on all of them. The whole
// drag or typed edit is one undo step that puts each object's own old value
// back (a single shared "before" would not).
void InspectorPanel::DrawMultiTransform(const std::vector<Deki::Object*>& objects)
{
    struct Before
    {
        Deki::Object* obj;
        float x, y, rot, sx, sy;
    };
    static std::vector<Before> s_Before;  // snapshot taken when a box is grabbed

    const float ppm = Deki::EngineSettings::Global().pixelsPerMeter;
    constexpr float kRadToDeg = 57.29578f;

    // Component `i` of row `row` of an object, in canonical units.
    auto get = [](Deki::Object* o, int row, int i) -> float
    {
        switch (row)
        {
            case 0: return i == 0 ? o->GetX() : o->GetY();
            case 1: return o->GetLocalRotation();
            default: return i == 0 ? o->GetScaleX() : o->GetScaleY();
        }
    };
    auto set = [](Deki::Object* o, int row, int i, float v)
    {
        switch (row)
        {
            case 0: o->SetLocalPosition(i == 0 ? v : o->GetX(), i == 0 ? o->GetY() : v); break;
            case 1: o->SetRotation(v); break;
            default: o->SetScale(i == 0 ? v : o->GetScaleX(), i == 0 ? o->GetScaleY() : v); break;
        }
    };

    struct Row
    {
        const char* label;
        int count;
        float toDisplay;
        const char* format;
        float speed;
    };
    const Row rows[3] = {
        { objects[0]->GetParent() ? TrMark("Local Position") : TrMark("Position"), 2, ppm, "%.0f px", 1.0f },
        { TrMark("Rotation"), 1, kRadToDeg, "%.2f\xc2\xb0", 0.5f },
        { TrMark("Scale"), 2, 1.0f, "%.3f", 0.01f },
    };

    bool anyAnchored = false;
    for (Deki::Object* o : objects)
    {
        anyAnchored |= o->HasComponent<Deki::ScreenAnchor>();
    }

    for (int row = 0; row < 3; ++row)
    {
        const Row& r = rows[row];
        InspectorUtils::BeginPropertyRow(r.label);
        ImGui::PushID(row);
        // A Screen Anchor sets the position (see DrawTransform).
        const bool locked = row == 0 && anyAnchored;
        ImGui::BeginDisabled(locked);
        // The single-object rows' widths and gap (SchematicDragFloat2's).
        const float gap = ImGui::GetStyle().ItemInnerSpacing.x;
        const float boxW = (ImGui::CalcItemWidth() - gap * (float)(r.count - 1)) / (float)r.count;
        for (int i = 0; i < r.count; ++i)
        {
            if (i > 0)
            {
                ImGui::SameLine(0.0f, gap);
            }
            const float first = get(objects[0], row, i);
            bool mixed = false;
            for (Deki::Object* o : objects)
            {
                mixed |= get(o, row, i) != first;
            }

            float disp = first * r.toDisplay;
            ImGui::PushID(i);
            ImGui::SetNextItemWidth(boxW);
            const char* format = mixed ? "\xe2\x80\x94" : r.format;
            const bool edited = r.count > 1 ? SchematicDragFloatAxis("##v", &disp, i, r.speed, 0.0f, 0.0f, format)
                                            : SchematicDragFloat("##v", &disp, r.speed, 0.0f, 0.0f, format);
            if (ImGui::IsItemActivated())
            {
                s_Before.clear();
                for (Deki::Object* o : objects)
                {
                    s_Before.push_back(
                        { o, o->GetX(), o->GetY(), o->GetLocalRotation(), o->GetScaleX(), o->GetScaleY() });
                }
                BeginLiveEdit();
            }
            if (edited && r.toDisplay != 0.0f)
            {
                for (Deki::Object* o : objects)
                {
                    set(o, row, i, disp / r.toDisplay);
                }
            }
            if (ImGui::IsItemDeactivated())
            {
                EndLiveEdit();
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    std::vector<std::unique_ptr<Command>> steps;
                    for (const Before& b : s_Before)
                    {
                        Deki::Object* o = b.obj;
                        if (row == 0 && (o->GetX() != b.x || o->GetY() != b.y))
                        {
                            steps.push_back(std::make_unique<SetPositionCommand>(o, b.x, b.y, o->GetX(), o->GetY()));
                        }
                        else if (row == 1 && o->GetLocalRotation() != b.rot)
                        {
                            steps.push_back(std::make_unique<SetRotationCommand>(o, b.rot, o->GetLocalRotation()));
                        }
                        else if (row == 2 && (o->GetScaleX() != b.sx || o->GetScaleY() != b.sy))
                        {
                            steps.push_back(
                                std::make_unique<SetScaleCommand>(o, b.sx, b.sy, o->GetScaleX(), o->GetScaleY()));
                        }
                    }
                    if (!steps.empty())
                    {
                        CommandHistory::Instance().Execute(std::make_unique<CompoundCommand>(
                            std::string(r.label) + " (" + std::to_string(objects.size()) + " objects)",
                            std::move(steps)));
                    }
                }
                s_Before.clear();
            }
            ImGui::PopID();
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
}

std::vector<InspectorPanel::ComponentGroup>
InspectorPanel::GetCommonComponents(const std::vector<Deki::Object*>& objects)
{
    std::vector<ComponentGroup> result;
    if (objects.empty())
    {
        return result;
    }

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
            {
                it = componentMap.erase(it);
            }
            else
            {
                ++it;
            }
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
    {
        return;
    }

    const bool open = SchematicSectionBegin(group.meta->GetDisplayName(),
                                            ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);

    // Right-aligned `EDIT ->` action link for package/project components.
    if (group.meta && !group.components.empty() && ComponentManager::IsEditableComponent(m_App, group.meta->typeId))
    {
        if (SchematicActionLink((std::string(Tr("EDIT")) + "##multi_comp_edit_" + group.typeName).c_str()))
        {
            ComponentManager::OpenComponentSourceFile(m_App, group.meta);
        }
    }

    if (!open)
    {
        return;
    }

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
