#include "ObjectInternal.h"
#include "HierarchyPanel.h"
#include "../PanelMaximize.h"
#include "EditorApp.h"
#include <deki-editor/EditorTheme.h>
#include <deki-editor/Motion.h>
#include <deki-editor/IconsTabler.h>
#include <deki/Object.h>
#include <deki/Engine.h>
#include <deki/SceneSystem.h>
#include <deki-editor/CommandHistory.h>
#include "core/Commands.h"
#include "core/Project.h"
#include "services/ClipboardService.h"
#include <deki-editor/AssetPicker.h>
#include "widgets/DragScroll.h"
#include <deki-editor/AssetPipeline.h>
#include <deki-editor/EditorExtension.h>
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <unordered_set>

namespace DekiEditor
{

namespace
{
}  // namespace

HierarchyPanel::HierarchyPanel(EditorApp* app)
    : m_App(app),
      m_AssetPicker(std::make_unique<AssetPicker>())
{
}

HierarchyPanel::~HierarchyPanel() = default;

bool HierarchyPanel::IsProtectedObject(Deki::Object* obj) const
{
    // One rule, owned by EditorApp so the Scene View applies the same one.
    return m_App->IsProtectedObject(obj);
}

void HierarchyPanel::Draw()
{
    if (!isOpen)
    {
        return;
    }

    // The panel itself never scrolls: the breadcrumb and search stay put and
    // the tree below scrolls in its own region (it used to take the search
    // field away with it).
    ImGui::Begin("HIERARCHY###Hierarchy", &isOpen,
                 DekiEditor::MaximizedWindowFlags("HIERARCHY###Hierarchy") | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse);
    auto* scene = m_App->GetCurrentScene();

    // Rows read last frame's hover (see DrawObjectNode).
    m_RowHover.BeginFrame();

    // Breadcrumb navigation header
    const auto& navStack = m_App->GetSceneNavigationStack();
    const std::string& scenePath = m_App->GetCurrentScenePath();
    std::string sceneName = scenePath.empty() ? "Untitled Scene" : std::filesystem::path(scenePath).stem().string();
    // During Play, the edit scene's state: Play's own changes are discarded on Stop.
    bool isDirty =
        m_App->IsStopped() ? CommandHistory::Instance().IsDirty() : CommandHistory::Instance().IsSavedAsideDirty();

    const float breadcrumbIndent = m_App->Dpi(16.0f);

    // 12 CSS px Inter for the breadcrumb row. CssPx applies Inter's REAL_DIM
    // ratio so the visual size matches CSS semantics (the requested ImGui
    // font size will be ~14.5 to render an em-square that looks like 12px).
    ImGui::PushFont(g_FontUI, Fonts::CssPx(g_FontUI, 12.0f));

    // Header band: the breadcrumb text is vertically centered between the
    // panel's top edge (the tab bar separator above) and the 1px divider
    // below. Equal padding top and bottom - explicit cursor positioning is
    // used instead of stacked Dummy() items so ImGui's per-item ItemSpacing
    // can't make the two gaps uneven.
    const float headerPadY = m_App->Dpi(8.0f);
    const float textH = ImGui::GetTextLineHeight();
    const ImVec2 headerStart = ImGui::GetCursorScreenPos();
    const float headerEndY = headerStart.y + headerPadY + textH + headerPadY;

    ImGui::SetCursorScreenPos(ImVec2(headerStart.x + breadcrumbIndent, headerStart.y + headerPadY));

    if (!navStack.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.5f, 0.8f, 0.3f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.3f, 0.5f, 0.8f, 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Text, DekiEditor::Palette::Accent);

        for (size_t i = 0; i < navStack.size(); i++)
        {
            std::string ancestorName = std::filesystem::path(navStack[i]).stem().string();
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::SmallButton(ancestorName.c_str()))
            {
                m_App->NavigateBackTo(i);
                ImGui::PopID();
                ImGui::PopStyleColor(4);
                ImGui::PopFont();
                ImGui::End();
                return;
            }
            ImGui::PopID();
            ImGui::SameLine(0, 2);
            ImGui::PushStyleColor(ImGuiCol_Text, DekiEditor::Palette::Dim);
            ImGui::TextUnformatted(">");
            ImGui::PopStyleColor();
            ImGui::SameLine(0, 2);
        }

        ImGui::PopStyleColor(4);

