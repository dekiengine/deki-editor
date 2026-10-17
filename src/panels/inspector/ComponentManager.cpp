#include "ComponentManager.h"
#include "../../core/commands/HistoryGuards.h"
#include <unordered_map>
#include <deki-editor/Paths.h>
#include <deki-editor/ProcessLaunch.h>
#include "InspectorUtils.h"
#include "../../EditorApp.h"
#include <deki-editor/EditorTheme.h>
#include <deki-editor/CommandHistory.h>
#include "../../core/Commands.h"
#include <deki-editor/build/PlatformConfig.h>
#include "../../services/AgentCursor.h"
#include "../../services/ClipboardService.h"
#include "../../services/PackageLoader.h"
#include <deki-editor/EditorSettings.h>
#include "../../services/PluginLoader.h"
#include <deki/Object.h>
#include <deki/Component.h>
#include "MissingComponent.h"
#include <deki/Scene.h>
#include <deki/reflection/ComponentRegistry.h>
#include "imgui.h"
#include <deki-editor/IconsTabler.h>
#include <deki-editor/Localization.h>
#include "../../core/Project.h"
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <climits>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace DekiEditor
{

// The component's .cpp (or else .h) file under `dir`, found by name.
static std::string FindComponentSourceFile(const std::string& dir, const std::string& componentName)
{
    namespace fs = std::filesystem;
    if (dir.empty() || !fs::exists(dir))
    {
        return "";
    }

    for (const char* ext : { ".cpp", ".h" })
    {
        std::string filename = componentName + ext;
        try
        {
            for (const auto& entry : fs::recursive_directory_iterator(dir))
            {
                if (!entry.is_regular_file())
                {
                    continue;
                }
                if (entry.path().filename().string() == filename)
                {
                    return entry.path().string();
                }
            }
        }
        catch (const fs::filesystem_error&)
        {
        }
    }
    return "";
}

bool ComponentManager::IsEditableComponent(EditorApp* app, uint32_t typeId)
{
    // Project plugin components.
    if (Deki::ComponentRegistry::Instance().IsPluginComponent(typeId))
    {
        return true;
    }

    // Components of a loaded package. This runs per component per frame, so
    // the answer is cached per type and dropped when the loaded packages
    // change.
    auto* packageLoader = app->GetPackageLoader();
    if (!packageLoader)
    {
        return false;
    }
    const auto& packages = packageLoader->GetLoadedPackages();

    static std::unordered_map<uint32_t, bool> s_Cache;
    static size_t s_CachedPackageCount = static_cast<size_t>(-1);
    static const void* s_CachedPackagesData = nullptr;
    if (packages.size() != s_CachedPackageCount || packages.data() != s_CachedPackagesData)
    {
        s_Cache.clear();
        s_CachedPackageCount = packages.size();
        s_CachedPackagesData = packages.data();
    }
    auto it = s_Cache.find(typeId);
    if (it != s_Cache.end())
    {
        return it->second;
    }

    bool editable = false;
    for (const auto& pkg : packages)
    {
        for (uint32_t modTypeId : pkg.registeredTypeIds)
        {
            if (modTypeId == typeId)
            {
                editable = true;
                break;
            }
        }
        if (editable)
        {
            break;
        }
    }
    s_Cache[typeId] = editable;
    return editable;
}

void ComponentManager::OpenComponentSourceFile(EditorApp* app, const Deki::ComponentMeta* meta)
{
    if (!meta || !meta->name)
    {
        return;
    }

    std::string componentName = meta->name;
    std::string sourcePath;

    // Packages first, then project plugins.
    auto* packageLoader = app->GetPackageLoader();
    if (packageLoader)
    {
        for (const auto& pkg : packageLoader->GetLoadedPackages())
        {
            for (uint32_t typeId : pkg.registeredTypeIds)
            {
                if (typeId == meta->typeId)
                {
                    std::string projectPath = DekiEditor::EditorSettings::GetLastProjectPath();
                    std::string sourceDir = PackageLoader::ResolvePackageSourceDir(projectPath, pkg.dllFilename);
                    sourcePath = FindComponentSourceFile(sourceDir, componentName);
                    break;
                }
            }
            if (!sourcePath.empty())
            {
                break;
            }
        }
    }

    if (sourcePath.empty())
    {
        auto* project = app->GetProject();
        if (project)
        {
            sourcePath = FindComponentSourceFile(project->GetSrcPath(), componentName);
        }
    }

    if (sourcePath.empty())
    {
        return;
    }

    OpenInCodeEditor(sourcePath);
}

bool ComponentManager::IsProtectedComponent(Deki::Object* obj) const
{
    if (!obj || obj->GetName() != "Setup" || obj->GetParent() != nullptr)
    {
        return false;
    }

    auto* project = m_App->GetProject();
    if (!project)
    {
        return false;
    }

    const std::string& absPath = m_App->GetCurrentScenePath();
    std::string platformsDir = GetPlatformsDirectory(project->GetPath());
    auto normPath = std::filesystem::path(absPath).lexically_normal().generic_string();
    auto normPlatforms = std::filesystem::path(platformsDir).lexically_normal().generic_string();

    return normPath.find(normPlatforms) == 0;
}

bool ComponentManager::IsBootScene() const
{
    auto* project = m_App->GetProject();
    if (!project)
    {
        return false;
    }

    const std::string& currentPath = m_App->GetCurrentScenePath();
    if (currentPath.empty())
    {
        return false;
    }

    // The active platform's boot scene.
    const PlatformConfig* config = PlatformRegistry::Instance().GetPlatform(project->GetActivePlatformId());

    if (!config || config->bootScene.empty())
    {
        return false;
    }

    // Compared as normalized paths.
    namespace fs = std::filesystem;
    std::string bootSceneAbs = (fs::path(project->GetAssetsPath()) / config->bootScene).string();
    auto normCurrent = fs::path(currentPath).lexically_normal().generic_string();
    auto normBoot = fs::path(bootSceneAbs).lexically_normal().generic_string();

    return normCurrent == normBoot;
}

bool ComponentManager::IsPlatformScene() const
{
    auto* project = m_App->GetProject();
    if (!project)
    {
        return false;
    }

    const std::string& absPath = m_App->GetCurrentScenePath();
    if (absPath.empty())
    {
        return false;
    }

    std::string platformsDir = GetPlatformsDirectory(project->GetPath());
    auto normPath = std::filesystem::path(absPath).lexically_normal().generic_string();
    auto normPlatforms = std::filesystem::path(platformsDir).lexically_normal().generic_string();

    return normPath.find(normPlatforms) == 0;
}

ComponentManager::ComponentManager(EditorApp* app)
    : m_App(app)
{
}

bool ComponentManager::DrawComponentList(Deki::Object* obj, DrawComponentCallback drawComponent)
{
    const auto& components = obj->GetComponents();
    auto& registry = Deki::ComponentRegistry::Instance();
    bool componentRemoved = false;

    // Filtering forces matching components open (below). When the filter
    // clears, each one gets back its earlier state, written into ImGui's
    // storage where a collapsing header keeps it.
    const bool filtering = InspectorUtils::PropertyFilterActive();
    if (!filtering && m_FilterWasActive)
    {
        ImGuiStorage* storage = ImGui::GetStateStorage();
        for (const auto& [key, state] : m_PreFilterOpen)
        {
            storage->SetInt(static_cast<ImGuiID>(state.headerId), state.open ? 1 : 0);
        }
        m_PreFilterOpen.clear();
    }
    m_FilterWasActive = filtering;

    for (size_t i = 0; i < components.size(); ++i)
    {
        Deki::Component* comp = components[i];

        ImGui::PushID(static_cast<int>(i));

        // No offset here: FullBleedSeparator adds no spacing after its line,
        // so the header starts right on it. Pulling the cursor up would paint
        // the band over the line.

        // A missing component: red header, nothing to edit.
        if (comp->GetType() == Deki::MissingComponent::kStaticType)
        {
            auto* missing = static_cast<Deki::MissingComponent*>(comp);

            ImFont* f = g_FontDisplaySmall;
            if (f)
            {
                ImGui::PushFont(f);
            }
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 8.0f));
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.5f, 0.1f, 0.1f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.6f, 0.15f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.7f, 0.2f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, Palette::Red);

            // The id stays the English text, so the header keeps its state
            // when the language changes.
            char shown[256];
            std::snprintf(shown, sizeof(shown), Tr("Missing: %s"), missing->typeName.c_str());
            std::string label = std::string(shown) + "###Missing: " + missing->typeName;
            bool open = ImGui::CollapsingHeader(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen);

            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar();
            if (f)
            {
                ImGui::PopFont();
            }

            // Its context menu offers only Remove.
            PushContextMenuPopupStyle();
            if (ImGui::BeginPopupContextItem())
            {
                PushContextMenuItemStyle();
                if (ImGui::MenuItem(TrLabel("Remove Component")))
                {
                    CommandHistory::Instance().Execute(std::make_unique<RemoveComponentCommand>(obj, comp));
                    ImGui::PopStyleVar();  // context menu ItemSpacing
                    ImGui::EndPopup();
                    ImGui::PopID();
                    componentRemoved = true;
                    ImGui::PopStyleVar();  // context menu WindowPadding
                    return componentRemoved;
                }
                ImGui::PopStyleVar();  // context menu ItemSpacing
                ImGui::EndPopup();
            }
            ImGui::PopStyleVar();  // context menu WindowPadding

            if (open)
            {
                ImGui::TextColored(DekiEditor::Palette::Red, "%s", Tr("Component type not found"));
                if (!missing->jsonProperties.empty())
                {
                    ImGui::TextDisabled(Tr("Data preserved (%u properties)"), (unsigned)missing->jsonProperties.size());
                }
                else if (!missing->rawData.empty())
                {
                    ImGui::TextDisabled(Tr("Data preserved (%u bytes, %u fields)"), (unsigned)missing->rawData.size(),
                                        missing->dataMapSize);
                }
                else
                {
                    ImGui::TextDisabled("%s", Tr("No data"));
                }
            }

            DekiEditor::FullBleedSeparator();
            ImGui::PopID();
            continue;
        }

        const Deki::ComponentMeta* meta = registry.GetMeta(comp->GetType());

        // With a filter typed, a component with no matching property is left
        // out, header and all.
        if (!InspectorUtils::ComponentHasFilterMatch(meta))
        {
            ImGui::PopID();
            continue;
        }
        // Counted per component, not per row: a header answers the query even
        // collapsed, and even with no properties under it.
        if (InspectorUtils::PropertyFilterActive())
        {
            InspectorUtils::NoteFilterHit();
        }

        const char* compDisplayName = meta ? meta->GetDisplayName() : TrMark("Unknown Component");
        // A component that passes the filter is forced open while the filter
        // lasts. Its own state was recorded the first frame it was seen (see
        // below) and is put back when the filter clears.
        const uint64_t stateKey = (static_cast<uint64_t>(comp->GetType()) << 32) | static_cast<uint32_t>(i);
        const bool stateKnown = m_PreFilterOpen.find(stateKey) != m_PreFilterOpen.end();
        if (filtering && stateKnown)
        {
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        }

        // A section, not a bare header: `open` stays true while the component
        // closes, since its rows are still drawn for the slide-up. Every path
        // out of this iteration with `open` set must close the section,
        // including the removal below, which returns.
        ImGuiID headerId = 0;
        const bool open = DekiEditor::SchematicSectionBegin(
            compDisplayName, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap, &headerId);

        // First frame this component is seen under a filter: it was not forced
        // above, so ImGui holds the user's own state. Record it; forcing starts
        // next frame.
        if (filtering && !stateKnown && headerId != 0)
        {
            m_PreFilterOpen[stateKey] = PreFilterState{ static_cast<unsigned int>(headerId),
                                                        ImGui::GetStateStorage()->GetInt(headerId, 1) != 0 };
        }
        ImGui::OpenPopupOnItemClick("ComponentContextMenu", ImGuiPopupFlags_MouseButtonRight);

        // The agent pointer's component: its header is the target of a
        // component tool, and stands in for one of its properties (exact once
        // the row is drawn, which a collapsed header does not do).
        if (const AgentIntent* want = AgentCursor::Get().Wanted();
            want && meta && want->objectId == obj->GetId() && want->component == meta->serializedName &&
            (want->target == AgentIntent::Target::Component || want->target == AgentIntent::Target::Property))
        {
            AgentCursor::Get().Report(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                      want->target == AgentIntent::Target::Component ? AgentCursor::Precision::Exact
                                                                                     : AgentCursor::Precision::Section);
            if (AgentCursor::Get().WantsReveal() && !ImGui::IsItemVisible())
            {
                ImGui::SetScrollHereY(0.5f);
            }
        }

        // Right-aligned `EDIT ->` action link for package/project components.
        if (meta && IsEditableComponent(m_App, comp->GetType()))
        {
            if (DekiEditor::SchematicActionLink((std::string(Tr("EDIT")) + "##comp_edit_" + std::to_string(i)).c_str()))
            {
                OpenComponentSourceFile(m_App, meta);
            }
        }

        PushContextMenuPopupStyle();
        if (ImGui::BeginPopup("ComponentContextMenu"))
        {
            PushContextMenuItemStyle();
            auto& clipboard = ClipboardService::Instance();
            const std::string typeName = meta ? meta->name : "";
            const std::string serializedName = meta ? meta->serializedName : "";

            if (ImGui::MenuItem(TrLabel("Copy Component")))
            {
                clipboard.CopyComponent(comp);
            }

            ImGui::Separator();

            bool canPasteValues = clipboard.HasComponentOfType(serializedName);
            if (ImGui::MenuItem(TrLabel("Paste Component Values"), nullptr, false, canPasteValues))
            {
                auto compDataOpt = clipboard.GetComponentData();
                if (compDataOpt.has_value())
                {
                    const auto& selectedObjects = m_App->GetSelectedObjects();
                    std::vector<Deki::Component*> targetComponents;

                    for (auto* selObj : selectedObjects)
                    {
                        for (auto* selComp : selObj->GetComponents())
                        {
                            const Deki::ComponentMeta* selMeta = registry.GetMeta(selComp->GetType());
                            if (selMeta && selMeta->serializedName == serializedName)
                            {
                                targetComponents.push_back(selComp);
                                break;
                            }
                        }
                    }

                    if (!targetComponents.empty())
                    {
                        CommandHistory::Instance().Execute(
                            std::make_unique<PasteComponentValuesCommand>(targetComponents, compDataOpt.value()));
                    }
                }
            }

            bool canPasteAsNew = clipboard.HasComponent();
            if (ImGui::MenuItem(TrLabel("Paste Component as New"), nullptr, false, canPasteAsNew))
            {
                auto compDataOpt = clipboard.GetComponentData();
                if (compDataOpt.has_value())
                {
                    const auto& selectedObjects = m_App->GetSelectedObjects();
                    std::vector<Deki::Object*> targetObjects;

                    const Deki::ComponentMeta* pasteMeta = registry.GetMeta(compDataOpt->typeName);

                    for (auto* selObj : selectedObjects)
                    {
                        bool canAdd = true;
                        if (pasteMeta && pasteMeta->isUnique)
                        {
                            for (auto* existComp : selObj->GetComponents())
                            {
                                if (existComp->GetType() == pasteMeta->typeId)
                                {
                                    canAdd = false;
                                    break;
                                }
                            }
                        }
                        if (canAdd)
                        {
                            targetObjects.push_back(selObj);
                        }
                    }

                    if (!targetObjects.empty())
                    {
                        CommandHistory::Instance().Execute(
                            std::make_unique<PasteComponentAsNewCommand>(targetObjects, compDataOpt.value()));
                    }
                }
            }

            ImGui::Separator();

            bool protectRemove = (typeName == "PlatformSetupComponent" && IsProtectedComponent(obj));
            if (ImGui::MenuItem(TrLabel("Remove Component"), nullptr, false, !protectRemove))
            {
                CommandHistory::Instance().Execute(std::make_unique<RemoveComponentCommand>(obj, comp));
                ImGui::PopStyleVar();  // context menu ItemSpacing
                ImGui::EndPopup();
                ImGui::PopID();
                componentRemoved = true;
                ImGui::PopStyleVar();  // context menu WindowPadding
                if (open)
                {
                    DekiEditor::SchematicSectionEnd();
                }
                return componentRemoved;
            }
            ImGui::PopStyleVar();  // context menu ItemSpacing
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();  // context menu WindowPadding

        if (open)
        {
            bool hasProperties = meta && (meta->propertyCount > 0 || meta->baseTypeId != 0);
            if (hasProperties)
            {
                // A filter that names this component shows all of it. Set for
                // these rows only, so the next component is judged on its own
                // properties.
                InspectorUtils::SetComponentNameMatched(InspectorUtils::ComponentNameMatchesFilter(meta));
                {
                    // Edits made in these rows record whose they are, so the
                    // undo history can drop them when it is freed.
                    HistoryGuards::EditOwnerScope objectOwner(obj);
                    HistoryGuards::EditOwnerScope componentOwner(comp);
                    drawComponent(comp, meta);
                }
                InspectorUtils::SetComponentNameMatched(false);
            }
            else
            {
                ImGui::TextDisabled("%s", Tr("(No properties)"));
            }
            DekiEditor::SchematicSectionEnd();
        }

        DekiEditor::FullBleedSeparator();
        ImGui::PopID();
    }

    return componentRemoved;
}

