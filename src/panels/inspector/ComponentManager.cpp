#include "ComponentManager.h"
#include <unordered_map>
#include <deki-editor/Paths.h>
#include "../../services/ProcessLaunch.h"
#include "InspectorUtils.h"
#include "../../EditorApp.h"
#include <deki-editor/EditorTheme.h>
#include <deki-editor/CommandHistory.h>
#include "../../core/Commands.h"
#include <deki-editor/build/PlatformConfig.h>
#include "../../services/ClipboardService.h"
#include "../../services/PackageLoader.h"
#include "../../services/EditorSettings.h"
#include "../../services/PluginLoader.h"
#include <deki/Object.h>
#include <deki/Component.h>
#include "MissingComponent.h"
#include <deki/Scene.h>
#include <deki/reflection/ComponentRegistry.h>
#include "imgui.h"
#include <deki-editor/IconsTabler.h>
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

// Find the source file (.cpp or .h) for a component by name in a directory tree
static std::string FindComponentSourceFile(const std::string& dir, const std::string& componentName)
{
    namespace fs = std::filesystem;
    if (dir.empty() || !fs::exists(dir))
        return "";

    // Look for ComponentName.cpp first, then .h
    for (const char* ext : { ".cpp", ".h" })
    {
        std::string filename = componentName + ext;
        try
        {
            for (const auto& entry : fs::recursive_directory_iterator(dir))
            {
                if (!entry.is_regular_file())
                    continue;
                if (entry.path().filename().string() == filename)
                    return entry.path().string();
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
    // Check if registered as a plugin component (project plugins)
    if (Deki::ComponentRegistry::Instance().IsPluginComponent(typeId))
        return true;

    // Check if it belongs to any loaded package. This runs per component per
    // frame and used to walk every package's type list each time; the answer
    // is cached per type and dropped when the set of loaded packages changes.
    auto* packageLoader = app->GetPackageLoader();
    if (!packageLoader)
        return false;
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
        return it->second;

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
        if (editable) break;
    }
    s_Cache[typeId] = editable;
    return editable;
}

void ComponentManager::OpenComponentSourceFile(EditorApp* app, const Deki::ComponentMeta* meta)
{
    if (!meta || !meta->name)
        return;

    std::string componentName = meta->name;
    std::string sourcePath;

    // Check packages first
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
                break;
        }
    }

    // Check project plugins if not found in packages
    if (sourcePath.empty())
    {
        auto* project = app->GetProject();
        if (project)
            sourcePath = FindComponentSourceFile(project->GetSrcPath(), componentName);
    }

    if (sourcePath.empty())
        return;

    OpenInCodeEditor(sourcePath);
}

bool ComponentManager::IsProtectedComponent(Deki::Object* obj) const
{
    if (!obj || obj->GetName() != "Setup" || obj->GetParent() != nullptr)
        return false;

    auto* project = m_App->GetProject();
    if (!project)
        return false;

    const std::string& absPath = m_App->GetCurrentScenePath();
    std::string platformsDir = GetPlatformsDirectory(project->GetPath());
    auto normPath = std::filesystem::path(absPath).lexically_normal().generic_string();
    auto normPlatforms = std::filesystem::path(platformsDir).lexically_normal().generic_string();

    return normPath.find(normPlatforms) == 0;
}

bool ComponentManager::IsBootScene() const
{
    auto* project = m_App->GetProject();
    if (!project) return false;

    const std::string& currentPath = m_App->GetCurrentScenePath();
    if (currentPath.empty()) return false;

    // Get active platform's boot scene path
    const PlatformConfig* config =
        PlatformRegistry::Instance().GetPlatform(project->GetActivePlatformId());

    if (!config || config->bootScene.empty()) return false;

    // Compare paths (normalize for comparison)
    namespace fs = std::filesystem;
    std::string bootSceneAbs = (fs::path(project->GetAssetsPath()) / config->bootScene).string();
    auto normCurrent = fs::path(currentPath).lexically_normal().generic_string();
    auto normBoot = fs::path(bootSceneAbs).lexically_normal().generic_string();

    return normCurrent == normBoot;
}