        // Current scene name (not clickable)
        std::string displayName = sceneName + (isDirty ? " *" : "");
        ImGui::TextUnformatted(displayName.c_str());
    }
    else
    {
        // Just the scene name in foreground (dirty asterisk if unsaved).
        // The earlier "scene /" prefix and right-aligned "live" indicator
        // were dropped at the user's request - only the name is shown now.
        ImGui::PushStyleColor(ImGuiCol_Text, Palette::Fg);
        std::string displayName = sceneName + (isDirty ? " *" : "");
        ImGui::TextUnformatted(displayName.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::PopFont();

    // 1px full-width horizontal divider at the bottom of the header band.
    // Pixel-aligned (ImFloor) so it stays a crisp single-row line at any DPI.
    {
        const float winLeft = ImGui::GetWindowPos().x;
        const float winRight = winLeft + ImGui::GetWindowSize().x;
        const float y = ImFloor(headerEndY);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImVec2(winLeft, y), ImVec2(winRight, y + 1.0f), ImGui::GetColorU32(ImGuiCol_Border));
    }

    // Position the layout cursor just below the breadcrumb divider - this is
    // the top of the search band.
    ImGui::SetCursorScreenPos(ImVec2(headerStart.x, ImFloor(headerEndY) + 1.0f));

    // Search band - the search row is vertically centered between the
    // breadcrumb divider above and the tree separator below. Equal padding
    // top and bottom, positioned explicitly (same approach as the header
    // band) so ItemSpacing can't skew the two gaps. The magnifier icon is
    // rendered ~2 CSS-px larger than the input text and shifted so its
    // vertical center lines up with the input text's center.
    {
        const float searchInset = m_App->Dpi(12.0f);
        const float inputSize = Fonts::CssPx(g_FontUI, 12.0f);
        const float iconSize = Fonts::CssPx(g_FontUI, 14.0f);
        const float searchPadY = m_App->Dpi(6.0f);

        // Tighter frame padding for the search input so the row isn't as tall
        // as the theme default (~10px). Pushed before measuring GetFrameHeight
        // so the band height reflects the reduced frame.
        const ImVec2 themePad = ImGui::GetStyle().FramePadding;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(themePad.x, m_App->Dpi(4.0f)));
        const float framePadY = ImGui::GetStyle().FramePadding.y;

        // Measure the *rendered* (DPI-scaled) sizes with each font pushed.
        // Cursor coords are physical pixels, so the vertical math must use the
        // scaled line/frame heights - using the base CssPx sizes here is what
        // previously threw the centering off (they're ~half the rendered size
        // at 200% DPI).
        ImGui::PushFont(g_FontUI, inputSize);
        const float inputLineH = ImGui::GetTextLineHeight();
        const float inputFrameH = ImGui::GetFrameHeight();
        ImGui::PopFont();

        ImGui::PushFont(g_FontUI, iconSize);
        const float iconLineH = ImGui::GetTextLineHeight();
        const float iconAdvance = ImGui::CalcTextSize(ICON_TI_SEARCH).x;
        ImGui::PopFont();

        const ImVec2 bandStart = ImGui::GetCursorScreenPos();
        const float bandEndY = bandStart.y + searchPadY + inputFrameH + searchPadY;
        const float rowY = bandStart.y + searchPadY;  // input frame top

        const float winLeft = ImGui::GetWindowPos().x;
        const float winRight = winLeft + ImGui::GetWindowSize().x;
        const float iconLeftX = bandStart.x + searchInset;

        // Magnifier icon, vertically centered on the input text's middle.
        // input text center = rowY + framePadY + inputLineH/2.
        const float iconCursorOffset = framePadY + (inputLineH - iconLineH) * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(iconLeftX, rowY + iconCursorOffset));
        ImGui::PushFont(g_FontUI, iconSize);
        ImGui::PushStyleColor(ImGuiCol_Text, Palette::Dim2);
        ImGui::TextUnformatted(ICON_TI_SEARCH);
        ImGui::PopStyleColor();
        ImGui::PopFont();

        // Input field on the same row, beginning after the icon + 8px gap,
        // filling to the right inset.
        const float inputLeftX = iconLeftX + iconAdvance + 8.0f;
        ImGui::SetCursorScreenPos(ImVec2(inputLeftX, rowY));
        ImGui::PushFont(g_FontUI, inputSize);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, Palette::Dim2);
        ImGui::SetNextItemWidth((winRight - searchInset) - inputLeftX);
        ImGui::InputTextWithHint("##hierarchy_search", "Search hierarchy", m_SearchBuf, sizeof(m_SearchBuf));
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();
        ImGui::PopFont();

        // Bottom separator at the band's end (same color/technique as the
        // breadcrumb divider so the two read as a matched pair).
        const float sepY = ImFloor(bandEndY);
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(winLeft, sepY), ImVec2(winRight, sepY + 1.0f),
                                                  ImGui::GetColorU32(ImGuiCol_Border));

        // Advance the layout cursor below the separator for the tree content.
        ImGui::SetCursorScreenPos(ImVec2(bandStart.x, sepY + 1.0f));

        ImGui::PopStyleVar();  // FramePadding
    }

    // Everything below the search band lives in the scrolling region, to the
    // end of Draw: keys, drops, context menus and dialogs included, so they
    // keep working on the same window as the rows.
    ImGui::BeginChild("##hierarchy_tree", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);

    // Auto-scroll while dragging items near the top/bottom edge of the panel.
    AutoScrollOnDrag();

    // Build visible flattened list during draw (for arrow key navigation)
    m_VisibleLastFrame.swap(m_FlattenedObjects);
    m_FlattenedObjects.clear();
    m_CollapsedByArrow = nullptr;
    m_ArrowKeyHandled = false;

    // Remove the vertical gap between tree rows so selection/hover highlights
    // form a continuous list (no 4px unhighlighted strip between rows). Scoped
    // to the whole tree + persistent section below, then popped.
    const ImVec2 themeItemSp = ImGui::GetStyle().ItemSpacing;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(themeItemSp.x, 0.0f));

    // Object tree - only show root objects (children are drawn recursively)
    // Make a copy since deletion may modify the list
    auto objects = scene->GetObjects();
    for (Deki::Object* obj : objects)
    {
        // Skip objects that have a parent (they'll be drawn as children)
        if (obj->GetParent() != nullptr)
        {
            continue;
        }
        if (DrawObjectNode(obj))
        {
            // Object was deleted - stop iterating to avoid invalid access
            break;
        }
    }

    // Persistent section - show persistent objects that survive scene changes
    const auto& persistentObjects = Deki::Engine::GetInstance().GetSceneSystem().GetPersistentObjects();
    if (!persistentObjects.empty())
    {
        ImGui::Separator();

        // Draw header with a distinct style
        ImGui::PushStyleColor(ImGuiCol_Text, DekiEditor::Palette::Amber);  // Yellow-ish
        bool persistentOpen =
            ImGui::TreeNodeEx("Persistent", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
        ImGui::PopStyleColor();

        if (persistentOpen)
        {
            for (Deki::Object* obj : persistentObjects)
            {
                if (obj->GetParent() != nullptr)
                {
                    continue;
                }
                DrawObjectNode(obj, true);  // Pass flag to indicate read-only
            }
            ImGui::TreePop();
        }
    }

    ImGui::PopStyleVar();  // ItemSpacing (tree row gap)

    // Keyboard edits apply only while this panel has focus and no text field
    // is being typed into. Without the guard, Ctrl+D/C/V fired from the
    // hierarchy search box, the Inspector rename field and the Console filter
    // (Ctrl+V pasted objects AND text), and Delete removed the selected object
    // while editing the search text in this very window.
    const bool keysActive = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                            !ImGui::GetIO().WantTextInput && !m_RenamingObject;

    // Ctrl+D duplicates everything selected.
    Deki::Object* selected = m_App->GetSelectedObject();
    if (keysActive && selected && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D))
    {
        m_App->DuplicateObjects(m_App->GetSelectedObjects());
    }

    // Ctrl+C to copy selected objects
    if (keysActive && selected && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C))
    {
        ClipboardService::Instance().CopyObjects(m_App->GetSelectedObjects());
    }

    // Ctrl+V to paste objects
    if (keysActive && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V))
    {
        auto& clipboard = ClipboardService::Instance();
        if (clipboard.HasObjects())
        {
            auto data = clipboard.GetObjectsData();
            if (data.has_value())
            {
                Deki::Object* parent = selected ? selected->GetParent() : nullptr;
                CommandHistory::Instance().Execute(
                    std::make_unique<PasteObjectsCommand>(scene, data.value(), parent,
                                                          [this](const std::vector<Deki::Object*>& pasted)
                                                          {
                                                              if (!pasted.empty())
                                                              {
                                                                  m_App->SetSelectedObject(pasted[0]);
                                                              }
                                                          }));
            }
        }
    }

    // Up/Down arrow keys to navigate objects (macOS-style)
    // Uses m_FlattenedObjects built during DrawObjectNode() - only includes visible (not collapsed) items
    if (ImGui::IsWindowFocused() && !m_RenamingObject &&
        (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow)))
    {
        int idx = selected ? FindObjectIndex(selected) : -1;
        Deki::Object* newSel = nullptr;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && idx > 0)
        {
            newSel = m_FlattenedObjects[idx - 1];
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && idx < (int)m_FlattenedObjects.size() - 1)
        {
            newSel = m_FlattenedObjects[idx + 1];
        }
        else if (idx == -1 && !m_FlattenedObjects.empty())
        {
            newSel = m_FlattenedObjects[0];
        }
        if (newSel)
        {
            m_App->SetSelectedObject(newSel);
            m_ScrollToObject = newSel;
            ImGui::NavMoveRequestCancel();
        }
    }

    // Delete key. EditorApp::DeleteObject applies the protection rules
    // (root, boot Setup, instance children) - the same ones the Scene View uses.
    if (keysActive && selected && ImGui::IsKeyPressed(ImGuiKey_Delete))
    {
        m_App->DeleteObjects(m_App->GetSelectedObjects());
    }

    // A search that matched nothing said nothing; say so.
    if (m_SearchBuf[0] != '\0' && m_FlattenedObjects.empty())
    {
        DekiEditor::EmptyStateText("No objects match the search.");
    }

    // Fill remaining space so the window is fully interactive
    ImVec2 availSpace = ImGui::GetContentRegionAvail();
    if (availSpace.y > 0)
    {
        ImGui::Dummy(ImVec2(-1, availSpace.y));
    }

    // Drop target for empty space - use custom rect so Dummy doesn't eat clicks
    {
        // InnerRect is what is on screen. ContentRegionRect moves with the
        // scroll, so once the list was scrolled, drops on it were ignored.
        ImRect windowRect = ImGui::GetCurrentWindow()->InnerRect;
        if (ImGui::BeginDragDropTargetCustom(windowRect, ImGui::GetCurrentWindow()->ID))
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_OBJECT"))
            {
                if (!m_DraggedObjects.empty())
                {
                    // Move all dragged objects to end of root's children
                    size_t endIndex = scene->GetObjects().empty() ? 0 : scene->GetObjects()[0]->GetChildren().size();
                    CommandHistory::Instance().Execute(
                        std::make_unique<MoveObjectsCommand>(scene, m_DraggedObjects, nullptr, endIndex));
                }
            }
            // .scene assets from the Asset Browser: instances at the end of the root.
            AcceptSceneAssetDrop(nullptr, SIZE_MAX);
            ImGui::EndDragDropTarget();
        }
    }

    // Context menu for empty space (right-click)
    PushContextMenuPopupStyle();
    if (ImGui::BeginPopupContextWindow("HierarchyContextMenu",
                                       ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
    {
        PushContextMenuItemStyle();
        auto& roots = scene->GetObjects();
        Deki::Object* sceneRoot = roots.empty() ? nullptr : roots[0];

        if (ImGui::MenuItem("Create Empty"))
        {
            CommandHistory::Instance().Execute(std::make_unique<CreateObjectCommand>(
                scene, "DekiObject", [this](Deki::Object* obj) { m_App->SetSelectedObject(obj); }, sceneRoot,
                [this]() { m_App->ClearSelection(); }));
        }

        ImGui::Separator();

        // Dynamic "Create Object" menu from CreateMenuRegistry
        {
            auto& createMenu = CreateMenuRegistry::Instance();
            const auto& entries = createMenu.GetEntries();

            // Collect unique categories in order
            std::vector<std::string> categories;
            for (const auto& entry : entries)
            {
                if (std::find(categories.begin(), categories.end(), entry.category) == categories.end())
                {
                    categories.push_back(entry.category);
                }
            }

            for (const auto& category : categories)
            {
                if (ImGui::BeginMenu(category.c_str()))
                {
                    for (const auto& entry : entries)
                    {
                        if (entry.category != category)
                        {
                            continue;
                        }

                        if (ImGui::MenuItem(entry.displayName.c_str()))
                        {
                            std::string compName = entry.componentName;
                            std::string objName = entry.objectName;
                            CommandHistory::Instance().Execute(std::make_unique<CreateObjectCommand>(
                                scene, objName,
                                [this, compName](Deki::Object* obj)
                                {
                                    Deki::Internal::AddComponentByName(*obj, compName);
                                    m_App->SetSelectedObject(obj);
                                },
                                sceneRoot, [this]() { m_App->ClearSelection(); }));
                        }
                    }
                    ImGui::EndMenu();
                }
            }
        }

        ImGui::Separator();

        if (ImGui::MenuItem("Paste", "Ctrl+V", false, ClipboardService::Instance().HasObjects()))
        {
            auto& clipboard = ClipboardService::Instance();
            auto data = clipboard.GetObjectsData();
            if (data.has_value())
            {
                CommandHistory::Instance().Execute(
                    std::make_unique<PasteObjectsCommand>(scene, data.value(), nullptr,
                                                          [this](const std::vector<Deki::Object*>& pasted)
                                                          {
                                                              if (!pasted.empty())
                                                              {
                                                                  m_App->SetSelectedObject(pasted[0]);
                                                              }
                                                          }));
            }
        }

        ImGui::PopStyleVar();  // context menu ItemSpacing
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();  // context menu WindowPadding

    // Link to Scene picker (deferred open from context menu)
    if (m_OpenLinkPicker && m_LinkTargetObject)
    {
        std::string dummy;
        m_AssetPicker->Open("LinkToScenePicker", "", AssetFilterType::Scenes, dummy);
        m_OpenLinkPicker = false;
    }

    if (m_LinkTargetObject)
    {
        std::string selectedGuid;
        bool picked = m_AssetPicker->Draw("LinkToScenePicker", selectedGuid);
        bool popupOpen = ImGui::IsPopupOpen("LinkToScenePicker");

        if (picked)
        {
            if (!selectedGuid.empty())
            {
                if (ValidateHierarchyMatch(m_LinkTargetObject, selectedGuid))
                {
                    CommandHistory::Instance().Execute(
                        std::make_unique<LinkInstanceCommand>(scene, m_LinkTargetObject, selectedGuid));
                }
                else
                {
                    DEKI_LOG_WARNING("Link to Scene: hierarchy does not match the selected scene");
                }
            }
            m_LinkTargetObject = nullptr;
        }
        else if (!popupOpen)
        {
            // Popup was closed/cancelled
            m_LinkTargetObject = nullptr;
        }
    }

    ImGui::EndChild();  // ##hierarchy_tree
    ImGui::End();

    // Process deferred navigation (after draw loop to avoid destroying scene mid-iteration)
    if (!m_PendingNavigatePath.empty())
    {
        std::string path = std::move(m_PendingNavigatePath);
        m_PendingNavigatePath.clear();
        m_App->NavigateIntoScene(path);
    }
}

// Case-insensitive "haystack contains needle" with no allocation: the search
// runs over every descendant of every node, every frame the box is non-empty,
// and used to lower-case a copy of each name (and of the needle, per node).
static bool ContainsIgnoreCase(const char* haystack, const std::string& needleLower)
{
    if (needleLower.empty())
    {
        return true;
    }
    const size_t n = needleLower.size();
    for (const char* h = haystack; *h; ++h)
    {
        size_t i = 0;
        while (i < n && h[i] && (char)std::tolower((unsigned char)h[i]) == needleLower[i])
        {
            ++i;
        }
        if (i == n)
        {
            return true;
        }
        if (!h[i])
        {
            return false;
        }
    }
    return false;
}

// True if `obj` or any descendant has a name containing `needle` (case-insensitive).
// Needle is already lowercased by the caller.
static bool SubtreeMatchesSearch(Deki::Object* obj, const std::string& needle)
{
    if (needle.empty())
    {
        return true;
    }
    if (ContainsIgnoreCase(obj->GetName().c_str(), needle))
    {
        return true;
    }
    for (Deki::Object* child : obj->GetChildren())
    {
        if (SubtreeMatchesSearch(child, needle))
        {
            return true;
        }
    }
    return false;
}

bool HierarchyPanel::DrawObjectNode(Deki::Object* obj, bool readOnly)
{
    // Search filter - collapse this subtree if neither it nor any descendant matches.
    if (m_SearchBuf[0])
    {
        // Lower-cased once per change of the search text, not once per node.
        if (m_SearchLowerSource != m_SearchBuf)
        {
            m_SearchLowerSource = m_SearchBuf;
            m_SearchLower = m_SearchLowerSource;
            std::transform(m_SearchLower.begin(), m_SearchLower.end(), m_SearchLower.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
        }
        if (!SubtreeMatchesSearch(obj, m_SearchLower))
        {
            return false;
        }
    }

    // Track visible objects for arrow key navigation
    m_FlattenedObjects.push_back(obj);

    // _FramePadding is required for our pushed FramePadding.y to actually
    // grow the row height. Without it, ImGui clamps a non-framed tree node's
    // vertical padding to ImMin(CurrLineTextBaseOffset, FramePadding.y) ≈ 0
    // (imgui_widgets.cpp:6888), so any FramePadding.y push is ignored.
    ImGuiTreeNodeFlags flags =
        ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding;

    if (obj->GetChildren().empty())
    {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }

    // The row's highlight is set where the node is drawn: it fades in and out
    // with selection and hover, so "selected" there means "highlight showing".
    const bool isSelected = m_App->IsSelected(obj);

    // Auto-expand if any selected object is a descendant of this node
    // Skip if this node was just collapsed via Left arrow key
    Deki::Object* primarySelected = m_App->GetSelectedObject();
    if (primarySelected && primarySelected != obj && primarySelected->IsDescendantOf(obj) && obj != m_CollapsedByArrow)
    {
        ImGui::SetNextItemOpen(true);
    }

    // Check if we're renaming this object
    if (m_RenamingObject == obj)
    {
        ImGui::PushID(obj);

        // Draw tree node indent space
        ImGui::TreeNodeEx("##renaming", flags | ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);

        ImGui::SameLine();

        // Set focus on first frame
        static bool s_FocusSet = false;
        if (!s_FocusSet)
        {
            ImGui::SetKeyboardFocusHere();
            s_FocusSet = true;
        }

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::InputText("##rename", m_RenameBuffer, sizeof(m_RenameBuffer),
                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
        {
            // Enter pressed - apply rename via command for undo support
            if (std::string(m_RenameBuffer) != obj->GetName())
            {
                CommandHistory::Instance().Execute(std::make_unique<RenameObjectCommand>(obj, m_RenameBuffer));
            }
            StopRenaming();
            s_FocusSet = false;
        }

        // Cancel on Escape or click outside
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) || (!ImGui::IsItemHovered() && ImGui::IsMouseClicked(0)))
        {
            StopRenaming();
            s_FocusSet = false;
        }

        ImGui::PopID();
        return false;
    }

    // Instance visual styling
    bool isInstance = obj->IsSceneInstance();
    bool isPartOfInstance = obj->IsPartOfInstance();
    bool instanceReadOnly = readOnly || isPartOfInstance;

    if (isInstance)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, DekiEditor::Palette::Accent);  // Blue for instance roots
    }
    else if (isPartOfInstance)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, DekiEditor::Palette::Dim);  // Dimmed for instance children
    }

    bool wasOpen = ImGui::GetStateStorage()->GetInt(ImGui::GetCurrentWindow()->GetID(NodeKey(obj)), 0) != 0;
    // Push extra vertical FramePadding so tree-node rows are taller. Total row
    // height = font line height + 2 * FramePadding.y. The Inter 13px default
    // font has a ~15.7 logical-px line height (REAL_DIM), so padding.y ~= 6
    // gives ~28px rows. Requires the _FramePadding flag set above.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 6.0f * m_App->Dpi(1.0f)));
    // Empty label + transparent text hides ImGui's filled triangle; we draw a
    // Tabler chevron + the name ourselves (capturing the current text color so
    // instance/child tinting is preserved). See DrawTreeNodeChevronLabel.
    ImU32 lblCol = ImGui::GetColorU32(ImGuiCol_Text);

    // An object that will not run reads as disabled: the name is faded rather
    // than recolored, so a scene instance stays blue and an instance child
    // stays gray while both still say "inactive". IsActiveInHierarchy, not
    // IsActive, because a live object under a disabled parent is just as
    // switched off - and looking active while its parent is not is exactly the
    // thing that sends you hunting for why nothing happens.
    if (!obj->IsActiveInHierarchy())
    {
        ImVec4 faded = ImGui::ColorConvertU32ToFloat4(lblCol);
        faded.w *= 0.45f;
        lblCol = ImGui::ColorConvertFloat4ToU32(faded);
    }
    // Cyan selection/hover background (the global Header is transparent, so a
    // selected row would otherwise show only the accent left border). It
    // fades: selection and hover each ease in and out per row, and the row is
    // drawn with the blend of the two. Hover is last frame's, since this
    // frame's is only known once the row is submitted - so a row entered by
    // the mouse starts from nothing and fades up instead of flashing on.
    const ImVec4 acc = DekiEditor::Palette::Accent;
    const ImGuiID rowId = ImGui::GetID(NodeKey(obj));
    float selT = 0.0f;
    const float rowAlpha = Motion::RowHighlight(rowId, isSelected, m_RowHover.WasHovered(rowId), 0.22f, 0.12f, &selT);
    if (rowAlpha > 0.0f)
    {
        flags |= ImGuiTreeNodeFlags_Selected;  // draws the (fading) background
    }
    const ImVec4 rowBg(acc.x, acc.y, acc.z, rowAlpha);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_Header, rowBg);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, rowBg);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(acc.x, acc.y, acc.z, 0.18f));
    bool open = ImGui::TreeNodeEx(NodeKey(obj), flags, "%s", "");
    // The node's id, captured before anything else is submitted: the children
    // slide open and shut against it (see SchematicTreeBodyBegin).
    const ImGuiID nodeId = ImGui::GetItemID();
    m_RowHover.Track(rowId);
    ImGui::PopStyleColor(4);
    DrawTreeNodeChevronLabel(open, !(flags & ImGuiTreeNodeFlags_Leaf), obj->GetName().c_str(), lblCol);
    ImGui::PopStyleVar();

    // Mockup-style 2px accent left border on the selected row (line 98 of
    // the React mockup). Overlay on the window draw list so it sits on top of
    // the row's bg highlight.
    if (selT > 0.0f)
    {
        const ImVec2 rmin = ImGui::GetItemRectMin();
        const ImVec2 rmax = ImGui::GetItemRectMax();
        ImVec4 bar = Palette::Accent;
        bar.w *= selT;
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(rmin.x, rmin.y), ImVec2(rmin.x + 2.0f, rmax.y),
                                                  ImGui::ColorConvertFloat4ToU32(bar));
    }

    // Sync ImGui nav focus to match our selection after arrow key navigation
    if (m_ScrollToObject == obj)
    {
        ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
        ImGui::SetScrollHereY();
        m_ScrollToObject = nullptr;
    }

    if (isInstance || isPartOfInstance)
    {
        ImGui::PopStyleColor();
    }

    // Double-click on instance root → open source scene (deferred to avoid destroying scene mid-draw)
    if (isInstance && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
    {
        std::string sourcePath =
            Deki::Scene::s_GuidToPathResolver ? Deki::Scene::s_GuidToPathResolver(obj->GetSourceSceneGuid()) : "";
        if (!sourcePath.empty())
        {
            m_PendingNavigatePath = sourcePath;
        }
    }
    // Double-click to rename (not for read-only, protected, or instance objects)
    else if (!instanceReadOnly && !IsProtectedObject(obj) && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
    {
        StartRenaming(obj);
    }

    // Enter/F2 to rename selected object (not for read-only or protected
    // objects). Only when this panel actually has focus and nothing anywhere is
    // taking typed input: the shortcut used to fire on any Enter, so committing
    // a rename in the Inspector - or in any other text field, with the object
    // still selected - immediately opened a second rename here.
    if (!readOnly && !IsProtectedObject(obj) && m_App->GetSelectedObject() == obj &&
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput &&
        (ImGui::IsKeyPressed(ImGuiKey_F2) || ImGui::IsKeyPressed(ImGuiKey_Enter)))
    {
        StartRenaming(obj);
    }

    // Left/Right arrow keys for expand/collapse (macOS-style)
    if (!m_ArrowKeyHandled && m_App->GetSelectedObject() == obj && ImGui::IsWindowFocused() && !m_RenamingObject)
    {
        bool hasChildren = !obj->GetChildren().empty();

        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow))
        {
            if (hasChildren && !open)
            {
                ImGuiID nodeId = ImGui::GetCurrentWindow()->GetID(NodeKey(obj));
                ImGui::GetStateStorage()->SetInt(nodeId, 1);
            }
            // Selection stays on current node - use Down arrow to enter children
            ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
            m_ArrowKeyHandled = true;
            ImGui::NavMoveRequestCancel();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
        {
            if (wasOpen && hasChildren)
            {
                ImGuiID nodeId = ImGui::GetCurrentWindow()->GetID(NodeKey(obj));
                ImGui::GetStateStorage()->SetInt(nodeId, 0);
                m_CollapsedByArrow = obj;
                ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                m_ScrollToObject = obj;  // Re-focus next frame after children stop drawing
            }
            else if (obj->GetParent())
            {
                m_App->SetSelectedObject(obj->GetParent());
                m_ScrollToObject = obj->GetParent();
            }
            m_ArrowKeyHandled = true;
            ImGui::NavMoveRequestCancel();
        }
    }

    // Selection on single click with Ctrl/Shift modifiers
    if (ImGui::IsItemClicked() && !ImGui::IsMouseDoubleClicked(0))
    {
        ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl)
        {
            // Ctrl+click: Toggle selection
            m_App->ToggleSelection(obj);
        }
        else if (io.KeyShift)
        {
            // Shift+click: Range selection
            Deki::Object* lastSelected = m_App->GetSelectedObject();
            if (lastSelected)
            {
                SelectRange(lastSelected, obj);
            }
            else
            {
                m_App->SetSelectedObject(obj);
            }
        }
        else if (m_App->IsSelected(obj) && m_App->GetSelectionCount() > 1)
        {
            // Clicking an already-selected object in multi-selection:
            // defer to mouse-up so drag can capture full selection first
            m_PendingClickObject = obj;
        }
        else
        {
            // Normal click: Defer to mouse-up so drag can start without changing selection
            m_PendingClickObject = obj;
        }
    }

    // Deferred single-selection on mouse-up (if no drag happened)
    if (m_PendingClickObject == obj && ImGui::IsItemHovered() && ImGui::IsMouseReleased(0) &&
        !ImGui::IsDragDropActive())
    {
        m_App->SetSelectedObject(obj);
        m_PendingClickObject = nullptr;
    }

    // Context menu (not for read-only Persistent objects)
    PushContextMenuPopupStyle();
    if (!readOnly && ImGui::BeginPopupContextItem())
    {
        PushContextMenuItemStyle();
        bool isProtected = IsProtectedObject(obj);

        // Instance-specific menu items
        if (isInstance)
        {
            if (ImGui::MenuItem("Open Source Scene"))
            {
                std::string sourcePath = Deki::Scene::s_GuidToPathResolver
                                             ? Deki::Scene::s_GuidToPathResolver(obj->GetSourceSceneGuid())
                                             : "";
                if (!sourcePath.empty())
                {
                    m_PendingNavigatePath = sourcePath;
                }
            }
            if (ImGui::MenuItem("Link to Scene..."))
            {
                m_LinkTargetObject = obj;
                m_OpenLinkPicker = true;
            }
            if (ImGui::MenuItem("Unlink Instance"))
            {
                CommandHistory::Instance().Execute(std::make_unique<UnlinkInstanceCommand>(obj));
            }
            ImGui::Separator();
        }

        // Block editing operations for instance children
        if (isPartOfInstance)
        {
            ImGui::TextDisabled("Part of Scene Instance");
            ImGui::EndPopup();
        }
        else
        {
            if (ImGui::MenuItem("Rename", "Enter", false, !isProtected && !isPartOfInstance))
            {
                StartRenaming(obj);
            }
            if (ImGui::MenuItem("Create Child", nullptr, false, !isInstance))
            {
                // Must go through the command system: the old raw `new Deki::Object` +
                // AddChild bypassed the undo stack (and scene registration), so the
                // next Ctrl+Z undid an OLDER create command and deleted the wrong
                // object.
                CommandHistory::Instance().Execute(std::make_unique<CreateObjectCommand>(
                    m_App->GetCurrentScene(), "Child", [this](Deki::Object* created)
                    { m_App->SetSelectedObject(created); }, obj, [this]() { m_App->ClearSelection(); }));
            }
            // On a multi-selection these act on all of it, as the keys do.
            if (ImGui::MenuItem("Duplicate", "Ctrl+D"))
            {
                m_App->DuplicateObjects(m_App->ActionTargets(obj));
            }
            if (ImGui::MenuItem("Copy", "Ctrl+C"))
            {
                ClipboardService::Instance().CopyObjects(m_App->GetSelectedObjects());
            }
            if (ImGui::MenuItem("Paste", "Ctrl+V", false, ClipboardService::Instance().HasObjects()))
            {
                auto& clipboard = ClipboardService::Instance();
                auto data = clipboard.GetObjectsData();
                if (data.has_value())
                {
                    auto* scene = m_App->GetCurrentScene();
                    Deki::Object* parent = obj->GetParent();
                    CommandHistory::Instance().Execute(
                        std::make_unique<PasteObjectsCommand>(scene, data.value(), parent,
                                                              [this](const std::vector<Deki::Object*>& pasted)
                                                              {
                                                                  if (!pasted.empty())
                                                                  {
                                                                      m_App->SetSelectedObject(pasted[0]);
                                                                  }
                                                              }));
                }
            }
            // Link to Scene (for objects with children, including re-linking instances)
            if (!isInstance && !obj->GetChildren().empty())
            {
                if (ImGui::MenuItem("Link to Scene..."))
                {
                    m_LinkTargetObject = obj;
                    m_OpenLinkPicker = true;
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete", "Del", false, !isProtected))
            {
                m_App->DeleteObjects(m_App->ActionTargets(obj));
                // IMPORTANT: EndPopup first to exit popup window context,
                // then TreePop to close tree node in main hierarchy window
                ImGui::PopStyleVar();  // context menu ItemSpacing
                ImGui::EndPopup();
                if (open)
                {
                    ImGui::TreePop();
                }
                ImGui::PopStyleVar();  // context menu WindowPadding
                return true;           // Signal that an object was deleted (before any body)
            }

        }  // end else (non-instance-child block)
        ImGui::PopStyleVar();  // context menu ItemSpacing
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();  // context menu WindowPadding

    // Drag source - start dragging this object (not for read-only, Persistent, or instance children)
    if (!instanceReadOnly && ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
    {
        m_PendingClickObject = nullptr;  // Cancel deferred click - drag started
        // Snapshot dragged objects: all selected if this obj is in selection, otherwise just this obj
        if (m_App->IsSelected(obj) && m_App->GetSelectionCount() > 1)
        {
            m_DraggedObjects = m_App->GetSelectedObjects();
            // Filter to topmost objects only (remove any whose ancestor is also selected)
            m_DraggedObjects.erase(std::remove_if(m_DraggedObjects.begin(), m_DraggedObjects.end(),
                                                  [this](Deki::Object* o)
                                                  {
                                                      for (Deki::Object* other : m_DraggedObjects)
                                                      {
                                                          if (other != o && o->IsDescendantOf(other))
                                                          {
                                                              return true;
                                                          }
                                                      }
                                                      return false;
                                                  }),
                                   m_DraggedObjects.end());
            // Sort by flattened order to preserve relative positioning
            std::sort(m_DraggedObjects.begin(), m_DraggedObjects.end(),
                      [this](Deki::Object* a, Deki::Object* b) { return FindObjectIndex(a) < FindObjectIndex(b); });
        }
        else
        {
            m_DraggedObjects = { obj };
        }

        ImGui::SetDragDropPayload("HIERARCHY_OBJECT", &obj, sizeof(Deki::Object*));
        if (m_DraggedObjects.size() > 1)
        {
            ImGui::Text("Move %d items", (int)m_DraggedObjects.size());
        }
        else
        {
            ImGui::Text("%s", obj->GetName().c_str());
        }
        ImGui::EndDragDropSource();
    }

    // Drop target - handle both reparenting and reordering (not for read-only, Persistent, or instance children)
    if (!instanceReadOnly && ImGui::BeginDragDropTarget())
    {
        // Calculate drop position based on mouse Y relative to item
        ImVec2 itemMin = ImGui::GetItemRectMin();
        ImVec2 itemMax = ImGui::GetItemRectMax();
        float itemHeight = itemMax.y - itemMin.y;
        float mouseY = ImGui::GetIO().MousePos.y;
        float relativeY = mouseY - itemMin.y;

        // Divide item into thirds: top = above, middle = as child, bottom = below
        enum class DropPos
        {
            Above,
            AsChild,
            Below
        };
        DropPos dropPos;
        if (relativeY < itemHeight * 0.25f)
        {
            dropPos = DropPos::Above;
        }
        else if (relativeY > itemHeight * 0.75f)
        {
            dropPos = DropPos::Below;
        }
        else
        {
            dropPos = DropPos::AsChild;
        }

        // Visual indicator - only show line for reordering, highlight for parenting
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImU32 lineColor = IM_COL32(100, 149, 237, 255);  // Cornflower blue
        float lineThickness = 2.0f;

        if (dropPos == DropPos::Above)
        {
            drawList->AddLine(ImVec2(itemMin.x, itemMin.y), ImVec2(itemMax.x, itemMin.y), lineColor, lineThickness);
        }
        else if (dropPos == DropPos::Below)
        {
            drawList->AddLine(ImVec2(itemMin.x, itemMax.y), ImVec2(itemMax.x, itemMax.y), lineColor, lineThickness);
        }
        else
        {
            // AsChild - draw a subtle highlight rectangle to indicate parenting
            ImU32 highlightColor = IM_COL32(100, 149, 237, 50);  // Semi-transparent blue
            drawList->AddRectFilled(itemMin, itemMax, highlightColor);
        }

        // Use AcceptNoDrawDefaultRect to suppress ImGui's default highlight
        if (const ImGuiPayload* payload =
                ImGui::AcceptDragDropPayload("HIERARCHY_OBJECT", ImGuiDragDropFlags_AcceptNoDrawDefaultRect))
        {
            // Filter dragged objects: remove any that are the target or an ancestor of it
            std::vector<Deki::Object*> validObjects;
            for (Deki::Object* dragObj : m_DraggedObjects)
            {
                if (dragObj != obj && !obj->IsDescendantOf(dragObj))
                {
                    validObjects.push_back(dragObj);
                }
            }

            if (!validObjects.empty())
            {
                auto* scene = m_App->GetCurrentScene();

                if (dropPos == DropPos::AsChild)
                {
                    // Reparent all as children (append to end)
                    size_t targetIndex = obj->GetChildren().size();
                    CommandHistory::Instance().Execute(
                        std::make_unique<MoveObjectsCommand>(scene, validObjects, obj, targetIndex));
                }
                else
                {
                    // Reorder: insert above or below target
                    Deki::Object* targetParent = obj->GetParent();
                    size_t targetIndex;
                    if (targetParent)
                    {
                        targetIndex = static_cast<size_t>(targetParent->GetChildIndex(obj));
                    }
                    else
                    {
                        targetIndex = static_cast<size_t>(scene->GetObjectIndex(obj));
                    }

                    if (dropPos == DropPos::Below)
                    {
                        targetIndex++;
                    }

                    CommandHistory::Instance().Execute(
                        std::make_unique<MoveObjectsCommand>(scene, validObjects, targetParent, targetIndex));
                }
            }
        }
        // A scene from the Asset Browser goes where the indicator shows.
        {
            Deki::Object* parent = obj;
            size_t index = SIZE_MAX;
            if (dropPos != DropPos::AsChild && obj->GetParent())
            {
                parent = obj->GetParent();
                index = static_cast<size_t>(parent->GetChildIndex(obj)) + (dropPos == DropPos::Below ? 1 : 0);
            }
            AcceptSceneAssetDrop(parent, index);
        }
        ImGui::EndDragDropTarget();
    }

    // Children as an animated body: they are still submitted while the node is
    // closing, which is what gives them somewhere to slide to. `open` is ImGui's
    // state and still owns its own TreePop; the flag below is the one that says
    // whether to draw.
    const bool drawChildren = DekiEditor::SchematicTreeBodyBegin(open, nodeId);
    if (drawChildren)
    {
        // Make a copy of children since deletion may modify the list
        auto children = obj->GetChildren();
        bool childReadOnly = readOnly || isInstance;  // Instance children are read-only in hierarchy
        for (Deki::Object* child : children)
        {
            if (DrawObjectNode(child, childReadOnly))
            {
                // Object was deleted - stop iterating to avoid invalid access
                DekiEditor::SchematicTreeBodyEnd();
                if (open)
                {
                    ImGui::TreePop();
                }
                return true;
            }
        }
    }
    DekiEditor::SchematicTreeBodyEnd();
    if (open)
    {
        ImGui::TreePop();
    }

    return false;
}

// A tree row's identity: the object's id, not its address. Play runs on a copy
// of the scene and Stop switches back, so addresses change while ids do not;
// keyed by address, every expanded object collapsed on Play and on Stop.
const void* HierarchyPanel::NodeKey(const Deki::Object* obj)
{
    return reinterpret_cast<const void*>(static_cast<uintptr_t>(obj->GetId()) + 1u);
}

void HierarchyPanel::AcceptSceneAssetDrop(Deki::Object* parent, size_t index)
{
    std::vector<std::string> paths;
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_MULTI"))
    {
        std::istringstream stream(std::string(static_cast<const char*>(payload->Data)));
        std::string line;
        while (std::getline(stream, line))
        {
            const size_t tab = line.find('\t');
            if (tab != std::string::npos)
            {
                paths.push_back(line.substr(tab + 1));
            }
        }
    }
    else if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_PATH"))
    {
        paths.emplace_back(static_cast<const char*>(payload->Data));
    }
    auto* pipeline = m_App->GetAssetPipeline();
    auto* scene = m_App->GetCurrentScene();
    if (paths.empty() || !pipeline || !scene)
    {
        return;
    }

    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path editing = m_App->GetCurrentScenePath().empty()
                                 ? fs::path()
                                 : fs::weakly_canonical(fs::path(m_App->GetCurrentScenePath()), ec);
    std::vector<std::unique_ptr<Command>> cmds;
    for (const std::string& relPath : paths)
    {
        if (fs::path(relPath).extension() != ".scene")
        {
            continue;
        }
        const AssetInfo* info = pipeline->GetAssetInfo(relPath);
        if (!info || info->guid.empty())
        {
            continue;
        }
        if (!editing.empty() && fs::weakly_canonical(fs::path(pipeline->GetAbsolutePath(relPath)), ec) == editing)
        {
            DEKI_LOG_WARNING("A scene cannot contain itself: '%s' is the scene being edited", relPath.c_str());
            continue;
        }
        const size_t slot = (index == SIZE_MAX) ? SIZE_MAX : index + cmds.size();
        cmds.push_back(std::make_unique<CreateInstanceCommand>(
            scene, info->guid, fs::path(relPath).stem().string(), parent,
            [this, slot](Deki::Object* created)
            {
                // Created at the end of its parent; move it to the drop slot.
                if (Deki::Object* p = created->GetParent(); p && slot != SIZE_MAX)
                {
                    p->RemoveChild(created);
                    p->InsertChildAt(created, std::min(slot, p->GetChildren().size()));
                }
                m_App->AddToSelection(created);
            },
            [this]() { m_App->ClearSelection(); }));
    }
    if (cmds.empty())
    {
        return;
    }
    m_App->ClearSelection();
    if (cmds.size() == 1)
    {
        CommandHistory::Instance().Execute(std::move(cmds[0]));
    }
    else
    {
        CommandHistory::Instance().Execute(
            std::make_unique<CompoundCommand>("Create " + std::to_string(cmds.size()) + " Instances", std::move(cmds)));
    }
}

void HierarchyPanel::StartRenaming(Deki::Object* obj)
{
    m_RenamingObject = obj;
    strncpy(m_RenameBuffer, obj->GetName().c_str(), sizeof(m_RenameBuffer) - 1);
    m_RenameBuffer[sizeof(m_RenameBuffer) - 1] = '\0';
}

void HierarchyPanel::StopRenaming()
{
    m_RenamingObject = nullptr;
    m_RenameBuffer[0] = '\0';
}

void HierarchyPanel::OnObjectsDestroyed(const std::vector<Deki::Object*>& dying)
{
    if (dying.empty())
    {
        return;
    }
    std::unordered_set<Deki::Object*> set(dying.begin(), dying.end());

    if (m_RenamingObject && set.count(m_RenamingObject))
    {
        StopRenaming();
    }
    if (m_CollapsedByArrow && set.count(m_CollapsedByArrow))
    {
        m_CollapsedByArrow = nullptr;
    }
    if (m_ScrollToObject && set.count(m_ScrollToObject))
    {
        m_ScrollToObject = nullptr;
    }
    if (m_PendingClickObject && set.count(m_PendingClickObject))
    {
        m_PendingClickObject = nullptr;
    }
    if (m_LinkTargetObject && set.count(m_LinkTargetObject))
    {
        m_LinkTargetObject = nullptr;
        m_OpenLinkPicker = false;
    }

    if (!m_DraggedObjects.empty())
    {
        m_DraggedObjects.erase(std::remove_if(m_DraggedObjects.begin(), m_DraggedObjects.end(),
                                              [&set](Deki::Object* o) { return set.count(o) > 0; }),
                               m_DraggedObjects.end());
    }

    // m_FlattenedObjects is rebuilt every Draw; last frame's copy is kept.
    m_VisibleLastFrame.erase(std::remove_if(m_VisibleLastFrame.begin(), m_VisibleLastFrame.end(),
                                            [&set](Deki::Object* o) { return set.count(o) > 0; }),
                             m_VisibleLastFrame.end());
}

void HierarchyPanel::RebuildFlattenedList()
{
    m_FlattenedObjects.clear();
    auto* scene = m_App->GetCurrentScene();

    for (Deki::Object* obj : scene->GetObjects())
    {
        // Only add root objects, children are added recursively
        if (obj->GetParent() == nullptr)
        {
            BuildFlattenedListRecursive(obj);
        }
    }
}

void HierarchyPanel::BuildFlattenedListRecursive(Deki::Object* obj)
{
    m_FlattenedObjects.push_back(obj);
    for (Deki::Object* child : obj->GetChildren())
    {
        BuildFlattenedListRecursive(child);
    }
}

int HierarchyPanel::FindObjectIndex(Deki::Object* obj) const
{
    for (size_t i = 0; i < m_FlattenedObjects.size(); ++i)
    {
        if (m_FlattenedObjects[i] == obj)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void HierarchyPanel::SelectRange(Deki::Object* from, Deki::Object* to)
{
    // The rows the user can see, not the whole tree: rebuilding the full list
    // here selected every child of a collapsed object between the two rows.
    auto indexOf = [this](Deki::Object* o) -> int
    {
        const auto it = std::find(m_VisibleLastFrame.begin(), m_VisibleLastFrame.end(), o);
        return it == m_VisibleLastFrame.end() ? -1 : static_cast<int>(it - m_VisibleLastFrame.begin());
    };
    int fromIdx = indexOf(from);
    int toIdx = indexOf(to);

    if (fromIdx < 0 || toIdx < 0)
    {
        // Fallback to single selection
        m_App->SetSelectedObject(to);
        return;
    }

    int minIdx = std::min(fromIdx, toIdx);
    int maxIdx = std::max(fromIdx, toIdx);

    std::vector<Deki::Object*> rangeSelection;
    for (int i = minIdx; i <= maxIdx; ++i)
    {
        rangeSelection.push_back(m_VisibleLastFrame[i]);
    }

    m_App->SetSelectedObjects(rangeSelection);
}

}  // namespace DekiEditor