void ComponentManager::DrawAddComponentMenu(Deki::Object* obj)
{
    // A search-first picker that browses in two levels: categories, then the
    // components in one. Browsing helps when you cannot name the component;
    // typing searches every level at once, grouped by category and best match
    // first, and clearing the box returns to where you were. Keyboard works
    // throughout: type, arrows, Enter to open a group or add a component, Left
    // to go back.
    const float popupW = m_App->Dpi(300.0f);
    const float listMaxH = m_App->Dpi(360.0f);
    // Room for the search band, its line and the popup's padding, with slack.
    // Only the list scrolls, so the popup must fit a full-height list plus
    // this; too little and the popup scrolls too.
    const float chromeH = m_App->Dpi(140.0f);
    const float popupMaxH = listMaxH + chromeH;

    // Cap the popup's height. On its first frame an auto-resizing popup has no
    // content size yet, and ImGui would place it from a wrong size.
    ImGui::SetNextWindowSizeConstraints(ImVec2(popupW, 0.0f), ImVec2(popupW, popupMaxH));

    // Then place it from the button's rect. ImGui needs the window's size to
    // place a popup, and there is none on its first frame. With the cap the
    // height is known in advance, so below-or-above is decided on frame one.
    if (m_AddPopupAnchor.valid)
    {
        const float gap = m_App->Dpi(4.0f);
        const float margin = m_App->Dpi(8.0f);

        // Vertical: below the button, or above it when the capped height does
        // not fit. Worked out as a top edge, not a pivot, so one clamp covers
        // both cases.
        const bool fitsBelow = m_AddPopupAnchor.bottom + popupMaxH + margin <= m_AddPopupAnchor.workBottom;
        float top = fitsBelow ? m_AddPopupAnchor.bottom + gap : m_AddPopupAnchor.top - gap - popupMaxH;
        if (top + popupMaxH > m_AddPopupAnchor.workBottom - margin)
        {
            top = m_AddPopupAnchor.workBottom - margin - popupMaxH;
        }
        if (top < m_AddPopupAnchor.workTop + margin)
        {
            top = m_AddPopupAnchor.workTop + margin;
        }

        // Horizontal: the picker is wider than its panel, so left-aligned with
        // the button it can pass the right edge, and a popup that does not fit
        // its viewport becomes a separate OS window, possibly off the desktop.
        float left = m_AddPopupAnchor.x;
        if (left + popupW > m_AddPopupAnchor.workRight - margin)
        {
            left = m_AddPopupAnchor.workRight - margin - popupW;
        }
        if (left < m_AddPopupAnchor.workLeft + margin)
        {
            left = m_AddPopupAnchor.workLeft + margin;
        }

        // Always, not Appearing: set every frame the popup is open, so a wrong
        // placement is corrected on the next frame.
        ImGui::SetNextWindowPos(ImVec2(left, top), ImGuiCond_Always);
    }

    PushContextMenuPopupStyle();
    // No scrollbar on the popup itself: only the list inside scrolls.
    if (ImGui::BeginPopup("AddComponentPopup", ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
    {
        auto& registry = Deki::ComponentRegistry::Instance();

        // What the picker lists, built each frame: it changes with the object
        // (a unique component already on it cannot be picked) and with hot
        // reload.
        //
        // Left out: abstract types, which cannot be created; scene-scoped ones
        // on anything but their scene; and settings types (category
        // "Settings"), which live in the Project Settings panel.
        m_AddCompMetas.clear();
        m_AddCompItems.clear();
        m_AddCompTips.clear();
        for (const auto& category : registry.GetCategories())
        {
            if (category == "Settings")
            {
                continue;
            }
            for (const auto* meta : registry.GetComponentsByCategory(category))
            {
                if (meta->isAbstract)
                {
                    continue;
                }
                if (meta->bootSceneOnly && !IsBootScene())
                {
                    continue;
                }
                if (meta->platformSceneOnly && !IsPlatformScene())
                {
                    continue;
                }
                m_AddCompMetas.push_back(meta);
            }
        }

        auto isTaken = [&](const Deki::ComponentMeta* meta)
        {
            if (!meta->isUnique)
            {
                return false;
            }
            for (auto* comp : obj->GetComponents())
            {
                if (comp->GetType() == meta->typeId)
                {
                    return true;
                }
            }
            return false;
        };

        // A deque, not a vector: the items point into these strings, and a
        // growing vector would move them.
        for (const Deki::ComponentMeta* meta : m_AddCompMetas)
        {
            // Shown in the editor's language; the class name stays searchable.
            const char* displayName = meta->GetDisplayName();
            DekiEditor::PickerItem item;
            item.label = Tr(displayName ? displayName : meta->name);
            item.altName = meta->name;
            item.category = meta->category ? Tr(meta->category) : nullptr;
            item.description = meta->description ? Tr(meta->description) : nullptr;  // shown under the name
            item.disabled = isTaken(meta);
            if (item.disabled)
            {
                char tip[160];
                std::snprintf(tip, sizeof(tip), Tr("Only one %s allowed per object"), item.label ? item.label : "");
                m_AddCompTips.push_back(tip);
                item.disabledTip = m_AddCompTips.back().c_str();
            }
            m_AddCompItems.push_back(item);
        }

        const int chosen = DekiEditor::SchematicPicker(
            "##addcomp", Tr("Add Component"), m_AddCompItems.data(), static_cast<int>(m_AddCompItems.size()),
            m_PopupJustOpened, listMaxH, ICON_TI_PUZZLE, Tr("Search components"), Tr("No components available"));
        m_PopupJustOpened = false;

        if (chosen >= 0 && chosen < static_cast<int>(m_AddCompMetas.size()))
        {
            // Through the command history so it can be undone. The command
            // also clears the behaviours cache, so ProcessInternalAwake sees
            // the new component.
            CommandHistory::Instance().Execute(
                std::make_unique<AddComponentCommand>(obj, m_AddCompMetas[chosen]->serializedName));

            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
    else
    {
        // So the search box gets focus when the popup opens again.
        m_PopupJustOpened = true;
    }
    ImGui::PopStyleVar();  // context menu WindowPadding
}

}  // namespace DekiEditor