bool ComponentManager::IsPlatformScene() const
{
    auto* project = m_App->GetProject();
    if (!project) return false;

    const std::string& absPath = m_App->GetCurrentScenePath();
    if (absPath.empty()) return false;

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

    // Filtering forces matching components open (below). The moment the filter
    // clears, every component that was forced goes back to the state it had
    // before — written straight into ImGui's storage, which is where a
    // collapsing header keeps it.
    const bool filtering = InspectorUtils::PropertyFilterActive();
    if (!filtering && m_FilterWasActive)
    {
        ImGuiStorage* storage = ImGui::GetStateStorage();
        for (const auto& [key, state] : m_PreFilterOpen)
            storage->SetInt(static_cast<ImGuiID>(state.headerId), state.open ? 1 : 0);
        m_PreFilterOpen.clear();
    }
    m_FilterWasActive = filtering;

    for (size_t i = 0; i < components.size(); ++i)
    {
        Deki::Component* comp = components[i];

        ImGui::PushID(static_cast<int>(i));

        // Nothing to pull back: FullBleedSeparator charges no spacing after its
        // line, so the header already starts on it and each component reads as
        // its own window bounded by hairlines. This used to subtract a row gap
        // here to cancel the spacing the separator charged — with the separator
        // now tight, that same subtraction dragged the band 11px UP, painting
        // its background over the very hairline it was supposed to sit under.

        // Missing component — special red header, no property editing
        if (comp->GetType() == Deki::MissingComponent::StaticType)
        {
            auto* missing = static_cast<Deki::MissingComponent*>(comp);

            // Missing component header: schematic font but red emphasis to
            // signal the error state.
            ImFont* f = g_DisplaySmallFont;
            if (f) ImGui::PushFont(f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 8.0f));
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.5f, 0.1f, 0.1f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.6f, 0.15f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.7f, 0.2f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, Palette::Red);

            std::string label = "Missing: " + missing->m_TypeName;
            bool open = ImGui::CollapsingHeader(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen);

            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar();
            if (f) ImGui::PopFont();

            // Context menu — only allow remove
            PushContextMenuPopupStyle();
            if (ImGui::BeginPopupContextItem())
            {
                PushContextMenuItemStyle();
                if (ImGui::MenuItem("Remove Component"))
                {
                    CommandHistory::Instance().Execute(
                        std::make_unique<RemoveComponentCommand>(obj, comp));
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
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Component type not found");
                if (!missing->m_JsonProperties.empty())
                {
                    ImGui::TextDisabled("Data preserved (%u properties)",
                                        (unsigned)missing->m_JsonProperties.size());
                }
                else if (!missing->m_RawData.empty())
                {
                    ImGui::TextDisabled("Data preserved (%u bytes, %u fields)",
                                        (unsigned)missing->m_RawData.size(), missing->m_DataMapSize);
                }
                else
                {
                    ImGui::TextDisabled("No data");
                }
            }

            DekiEditor::FullBleedSeparator();
            ImGui::PopID();
            continue;
        }

        const Deki::ComponentMeta* meta = registry.GetMeta(comp->GetType());

        // With a filter typed, a component none of whose properties match is
        // dropped whole — header included — instead of leaving empty sections
        // to scroll past.
        if (!InspectorUtils::ComponentHasFilterMatch(meta))
        {
            ImGui::PopID();
            continue;
        }
        // Counted here, where a component survives the filter, rather than per
        // row: a header is a visible answer to the query even collapsed, and
        // even on a component that has no properties to list under it.
        if (InspectorUtils::PropertyFilterActive())
            InspectorUtils::NoteFilterHit();

        const char* compDisplayName = meta ? meta->GetDisplayName() : "Unknown Component";
        // A component that survived the filter has something to show, so it is
        // shown: forced open for as long as the filter lasts. Its own state was
        // recorded the first frame it was seen (see below) and is put back when
        // the filter clears.
        const uint64_t stateKey =
            (static_cast<uint64_t>(comp->GetType()) << 32) | static_cast<uint32_t>(i);
        const bool stateKnown = m_PreFilterOpen.find(stateKey) != m_PreFilterOpen.end();
        if (filtering && stateKnown)
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);

        // Section, not a bare header: `open` stays true while the component is
        // closing, which is when its rows still have to be submitted for the
        // slide-up. Every path out of this iteration that took the flag must
        // close the section — including the removal below, which returns.
        ImGuiID headerId = 0;
        const bool open = DekiEditor::SchematicSectionBegin(compDisplayName,
                                                            ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap, &headerId);

        // First frame this component is seen under a filter: it was NOT forced
        // above, so what ImGui holds right now is the user's own state. Record
        // it, and force from the next frame on.
        if (filtering && !stateKnown && headerId != 0)
        {
            m_PreFilterOpen[stateKey] = PreFilterState{
                static_cast<unsigned int>(headerId),
                ImGui::GetStateStorage()->GetInt(headerId, 1) != 0
            };
        }
        ImGui::OpenPopupOnItemClick("ComponentContextMenu", ImGuiPopupFlags_MouseButtonRight);

        // Right-aligned `EDIT ->` action link for package/project components.
        if (meta && IsEditableComponent(m_App, comp->GetType()))
        {
            if (DekiEditor::SchematicActionLink(("EDIT##comp_edit_" + std::to_string(i)).c_str()))
            {
                OpenComponentSourceFile(m_App, meta);
            }
        }

        // Component context menu
        PushContextMenuPopupStyle();
        if (ImGui::BeginPopup("ComponentContextMenu"))
        {
            PushContextMenuItemStyle();
            auto& clipboard = ClipboardService::Instance();
            const std::string typeName = meta ? meta->name : "";
            const std::string serializedName = meta ? meta->serializedName : "";

            if (ImGui::MenuItem("Copy Component"))
            {
                clipboard.CopyComponent(comp);
            }

            ImGui::Separator();

            bool canPasteValues = clipboard.HasComponentOfType(serializedName);
            if (ImGui::MenuItem("Paste Component Values", nullptr, false, canPasteValues))
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
                            std::make_unique<PasteComponentValuesCommand>(
                                targetComponents, compDataOpt.value()));
                    }
                }
            }

            bool canPasteAsNew = clipboard.HasComponent();
            if (ImGui::MenuItem("Paste Component as New", nullptr, false, canPasteAsNew))
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
                            targetObjects.push_back(selObj);
                    }

                    if (!targetObjects.empty())
                    {
                        CommandHistory::Instance().Execute(
                            std::make_unique<PasteComponentAsNewCommand>(
                                targetObjects, compDataOpt.value()));
                    }
                }
            }

            ImGui::Separator();

            bool protectRemove = (typeName == "PlatformSetupComponent" && IsProtectedComponent(obj));
            if (ImGui::MenuItem("Remove Component", nullptr, false, !protectRemove))
            {
                CommandHistory::Instance().Execute(
                    std::make_unique<RemoveComponentCommand>(obj, comp));
                ImGui::PopStyleVar();  // context menu ItemSpacing
                ImGui::EndPopup();
                ImGui::PopID();
                componentRemoved = true;
                ImGui::PopStyleVar();  // context menu WindowPadding
                if (open)
                    DekiEditor::SchematicSectionEnd();
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
                // A filter that named this component shows all of it. Set around
                // the rows and cleared straight after, so the next component is
                // judged on its own properties again.
                InspectorUtils::SetComponentNameMatched(
                    InspectorUtils::ComponentNameMatchesFilter(meta));
                drawComponent(comp, meta);
                InspectorUtils::SetComponentNameMatched(false);
            }
            else
            {
                ImGui::TextDisabled("(No properties)");
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
    // A search-first picker that browses in TWO levels: categories, then the
    // components inside one. Browsing beats one long list for the thing you
    // cannot name yet ("it was something under Animation"), and searching beats
    // browsing for the thing you can - so typing cuts across every level at
    // once, results grouped under their categories and ranked best-first, and
    // clearing the box drops you back where you were. Keyboard throughout:
    // type, arrow, Enter to enter a group or add a component, Left to go back.
    const float popupW = m_App->Dpi(300.0f);
    const float listMaxH = m_App->Dpi(360.0f);
    // Room for the search band, its hairline and the popup's own padding, with
    // slack: the LIST is the scroller here, and the popup has to be able to
    // hold a full-height list plus that chrome without running out of room. Cut
    // it fine and the popup starts scrolling too, which is how you end up
    // dragging one scrollbar inside another.
    const float chromeH = m_App->Dpi(140.0f);
    const float popupMaxH = listMaxH + chromeH;

    // Bound the popup's own height. On the FIRST frame an auto-resizing popup
    // has no content size yet, so without a cap ImGui works from a size that is
    // wrong and places the window accordingly.
    ImGui::SetNextWindowSizeConstraints(ImVec2(popupW, 0.0f), ImVec2(popupW, popupMaxH));

    // ...and place it ourselves, from the button's rect. ImGui's popup
    // placement needs the window's size to decide where it fits, which is
    // exactly what does not exist on the frame the popup first appears — hence
    // the first open landing off screen while every later one was fine. The cap
    // above makes the height knowable in advance, so the choice of dropping
    // below the button or flipping above it can be made on frame one.
    if (m_AddPopupAnchor.valid)
    {
        const float gap = m_App->Dpi(4.0f);
        const float margin = m_App->Dpi(8.0f);

        // Vertical: drop below the button, or flip above it when the capped
        // height would not fit. Resolved to a TOP edge here rather than left to
        // a pivot, so the same clamp can apply to both cases.
        const bool fitsBelow = m_AddPopupAnchor.bottom + popupMaxH + margin <= m_AddPopupAnchor.workBottom;
        float top = fitsBelow ? m_AddPopupAnchor.bottom + gap
                              : m_AddPopupAnchor.top - gap - popupMaxH;
        if (top + popupMaxH > m_AddPopupAnchor.workBottom - margin)
            top = m_AddPopupAnchor.workBottom - margin - popupMaxH;
        if (top < m_AddPopupAnchor.workTop + margin)
            top = m_AddPopupAnchor.workTop + margin;

        // Horizontal: the picker is wider than the panel it opens from, so left
        // aligning it with the button pushes it past the right edge — and a
        // popup that does not fit its viewport becomes a separate OS window,
        // which is how it lands off the desktop.
        float left = m_AddPopupAnchor.x;
        if (left + popupW > m_AddPopupAnchor.workRight - margin)
            left = m_AddPopupAnchor.workRight - margin - popupW;
        if (left < m_AddPopupAnchor.workLeft + margin)
            left = m_AddPopupAnchor.workLeft + margin;

        // Always, not Appearing: re-stated every frame the popup is open, so
        // even a frame that got placed before this ran is corrected on the next
        // one instead of staying wrong for as long as the popup lives.
        ImGui::SetNextWindowPos(ImVec2(left, top), ImGuiCond_Always);
    }

    PushContextMenuPopupStyle();
    // No scrollbar on the popup itself: the list inside is the one thing meant
    // to scroll, and a second scroller on the window that contains it is both
    // confusing and a sign the chrome estimate above is off.
    if (ImGui::BeginPopup("AddComponentPopup",
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
    {
        auto& registry = Deki::ComponentRegistry::Instance();

        // What the picker lists. Built fresh each frame because the set moves
        // with the object (a unique component already on it becomes unpickable)
        // and with hot reload; the picker holds no opinion about where the rows
        // came from, only how to search and browse them.
        //
        // Hidden here rather than inside the picker: abstract types cannot be
        // instantiated, scene-scoped ones only belong on their scene, and
        // settings types live in the Project Settings panel (convention: their
        // category is "Settings").
        m_AddCompMetas.clear();
        m_AddCompItems.clear();
        m_AddCompTips.clear();
        for (const auto& category : registry.GetCategories())
        {
            if (category == "Settings") continue;
            for (const auto* meta : registry.GetComponentsByCategory(category))
            {
                if (meta->isAbstract) continue;
                if (meta->bootSceneOnly && !IsBootScene()) continue;
                if (meta->platformSceneOnly && !IsPlatformScene()) continue;
                m_AddCompMetas.push_back(meta);
            }
        }

        auto isTaken = [&](const Deki::ComponentMeta* meta)
        {
            if (!meta->isUnique) return false;
            for (auto* comp : obj->GetComponents())
                if (comp->GetType() == meta->typeId) return true;
            return false;
        };

        // A deque, not a vector: the items below point INTO these strings, and a
        // vector would move them out from under those pointers as it grows.
        for (const Deki::ComponentMeta* meta : m_AddCompMetas)
        {
            const char* displayName = meta->GetDisplayName();
            DekiEditor::PickerItem item;
            item.label = displayName ? displayName : meta->name;
            item.altName = meta->name;
            item.category = meta->category;
            item.description = meta->description;  // shown under the name
            item.disabled = isTaken(meta);
            if (item.disabled)
            {
                char tip[160];
                std::snprintf(tip, sizeof(tip), "Only one %s allowed per object",
                              item.label ? item.label : "");
                m_AddCompTips.push_back(tip);
                item.disabledTip = m_AddCompTips.back().c_str();
            }
            m_AddCompItems.push_back(item);
        }

        const int chosen = DekiEditor::SchematicPicker(
            "##addcomp", "Add Component",
            m_AddCompItems.data(), static_cast<int>(m_AddCompItems.size()),
            m_PopupJustOpened, listMaxH, ICON_TI_PUZZLE,
            "Search components", "No components available");
        m_PopupJustOpened = false;

        if (chosen >= 0 && chosen < static_cast<int>(m_AddCompMetas.size()))
        {
            // Through the command system so it can be undone, like Remove
            // Component beside it. The command also invalidates the
            // behaviours cache so ProcessInternalAwake sees the new component.
            CommandHistory::Instance().Execute(
                std::make_unique<AddComponentCommand>(obj, m_AddCompMetas[chosen]->serializedName));

            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
    else
    {
        // Reset flag when popup is closed so focus is set when it reopens.
        m_PopupJustOpened = true;
    }
    ImGui::PopStyleVar();  // context menu WindowPadding
}

}  // namespace DekiEditor
