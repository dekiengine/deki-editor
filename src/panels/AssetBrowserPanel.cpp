#include "AssetBrowserPanel.h"
#include "asset_browser/VirtualFolders.h"
#include "asset_browser/AssetFileOps.h"
#include <deki-editor/ProcessLaunch.h>
#include "../services/AgentCursor.h"
#include "../services/ScriptTemplate.h"
#include "../services/AssetImportDrop.h"
#include "../core/SceneExport.h"
#include "../widgets/DragScroll.h"
#include "../PanelMaximize.h"
#include "asset_browser/AssetBrowserDialogs.h"
#include "EditorApp.h"
#include <deki-editor/EditorTheme.h>
#include <deki-editor/Motion.h>
#include "core/Project.h"
#include <deki-editor/TextureImporter.h>
#include <deki-editor/AssetData.h>
#include <deki-editor/Paths.h>
#include <deki-editor/AssetPipeline.h>
#include <deki-editor/AssetTypeRegistry.h>
#include <deki-editor/CommandHistory.h>
#include "core/Commands.h"
#include <deki/Object.h>
#include <deki/Scene.h>
#include "imgui.h"
#include "imgui_internal.h"
#include <sstream>
#include <deki-editor/IconsTabler.h>
#include <deki-editor/Localization.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace DekiEditor
{

namespace
{
// Progress of the Assets <-> Scripts switch: the tab sets it to 0 and the
// content eases it to 1. It is one fixed key because the two live in
// different ID scopes (the panel and its content child).
const ImGuiID kModeSlideKey = ImHashStr("AssetBrowser.ModeSlide");

// Holds a bool at true for the scope and restores it on exit, exceptions
// included. It marks "asset view is mid-iteration": a draw that throws (caught
// by SafeDispatch) must not leave the flag true, or the tree never rebuilds
// again.
struct ScopedTrue
{
    bool& flag;
    bool prev;
    explicit ScopedTrue(bool& f)
        : flag(f),
          prev(f)
    {
        flag = true;
    }
    ~ScopedTrue() { flag = prev; }
    ScopedTrue(const ScopedTrue&) = delete;
    ScopedTrue& operator=(const ScopedTrue&) = delete;
};

// Redraws the folder glyph of a TreeNodeEx label in the folder colour. Call it
// right after the TreeNodeEx, so GetItemRect* refer to that node. It draws the
// same glyph in the same place, so it only recolours; layout and clicks are
// unchanged.
void TintTreeNodeFolderIcon()
{
    ImVec2 mn = ImGui::GetItemRectMin();
    ImVec2 mx = ImGui::GetItemRectMax();
    float iconX = mn.x + ImGui::GetTreeNodeToLabelSpacing();
    float iconY = (mn.y + mx.y) * 0.5f - ImGui::GetFontSize() * 0.5f;
    ImGui::GetWindowDrawList()->AddText(ImVec2(iconX, iconY), ImGui::GetColorU32(Palette::FolderIcon), ICON_TI_FOLDER);
}

// PushContextMenuPopupStyle and PushContextMenuItemStyle come from
// EditorTheme, so every panel's context menus look the same.

// Opens the OS file explorer at the path. A file is shown selected in its
// folder; a folder is opened.
void RevealInFileExplorer(const std::string& absolutePath)
{
    // Goes through the shared launcher, which passes the path as an argument.
    // Never paste it into a shell command: a quote in a file name would run as
    // a command.
    DekiEditor::RevealInFileManager(absolutePath);
}
}  // namespace

AssetBrowserPanel::AssetBrowserPanel(EditorApp* app)
    : m_App(app),
      m_CreateFolderDialog(std::make_unique<CreateFolderDialog>(this)),
      m_CreateAssetDialog(std::make_unique<CreateAssetDialog>(app, this)),
      m_DeleteConfirmDialog(std::make_unique<DeleteConfirmDialog>(this)),
      m_RenameDialog(std::make_unique<RenameDialog>(this))
{
}

AssetBrowserPanel::~AssetBrowserPanel() = default;

void AssetBrowserPanel::Draw()
{
    if (!isOpen)
    {
        return;
    }

    ImGui::Begin(TrLabel("ASSET BROWSER###Asset Browser"), &isOpen,
                 DekiEditor::MaximizedWindowFlags("ASSET BROWSER###Asset Browser"));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 4.0f));
    m_RowHover.BeginFrame();  // rows fade their hover from last frame's state

    auto* project = m_App->GetProject();
    if (!project)
    {
        ImGui::TextDisabled("%s", Tr("No project loaded"));
        ImGui::PopStyleVar(1);
        ImGui::End();
        return;
    }

    // Set the root paths on the first draw.
    if (m_AssetsRoot.empty())
    {
        m_ProjectRoot = project->GetPath();
        m_AssetsRoot = project->GetAssetsPath();
        m_SrcRoot = project->GetSrcPath();
        m_RootPath = m_AssetsRoot;
        m_CurrentPath = m_RootPath;
        BuildFolderTree();
    }

    // Project open marks the tree dirty instead of walking the disk during
    // startup. The walk happens here, on the first frame the panel draws.
    if (m_AssetsDirty)
    {
        m_AssetsDirty = false;
        RefreshAssets();
    }

    // Assets / Scripts tabs, outside the scroll region.
    DrawModeTabs();

    // Search bar (outside the scroll region), styled like the Hierarchy search.
    if (m_BrowserMode == BrowserMode::Scripts)
    {
        DrawHierarchyStyleSearch("##scriptfilter", m_ScriptFilter, sizeof(m_ScriptFilter));
    }
    else
    {
        DrawHierarchyStyleSearch("##assetfilter", m_AssetFilter, sizeof(m_AssetFilter));
    }

    ImGui::Separator();

    // Scrollable content.
    ImGui::BeginChild("AssetBrowserContent", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);

    // Scroll while an item is dragged near the top or bottom edge.
    AutoScrollOnDrag();

    // Rows as tall as the Hierarchy's (about 28px) with no gap between them,
    // so selection and hover highlights form one continuous list.
    // FramePadding.x is the theme's (same left inset as the Hierarchy);
    // FramePadding.y scales with DPI. ItemSpacing.y is zero for the tree only,
    // not the tabs.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 6.0f * m_App->Dpi(1.0f)));
    const ImVec2 abItemSp = ImGui::GetStyle().ItemSpacing;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(abItemSp.x, 0.0f));
    // While this flag is set, an asset change made from inside the tree or
    // list (drop targets, context menus) puts off its rebuild; the Build*
    // functions check it.
    {
        ScopedTrue drawing(m_DrawingAssetView);
        // Switching between Assets and Scripts slides the new list in from its
        // tab's side (Scripts from the right, Assets from the left) as it fades
        // in, clipped by this child. Only the drawn vertices move; the rows'
        // hit areas stay in place.
        ImDrawList* contentDl = ImGui::GetWindowDrawList();
        const int contentStart = contentDl->VtxBuffer.Size;
        const bool scripts = m_BrowserMode == BrowserMode::Scripts;
        if (scripts)
        {
            DrawScriptsList();
        }
        else
        {
            DrawAssetTree();
        }
        const float t =
            DekiEditor::Motion::EaseOut(DekiEditor::Motion::To(kModeSlideKey, 1.0f, DekiEditor::Motion::kSlow));
        if (t < 1.0f)
        {
            const float slide = std::min(ImGui::GetWindowWidth() * 0.3f, 120.0f * m_App->Dpi(1.0f));
            const float dx = (scripts ? 1.0f : -1.0f) * (1.0f - t) * slide;
            const ImU32 a = static_cast<ImU32>(t * 255.0f + 0.5f);
            for (int i = contentStart; i < contentDl->VtxBuffer.Size; ++i)
            {
                ImDrawVert& v = contentDl->VtxBuffer[i];
                v.pos.x += dx;
                const ImU32 va = (v.col >> IM_COL32_A_SHIFT) & 0xFF;
                v.col = (v.col & ~IM_COL32_A_MASK) | (((va * a) / 255) << IM_COL32_A_SHIFT);
            }
        }
    }
    ImGui::PopStyleVar(2);

    // Dropping a hierarchy object on empty space saves it as a scene in the
    // current folder.
    if (m_BrowserMode != BrowserMode::Scripts)
    {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        if (avail.y > 0)
        {
            ImGui::InvisibleButton("##asset_browser_drop_area", ImVec2(avail.x, avail.y));
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_OBJECT"))
                {
                    Deki::Object* obj = *static_cast<Deki::Object**>(const_cast<void*>(payload->Data));
                    if (obj)
                    {
                        std::string folder = m_CurrentPath;
                        m_DeferredActions.push_back([this, obj, folder]() { SaveObjectAsScene(obj, folder); });
                    }
                }
                ImGui::EndDragDropTarget();
            }
        }
    }

    // Dropping a script on empty space takes it out of its virtual folder.
    if (m_BrowserMode == BrowserMode::Scripts)
    {
        // The target covers the whole child, so all empty space accepts it.
        if (ImGui::BeginDragDropTargetCustom(ImGui::GetCurrentWindow()->ContentRegionRect,
                                             ImGui::GetID("##scripts_child_drop")))
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCRIPT_VFOLDER"))
            {
                std::string relPath(static_cast<const char*>(payload->Data));
                MoveScriptToFolder(relPath, "");
            }
            ImGui::EndDragDropTarget();
        }
    }

    // Inside the child window, so right-clicking empty space opens it.
    DrawContextMenu();

    ImGui::EndChild();

    // Run the actions queued during the tree/list draw. The iteration is over,
    // so their side effects may rebuild the tree.
    RunDeferredActions();

    // A rebuild put off during the draw.
    if (m_PendingTreeRebuild)
    {
        m_PendingTreeRebuild = false;
        BuildFolderTree();
        if (m_BrowserMode == BrowserMode::Scripts)
        {
            BuildScriptFlatList();
        }
    }

    m_CreateFolderDialog->Draw();
    m_CreateAssetDialog->Draw();
    m_DeleteConfirmDialog->Draw();
    m_RenameDialog->Draw();

    // Folder delete confirmation.
    if (!m_PendingDeleteFolderPath.empty())
    {
        ImGui::OpenPopup(TrLabel("Delete Folder?"));
    }
    {
        if (BeginSchematicModal(TrLabel("Delete Folder?"), 400.0f))
        {
            namespace fs = std::filesystem;
            std::string folderName = fs::path(m_PendingDeleteFolderPath).filename().string();
            ImGui::TextWrapped(Tr("Delete the folder '%s'?"), folderName.c_str());
            ImGui::TextColored(Palette::Dim, "%s", Tr("Ctrl+Z brings it back."));

            const int button = SchematicDialogButtons({ { "Cancel" }, { "Delete", DialogButton::Danger } });
            if (button == 1)
            {
                std::string folderPath = m_PendingDeleteFolderPath;
                m_PendingDeleteFolderPath.clear();
                // Trash goes inside the project: rename cannot cross volumes,
                // so a %TEMP% on another drive would fail every move silently.
                auto command = std::make_unique<DeleteAssetsCommand>(
                    std::vector<std::string>{ folderPath }, [this]() { BuildFolderTree(); }, m_ProjectRoot);
                CommandHistory::Instance().Execute(std::move(command));
                m_PendingTreeRebuild = true;
                ImGui::CloseCurrentPopup();
            }
            else if (button == 0)
            {
                m_PendingDeleteFolderPath.clear();
                ImGui::CloseCurrentPopup();
            }
            EndSchematicModal();
        }
    }

    // Folder rename.
    if (!m_PendingRenameFolderPath.empty())
    {
        ImGui::OpenPopup(TrLabel("Rename Folder"));
    }
    if (BeginSchematicModal(TrLabel("Rename Folder"), 440.0f))
    {
        namespace fs = std::filesystem;
        std::string oldName = fs::path(m_PendingRenameFolderPath).filename().string();
        ImGui::Text(Tr("Rename folder '%s'."), oldName.c_str());
        ImGui::Spacing();
        ImGui::TextColored(Palette::Dim, "%s", Tr("New name"));

        // Request focus before the input. Asking after it
        // (SetKeyboardFocusHere(-1)) does nothing on a modal's first frame, and
        // typing then goes nowhere.
        if (ImGui::IsWindowAppearing())
        {
            ImGui::SetKeyboardFocusHere();
        }

        ImGui::SetNextItemWidth(-FLT_MIN);
        bool enterPressed = ImGui::InputText("##renamefolder", m_RenameFolderBuffer, sizeof(m_RenameFolderBuffer),
                                             ImGuiInputTextFlags_EnterReturnsTrue);

        ImGui::Spacing();
        ImGui::Spacing();

        bool canRename = m_RenameFolderBuffer[0] != '\0' && std::string(m_RenameFolderBuffer) != oldName;

        float availW = ImGui::GetContentRegionAvail().x;
        const float gap = m_App->Dpi(8.0f);
        const float btnW = (availW - gap) * 0.5f;

        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
        if (ImGui::Button(TrLabel("Cancel"), ImVec2(btnW, 0)))
        {
            m_PendingRenameFolderPath.clear();
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine(0.0f, gap);

        if (!canRename)
        {
            ImGui::BeginDisabled();
        }
        ImGui::PushStyleColor(ImGuiCol_Button, Palette::AccentDim);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Palette::Accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Palette::Accent);

        if (ImGui::Button(TrLabel("Rename"), ImVec2(btnW, 0)) || (enterPressed && canRename))
        {
            RenameAssetWithData(m_PendingRenameFolderPath, m_RenameFolderBuffer);
            m_PendingRenameFolderPath.clear();
            m_PendingTreeRebuild = true;
            ImGui::CloseCurrentPopup();
        }

        ImGui::PopStyleColor(3);
        if (!canRename)
        {
            ImGui::EndDisabled();
        }
        ImGui::PopStyleVar();  // ButtonTextAlign

        EndSchematicModal();
    }

    // After a dragged-in object is saved as a scene, offer to link it.
    if (m_ShowLinkPrompt)
    {
        ImGui::OpenPopup(TrLabel("Link to Scene?"));
        m_ShowLinkPrompt = false;
    }

    if (BeginSchematicModal(TrLabel("Link to Scene?"), 400.0f))
    {
        ImGui::TextWrapped("%s", Tr("Link this object to the scene just created from it?"));

        const int button =
            SchematicDialogButtons({ { TrMark("Don't Link") }, { TrMark("Link"), DialogButton::Primary } });
        if (button == 1)
        {
            if (m_PendingLinkObject && !m_PendingLinkSceneGuid.empty())
            {
                auto* scene = m_App->GetCurrentScene();
                if (scene)
                {
                    CommandHistory::Instance().Execute(
                        std::make_unique<LinkInstanceCommand>(scene, m_PendingLinkObject, m_PendingLinkSceneGuid));
                }
            }
            m_PendingLinkObject = nullptr;
            m_PendingLinkSceneGuid.clear();
            ImGui::CloseCurrentPopup();
        }
        else if (button == 0)
        {
            m_PendingLinkObject = nullptr;
            m_PendingLinkSceneGuid.clear();
            ImGui::CloseCurrentPopup();
        }
        EndSchematicModal();
    }

    // Create Script.
    if (m_ShowCreateScriptPopup)
    {
        ImGui::OpenPopup(TrLabel("Create Script"));
        m_ShowCreateScriptPopup = false;
    }

    if (BeginSchematicModal(TrLabel("Create Script"), 440.0f))
    {
        ImGui::TextUnformatted(Tr("Create a new C++ script."));
        ImGui::Spacing();
        ImGui::TextColored(Palette::Dim, "%s", Tr("Script name"));

        // Request focus before the input. Asking after it
        // (SetKeyboardFocusHere(-1)) does nothing on a modal's first frame, and
        // typing then goes nowhere.
        if (ImGui::IsWindowAppearing())
        {
            ImGui::SetKeyboardFocusHere();
        }

        ImGui::SetNextItemWidth(-FLT_MIN);
        bool enterPressed = ImGui::InputText("##scriptname", m_NewScriptName, sizeof(m_NewScriptName),
                                             ImGuiInputTextFlags_EnterReturnsTrue);

        ImGui::Spacing();
        ImGui::Spacing();

        bool canCreate = m_NewScriptName[0] != '\0';

        float availW = ImGui::GetContentRegionAvail().x;
        const float gap = m_App->Dpi(8.0f);
        const float btnW = (availW - gap) * 0.5f;

        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
        if (ImGui::Button(TrLabel("Cancel"), ImVec2(btnW, 0)))
        {
            m_NewScriptName[0] = '\0';
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine(0.0f, gap);

        if (!canCreate)
        {
            ImGui::BeginDisabled();
        }
        ImGui::PushStyleColor(ImGuiCol_Button, Palette::AccentDim);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Palette::Accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Palette::Accent);

        if (ImGui::Button(TrLabel("Create"), ImVec2(btnW, 0)) || (enterPressed && canCreate))
        {
            // The Scripts tab always creates in the src root.
            std::string scriptDir = (m_BrowserMode == BrowserMode::Scripts) ? m_SrcRoot : m_CurrentPath;
            CreateScript(scriptDir, m_NewScriptName);
            m_NewScriptName[0] = '\0';
            if (m_BrowserMode == BrowserMode::Scripts)
            {
                BuildScriptFlatList();
            }
            else
            {
                NavigateTo(m_CurrentPath);
            }
            ImGui::CloseCurrentPopup();
        }

        ImGui::PopStyleColor(3);
        if (!canCreate)
        {
            ImGui::EndDisabled();
        }
        ImGui::PopStyleVar();  // ButtonTextAlign

        EndSchematicModal();
    }

    ImGui::PopStyleVar(1);
    ImGui::End();
}

void AssetBrowserPanel::DrawModeTabs()
{
    namespace fs = std::filesystem;

    // A row of two text links, not an ImGui tab bar: the active one has a 2px
    // accent underline, the inactive one brightens on hover, and an item count
    // sits on the right.
    const float dpi = m_App->Dpi(1.0f);
    const float rowH = 34.0f * dpi;
    const float padX = 14.0f * dpi;
    const float gapX = 18.0f * dpi;

    const ImVec2 rowMin = ImGui::GetCursorScreenPos();
    const float avail = ImGui::GetContentRegionAvail().x;
    const ImVec2 rowMax(rowMin.x + avail, rowMin.y + rowH);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 colLine = ImGui::ColorConvertFloat4ToU32(DekiEditor::Palette::Line2);
    const ImU32 colAccent = ImGui::ColorConvertFloat4ToU32(DekiEditor::Palette::Accent);
    const ImU32 colFg = ImGui::ColorConvertFloat4ToU32(DekiEditor::Palette::Fg);
    const ImU32 colDim = ImGui::ColorConvertFloat4ToU32(DekiEditor::Palette::Dim);

    float activeX0 = 0.0f, activeX1 = 0.0f;  // the active tab's span, for its underline
    auto drawLink = [&](const char* label, bool active, float& cursorX) -> bool
    {
        const char* shown = Tr(label);
        ImVec2 textSize = ImGui::CalcTextSize(shown);
        const float linkX0 = cursorX;
        const float linkX1 = linkX0 + textSize.x;
        ImGui::SetCursorScreenPos(ImVec2(linkX0, rowMin.y));
        ImGui::PushID(label);
        bool clicked = ImGui::InvisibleButton(label, ImVec2(textSize.x, rowH));
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const ImU32 col = active ? colFg : (hovered ? colFg : colDim);
        const float textY = rowMin.y + (rowH - textSize.y) * 0.5f;
        dl->AddText(ImVec2(linkX0, textY), col, shown);
        if (active)
        {
            activeX0 = linkX0;
            activeX1 = linkX1;
        }
        cursorX = linkX1;
        return clicked;
    };

    float cx = rowMin.x + padX;
    if (drawLink(TrMark("Assets"), m_BrowserMode == BrowserMode::Assets, cx) && m_BrowserMode != BrowserMode::Assets)
    {
        DekiEditor::Motion::Set(kModeSlideKey, 0.0f);
        m_BrowserMode = BrowserMode::Assets;
        m_RootPath = m_AssetsRoot;
        m_CurrentPath = m_RootPath;
        BuildFolderTree();
    }
    cx += gapX;
    if (drawLink(TrMark("Scripts"), m_BrowserMode == BrowserMode::Scripts, cx) && m_BrowserMode != BrowserMode::Scripts)
    {
        DekiEditor::Motion::Set(kModeSlideKey, 0.0f);
        m_BrowserMode = BrowserMode::Scripts;
        m_RootPath = m_SrcRoot;
        if (!m_RootPath.empty() && !fs::exists(m_RootPath))
        {
            fs::create_directories(m_RootPath);
        }
        m_CurrentPath = m_RootPath;
        BuildScriptFlatList();
    }

    // The active tab's underline glides to whichever tab is picked. Positions
    // are kept relative to the row, so moving the panel does not animate it.
    if (activeX1 > activeX0)
    {
        const float x0 = DekiEditor::Motion::Follow(ImHashStr("AssetBrowser.TabLine0"), activeX0 - rowMin.x,
                                                    DekiEditor::Motion::kNormal, 0.5f);
        const float x1 = DekiEditor::Motion::Follow(ImHashStr("AssetBrowser.TabLine1"), activeX1 - rowMin.x,
                                                    DekiEditor::Motion::kNormal, 0.5f);
        dl->AddRectFilled(ImVec2(rowMin.x + x0, rowMax.y - 2.0f), ImVec2(rowMin.x + x1, rowMax.y), colAccent);
    }

    // Count of the files in the current folder, on the right. With none it
    // shows nothing: beside a tree of folders, "0 items" reads as an empty
    // project.
    if (!m_Entries.empty())
    {
        char buf[64];
        snprintf(buf, sizeof(buf), m_Entries.size() == 1 ? Tr("%zu item") : Tr("%zu items"), m_Entries.size());
        if (DekiEditor::g_FontMonoSmall)
        {
            ImGui::PushFont(DekiEditor::g_FontMonoSmall, DekiEditor::Fonts::CssPx(DekiEditor::g_FontMonoSmall, 11.5f));
        }
        ImVec2 sz = ImGui::CalcTextSize(buf);
        const float countX = rowMax.x - sz.x - padX;
        const float countY = rowMin.y + (rowH - sz.y) * 0.5f;
        dl->AddText(ImVec2(countX, countY), ImGui::ColorConvertFloat4ToU32(DekiEditor::Palette::Dim2), buf);
        if (DekiEditor::g_FontMonoSmall)
        {
            ImGui::PopFont();
        }
    }

    // 1px hairline along the bar's bottom edge.
    dl->AddLine(ImVec2(rowMin.x, rowMax.y), ImVec2(rowMax.x, rowMax.y), colLine, 1.0f);

    // Reserve the row in the layout so the tree goes below it.
    ImGui::SetCursorScreenPos(ImVec2(rowMin.x, rowMax.y));
}

void AssetBrowserPanel::RefreshAssets()
{
    auto* project = m_App->GetProject();
    if (!project)
    {
        m_RootPath.clear();
        m_ProjectRoot.clear();
        m_AssetsRoot.clear();
        m_SrcRoot.clear();
        m_CurrentPath.clear();
        m_Entries.clear();
        m_RootNode = FolderNode();
        m_SelectedAsset = nullptr;
        m_SelectedIndex = -1;
        return;
    }

    m_ProjectRoot = project->GetPath();
    m_AssetsRoot = project->GetAssetsPath();
    m_SrcRoot = project->GetSrcPath();

    if (m_BrowserMode == BrowserMode::Scripts)
    {
        m_RootPath = m_SrcRoot;
    }
    else
    {
        m_RootPath = m_AssetsRoot;
    }

    if (m_CurrentPath.empty())
    {
        m_CurrentPath = m_RootPath;
    }

    if (m_BrowserMode == BrowserMode::Scripts)
    {
        BuildScriptFlatList();
    }
    else
    {
        BuildFolderTree();
    }
}

void AssetBrowserPanel::NavigateTo(const std::string& path)
{
    // Dialogs call this after creating something; it rebuilds the tree.
    m_CurrentPath = path;
    BuildFolderTree();
}

void AssetBrowserPanel::RevealAsset(const std::string& absolutePath)
{
    namespace fs = std::filesystem;

    // Normalize to match the tree's paths (native separators).
    fs::path normalized = fs::weakly_canonical(fs::path(absolutePath));
    fs::path assetDir = normalized.parent_path();
    fs::path root = fs::weakly_canonical(fs::path(m_RootPath));

    // Expand every folder from the root down to the asset's directory.
    for (fs::path p = assetDir; p != root && p.has_parent_path() && p != p.parent_path(); p = p.parent_path())
    {
        m_ExpandedFolders.insert(p.string());
    }
    m_ExpandedFolders.insert(m_RootPath);

    // The next draw selects it.
    m_PendingRevealPath = normalized.string();
}

void AssetBrowserPanel::BuildFolderTree()
{
    namespace fs = std::filesystem;

    // This frees m_RootNode's vectors. When DrawAssetTree is iterating them
    // (an action in the tree set off onAssetCreated -> RefreshAssets -> here),
    // leave the rebuild to Draw(), after the iteration.
    if (m_DrawingAssetView)
    {
        m_PendingTreeRebuild = true;
        return;
    }

    if (m_SelectedAsset && m_SelectedIndex >= 0 && m_SelectedIndex < static_cast<int>(m_Entries.size()))
    {
        m_ReselectPath = m_Entries[m_SelectedIndex].path;
    }

    m_RootNode = FolderNode();
    m_Entries.clear();
    m_SelectedAsset = nullptr;
    m_SelectedIndex = -1;
    m_LastClickedIndex = -1;
    m_SelectedIndices.clear();

    if (m_RootPath.empty() || !fs::exists(m_RootPath))
    {
        return;
    }

    m_RootNode.name = (m_BrowserMode == BrowserMode::Scripts) ? "src" : "assets";
    m_RootNode.path = m_RootPath;

    BuildFolderTreeRecursive(m_RootNode, fs::path(m_RootPath));

    // The root folder starts expanded.
    m_ExpandedFolders.insert(m_RootPath);
}

void AssetBrowserPanel::BuildFolderTreeRecursive(FolderNode& node, const std::filesystem::path& dirPath)
{
    namespace fs = std::filesystem;

    if (!fs::exists(dirPath) || !fs::is_directory(dirPath))
    {
        return;
    }

    std::vector<FolderNode> subfolders;
    std::vector<AssetEntry> files;

    for (const auto& entry : fs::directory_iterator(dirPath))
    {
        std::string name = entry.path().filename().string();

        // Skip hidden files.
        if (name[0] == '.')
        {
            continue;
        }

        if (entry.is_directory())
        {
            // The Scripts tab hides the generated folder.
            if (m_BrowserMode == BrowserMode::Scripts && name == "generated")
            {
                continue;
            }

            FolderNode subfolder;
            subfolder.name = name;
            subfolder.path = entry.path().string();
            BuildFolderTreeRecursive(subfolder, entry.path());
            subfolders.push_back(std::move(subfolder));
        }
        else
        {
            std::string ext = entry.path().extension().string();

            // Skip metadata files.
            if (ext == ".dekiguid" || ext == ".data")
            {
                continue;
            }

            // The Scripts tab hides PluginExports.cpp.
            if (m_BrowserMode == BrowserMode::Scripts && name == "PluginExports.cpp")
            {
                continue;
            }

            AssetEntry asset;
            asset.name = name;
            asset.path = entry.path().string();
            asset.relativePath = fs::relative(entry.path(), m_ProjectRoot).string();
            // Forward slashes.
            for (char& c : asset.relativePath)
            {
                if (c == '\\')
                {
                    c = '/';
                }
            }
            asset.isDirectory = false;

            // GUID and cache state.
            if (IsTextureFile(ext))
            {
                asset.guid = ReadAssetGuid(asset.path);
                if (!asset.guid.empty())
                {
                    auto* project = m_App->GetProject();
                    if (project)
                    {
                        asset.cachePath = GetCachePath(project->GetPath(), asset.guid);
                        asset.hasCachedVersion = fs::exists(asset.cachePath);
                    }
                }

                // Spritesheet frames make it expandable.
                auto* pipeline = m_App->GetAssetPipeline();
                if (pipeline && !asset.guid.empty())
                {
                    const auto* subAssets = pipeline->GetSubAssets(asset.guid);
                    if (subAssets && !subAssets->empty())
                    {
                        asset.isExpandable = true;
                        asset.isExpanded = m_ExpandedAssets.count(asset.guid) > 0;
                    }
                }
            }
            else if (ext == ".asset")
            {
                asset.guid = ReadAssetGuid(asset.path);
                if (!asset.guid.empty())
                {
                    auto* project = m_App->GetProject();
                    if (project)
                    {
                        asset.cachePath = GetCachePath(project->GetPath(), asset.guid);
                        asset.hasCachedVersion = fs::exists(asset.cachePath);
                    }
                }
            }
            else if (ext == ".ttf" || ext == ".otf")
            {
                asset.guid = ReadAssetGuid(asset.path);

                // Baked font variants are its sub-assets.
                auto* pipeline = m_App->GetAssetPipeline();
                if (pipeline && !asset.guid.empty())
                {
                    const auto* subAssets = pipeline->GetSubAssets(asset.guid);
                    if (subAssets && !subAssets->empty())
                    {
                        asset.isExpandable = true;
                        asset.isExpanded = m_ExpandedAssets.count(asset.guid) > 0;
                        asset.hasCachedVersion = true;  // baked variants count as cached
                    }
                }
            }
            else if (ext == ".bdf")
            {
                asset.guid = ReadAssetGuid(asset.path);

                auto* pipeline = m_App->GetAssetPipeline();
                if (pipeline && !asset.guid.empty())
                {
                    const auto* subAssets = pipeline->GetSubAssets(asset.guid);
                    if (subAssets && !subAssets->empty())
                    {
                        asset.isExpandable = true;
                        asset.isExpanded = m_ExpandedAssets.count(asset.guid) > 0;
                        asset.hasCachedVersion = true;  // baked variants count as cached
                    }
                }
            }
            else if (ext == ".scene" || ext == ".anim")
            {
                asset.guid = ReadAssetGuid(asset.path);
                if (!asset.guid.empty())
                {
                    auto* project = m_App->GetProject();
                    if (project)
                    {
                        asset.cachePath = GetCachePath(project->GetPath(), asset.guid);
                        asset.hasCachedVersion = fs::exists(asset.cachePath);
                    }
                }
            }

            files.push_back(std::move(asset));
        }
    }

    // Sort by name.
    std::sort(subfolders.begin(), subfolders.end(),
              [](const FolderNode& a, const FolderNode& b) { return a.name < b.name; });
    std::sort(files.begin(), files.end(), [](const AssetEntry& a, const AssetEntry& b) { return a.name < b.name; });

    node.subfolders = std::move(subfolders);
    node.files = std::move(files);
}

void AssetBrowserPanel::DrawHierarchyStyleSearch(const char* id, char* buf, size_t bufSize)
{
    // The shared search field from EditorTheme, also used by the Package
    // Manager.
    DekiEditor::SearchField(id, buf, bufSize);
}

// True if `name` (case-insensitive) contains the already-lowercased needle.
static bool AssetNameMatches(const std::string& name, const std::string& needleLower)
{
    if (needleLower.empty())
    {
        return true;
    }
    std::string n = name;
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return n.find(needleLower) != std::string::npos;
}

// True if any file under the folder, at any depth, matches.
static bool FolderHasMatchingFile(const FolderNode& node, const std::string& needleLower)
{
    for (const auto& f : node.files)
    {
        if (AssetNameMatches(f.name, needleLower))
        {
            return true;
        }
    }
    for (const auto& sub : node.subfolders)
    {
        if (FolderHasMatchingFile(sub, needleLower))
        {
            return true;
        }
    }
    return false;
}

void AssetBrowserPanel::DrawAssetTree()
{
    // Lowercase the filter once per frame for the node draw.
    m_AssetFilterLower.clear();
    if (m_AssetFilter[0])
    {
        m_AssetFilterLower = m_AssetFilter;
        std::transform(m_AssetFilterLower.begin(), m_AssetFilterLower.end(), m_AssetFilterLower.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
    }

    m_HoveredFolderPath.clear();

    // The selection is kept as row numbers into m_Entries, and the rows are
    // rebuilt every frame. A search or an opened folder shifts them, so note
    // which assets were selected and select them again in the new rows.
    auto keyOf = [](const AssetEntry& e) { return e.guid + "|" + e.path; };
    std::vector<std::string> selectedKeys;
    std::string primaryKey, anchorKey;
    for (int idx : m_SelectedIndices)
    {
        if (idx >= 0 && idx < (int)m_Entries.size())
        {
            selectedKeys.push_back(keyOf(m_Entries[idx]));
        }
    }
    if (m_SelectedIndex >= 0 && m_SelectedIndex < (int)m_Entries.size())
    {
        primaryKey = keyOf(m_Entries[m_SelectedIndex]);
    }
    if (m_LastClickedIndex >= 0 && m_LastClickedIndex < (int)m_Entries.size())
    {
        anchorKey = keyOf(m_Entries[m_LastClickedIndex]);
    }
    const std::set<int> indicesBefore = m_SelectedIndices;
    const int primaryBefore = m_SelectedIndex;

    m_Entries.clear();  // the flat row list, rebuilt by the draw for selection
    int flatIndex = 0;
    DrawFolderNode(m_RootNode, flatIndex);
    if (!m_AssetFilterLower.empty() && m_Entries.empty())
    {
        DekiEditor::EmptyStateText(Tr("No assets match the search."));
    }

    // Nothing was clicked this frame: put the selection back on its assets.
    if (!selectedKeys.empty() && m_SelectedIndices == indicesBefore && m_SelectedIndex == primaryBefore)
    {
        m_SelectedIndices.clear();
        m_SelectedIndex = -1;
        m_LastClickedIndex = -1;
        m_SelectedAsset = nullptr;
        for (size_t i = 0; i < m_Entries.size(); ++i)
        {
            const std::string key = keyOf(m_Entries[i]);
            if (std::find(selectedKeys.begin(), selectedKeys.end(), key) != selectedKeys.end())
            {
                m_SelectedIndices.insert(static_cast<int>(i));
            }
            if (key == primaryKey)
            {
                m_SelectedIndex = static_cast<int>(i);
                m_SelectedAsset = &m_Entries[i];
            }
            if (key == anchorKey)
            {
                m_LastClickedIndex = static_cast<int>(i);
            }
        }
    }

    // A tree rebuild cleared the selection; put it back on the same asset.
    if (!m_ReselectPath.empty())
    {
        for (size_t i = 0; i < m_Entries.size(); ++i)
        {
            if (m_Entries[i].path == m_ReselectPath)
            {
                m_SelectedIndex = static_cast<int>(i);
                m_LastClickedIndex = m_SelectedIndex;
                m_SelectedAsset = &m_Entries[i];
                m_SelectedIndices.clear();
                m_SelectedIndices.insert(m_SelectedIndex);
                break;
            }
        }
        m_ReselectPath.clear();
    }

    // Build the multi-drag payload here, once m_Entries is complete. Inside
    // DrawFileNode the list is still partial.
    m_CachedMultiDragPayload.clear();
    if (m_SelectedIndices.size() > 1)
    {
        for (int idx : m_SelectedIndices)
        {
            if (idx >= 0 && idx < static_cast<int>(m_Entries.size()))
            {
                auto& e = m_Entries[idx];
                m_CachedMultiDragPayload += e.guid + "\t" + e.relativePath + "\n";
            }
        }
    }

    // Keyboard navigation.
    if (ImGui::IsWindowFocused() && !m_InlineRenamingFolder && !m_Entries.empty())
    {
        namespace fs = std::filesystem;

        // Delete and Ctrl+D, as the context menu lists them.
        if (ImGui::IsKeyPressed(ImGuiKey_Delete) && !m_SelectedIndices.empty())
        {
            m_DeleteConfirmDialog->Open();
        }
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D) && m_SelectedIndices.size() == 1)
        {
            const int idx = *m_SelectedIndices.begin();
            if (idx >= 0 && idx < (int)m_Entries.size())
            {
                m_DeferredActions.push_back([this, p = m_Entries[idx].path]() { DuplicateAsset(p); });
            }
        }

        // Enter, or F2 as elsewhere, renames the selected asset.
        if ((ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_F2)) && m_SelectedIndices.size() == 1)
        {
            int idx = *m_SelectedIndices.begin();
            if (idx >= 0 && idx < (int)m_Entries.size())
            {
                std::string currentName = fs::path(m_Entries[idx].name).stem().string();
                m_RenameDialog->Open(idx, currentName, m_Entries[idx].isDirectory);
            }
        }

        // Up and Down move the selection.
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && m_SelectedIndex > 0)
        {
            int newIdx = m_SelectedIndex - 1;
            m_SelectedIndices.clear();
            m_SelectedIndices.insert(newIdx);
            m_SelectedIndex = newIdx;
            m_LastClickedIndex = newIdx;
            m_SelectedAsset = &m_Entries[newIdx];
            auto& e = m_Entries[newIdx];
            m_App->SetSelectedAsset(e.path, e.cachePath, e.hasCachedVersion, e.guid);
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && m_SelectedIndex < (int)m_Entries.size() - 1)
        {
            int newIdx = m_SelectedIndex + 1;
            m_SelectedIndices.clear();
            m_SelectedIndices.insert(newIdx);
            m_SelectedIndex = newIdx;
            m_LastClickedIndex = newIdx;
            m_SelectedAsset = &m_Entries[newIdx];
            auto& e = m_Entries[newIdx];
            m_App->SetSelectedAsset(e.path, e.cachePath, e.hasCachedVersion, e.guid);
        }
        else if (m_SelectedIndex < 0 &&
                 (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow)))
        {
            m_SelectedIndices.clear();
            m_SelectedIndices.insert(0);
            m_SelectedIndex = 0;
            m_LastClickedIndex = 0;
            m_SelectedAsset = &m_Entries[0];
            auto& e = m_Entries[0];
            m_App->SetSelectedAsset(e.path, e.cachePath, e.hasCachedVersion, e.guid);
        }
    }
}

void AssetBrowserPanel::DrawFolderNode(FolderNode& node, int& flatIndex)
{
    namespace fs = std::filesystem;

    // While searching, hide folders (other than the root) with no match inside.
    const bool filtering = !m_AssetFilterLower.empty();
    if (filtering && node.path != m_RootPath && !FolderHasMatchingFile(node, m_AssetFilterLower))
    {
        return;
    }

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                               ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding;

    bool hasChildren = !node.subfolders.empty() || !node.files.empty();
    if (!hasChildren)
    {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }

    // While searching, matching folders are forced open so the results show.
    bool isExpanded = m_ExpandedFolders.count(node.path) > 0;
    if (isExpanded || filtering)
    {
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    }

    ImGui::PushID(node.path.c_str());

    const ImU32 folderLblCol = ImGui::GetColorU32(ImGuiCol_Text);
    // Cyan hover background, fading in and out. The theme's Header colour is
    // transparent.
    const ImVec4 acc = DekiEditor::Palette::Accent;
    const ImGuiID folderRowId = ImGui::GetID("##folder");
    const float folderAlpha =
        DekiEditor::Motion::RowHighlight(folderRowId, false, m_RowHover.WasHovered(folderRowId), 0.22f, 0.12f);
    if (folderAlpha > 0.0f)
    {
        flags |= ImGuiTreeNodeFlags_Selected;  // draws the (fading) background
    }
    const ImVec4 folderBg(acc.x, acc.y, acc.z, folderAlpha);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));  // hide ImGui triangle
    ImGui::PushStyleColor(ImGuiCol_Header, folderBg);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, folderBg);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(acc.x, acc.y, acc.z, 0.18f));
    bool open = ImGui::TreeNodeEx("##folder", flags, "%s", "");
    const ImGuiID nodeId = ImGui::GetItemID();  // for the animated subtree below
    m_RowHover.Track(folderRowId);
    ImGui::PopStyleColor(4);
    std::string folderLabel = std::string(ICON_TI_FOLDER " ") + node.name;
    DrawTreeNodeChevronLabel(open, !(flags & ImGuiTreeNodeFlags_Leaf), folderLabel.c_str(), folderLblCol);
    TintTreeNodeFolderIcon();

    // Remember the user's expand state, but not while a search forces folders
    // open, so clearing the search brings the old state back.
    if (!filtering && open != isExpanded)
    {
        if (open)
        {
            m_ExpandedFolders.insert(node.path);
        }
        else
        {
            m_ExpandedFolders.erase(node.path);
        }
    }

    // Hovered folder, the target for drops from outside the editor.
    if (ImGui::IsItemHovered())
    {
        m_HoveredFolderPath = node.path;
    }

    // Folders can be dragged, except the root.
    if (node.path != m_RootPath && ImGui::BeginDragDropSource())
    {
        ImGui::SetDragDropPayload("FOLDER_PATH", node.path.c_str(), node.path.size() + 1);
        ImGui::TextColored(Palette::FolderIcon, ICON_TI_FOLDER);
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::Text(" %s", node.name.c_str());
        ImGui::EndDragDropSource();
    }

    if (ImGui::BeginDragDropTarget())
    {
        // Every change below is queued, not done now: this fires while
        // DrawFolderNode iterates the tree, and a move here would rebuild it
        // under the live iterator. RunDeferredActions runs them after EndChild.
        // The payload lives only this frame, so the queued lambdas capture
        // copies.
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_MULTI"))
        {
            // Several assets: one "guid\tpath" line each.
            std::string multiData(static_cast<const char*>(payload->Data));
            std::istringstream stream(multiData);
            std::string line;
            while (std::getline(stream, line))
            {
                if (line.empty())
                {
                    continue;
                }
                size_t tabPos = line.find('\t');
                if (tabPos == std::string::npos)
                {
                    continue;
                }
                std::string relPath = line.substr(tabPos + 1);
                std::string srcPath = (fs::path(m_ProjectRoot) / relPath).string();
                fs::path dest = fs::path(node.path) / fs::path(srcPath).filename();
                if (fs::path(srcPath) != dest.parent_path())
                {
                    DeferMove(srcPath, dest.string());
                }
            }
            m_PendingTreeRebuild = true;
        }
        else if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_PATH"))
        {
            std::string droppedRelPath(static_cast<const char*>(payload->Data));
            std::string droppedPath = (fs::path(m_ProjectRoot) / droppedRelPath).string();

            fs::path src(droppedPath);
            fs::path dest = fs::path(node.path) / src.filename();
            if (src != dest.parent_path())
            {
                DeferMove(droppedPath, dest.string());
            }
            m_PendingTreeRebuild = true;
        }
        else if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FOLDER_PATH"))
        {
            std::string srcFolderPath(static_cast<const char*>(payload->Data));
            fs::path src(srcFolderPath);
            fs::path nodePath(node.path);
            fs::path dest = nodePath / src.filename();

            std::string srcNorm = src.lexically_normal().string();
            std::string nodeNorm = nodePath.lexically_normal().string();
            std::string parentNorm = src.parent_path().lexically_normal().string();

            bool isSelf = (srcNorm == nodeNorm);
            bool isDescendant = nodeNorm.size() > srcNorm.size() && nodeNorm.compare(0, srcNorm.size(), srcNorm) == 0 &&
                                (nodeNorm[srcNorm.size()] == '/' || nodeNorm[srcNorm.size()] == '\\');
            bool isNoOp = (parentNorm == nodeNorm);

            if (!isSelf && !isDescendant && !isNoOp && !fs::exists(dest))
            {
                DeferMove(srcFolderPath, dest.string());
                m_ExpandedFolders.erase(srcFolderPath);
            }
            m_PendingTreeRebuild = true;
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_OBJECT"))
        {
            Deki::Object* obj = *static_cast<Deki::Object**>(const_cast<void*>(payload->Data));
            if (obj)
            {
                std::string folder = node.path;
                m_DeferredActions.push_back([this, obj, folder]() { SaveObjectAsScene(obj, folder); });
            }
        }
        ImGui::EndDragDropTarget();
    }

    // Folder context menu, styled like the menu bar's dropdowns.
    PushContextMenuPopupStyle();
    if (ImGui::BeginPopupContextItem())
    {
        PushContextMenuItemStyle();
        if (ImGui::MenuItem(TrLabel("New Folder")))
        {
            m_CurrentPath = node.path;
            m_CreateFolderDialog->Open();
        }

        if (m_BrowserMode == BrowserMode::Scripts)
        {
            if (ImGui::MenuItem(TrLabel("Create C++ Script")))
            {
                m_CurrentPath = node.path;
                m_ShowCreateScriptPopup = true;
            }
        }
        else
        {
            // Asset types that can be created, under a "Create..." submenu.
            const auto& creatableTypes = AssetTypeRegistry::Instance().GetTypes();
            if (!creatableTypes.empty() && ImGui::BeginMenu(TrLabel("Create...")))
            {
                for (const auto& type : creatableTypes)
                {
                    // Images, fonts and the like are imported, not created: a
                    // type with no starting content would make an empty, broken
                    // file.
                    if (type.defaultContent.empty())
                    {
                        continue;
                    }
                    if (ImGui::MenuItem(type.displayName.c_str()))
                    {
                        m_CurrentPath = node.path;
                        m_CreateAssetDialog->Open(type.typeName);
                    }
                }
                ImGui::EndMenu();
            }
        }

        ImGui::Separator();
        if (ImGui::MenuItem(TrLabel("Reveal in File Explorer")))
        {
            RevealInFileExplorer(node.path);
        }

        // Rename and Delete, except on the root.
        if (node.path != m_RootPath)
        {
            ImGui::Separator();
            if (ImGui::MenuItem(TrLabel("Rename"), "Enter"))
            {
                m_PendingRenameFolderPath = node.path;
                std::string folderName = fs::path(node.path).filename().string();
                strncpy(m_RenameFolderBuffer, folderName.c_str(), sizeof(m_RenameFolderBuffer) - 1);
                m_RenameFolderBuffer[sizeof(m_RenameFolderBuffer) - 1] = '\0';
            }
            if (ImGui::MenuItem(TrLabel("Delete"), "Del"))
            {
                m_PendingDeleteFolderPath = node.path;
            }
        }

        ImGui::PopStyleVar();  // context menu ItemSpacing
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();  // context menu WindowPadding

    // Subfolders and files, in an animated body. The ID scope is popped after
    // the subtree, so a folder's children keep one identity whether it is
    // open, closing or open again.
    const bool drawChildren = DekiEditor::SchematicTreeBodyBegin(open, nodeId);
    if (drawChildren)
    {
        // Subfolders first, then files.
        for (auto& subfolder : node.subfolders)
        {
            DrawFolderNode(subfolder, flatIndex);
        }

        for (auto& file : node.files)
        {
            // While searching, only files whose name matches.
            if (filtering && !AssetNameMatches(file.name, m_AssetFilterLower))
            {
                continue;
            }

            m_Entries.push_back(file);
            DrawFileNode(file, flatIndex);
            flatIndex++;

            // Sub-assets of an expanded file. Reads the live expand state, not
            // the copy in the entry.
            if (file.isExpandable && m_ExpandedAssets.count(file.guid) > 0)
            {
                auto* pipeline = m_App->GetAssetPipeline();
                if (pipeline)
                {
                    const auto* subAssets = pipeline->GetSubAssets(file.guid);
                    if (subAssets)
                    {
                        for (const auto& subAsset : *subAssets)
                        {
                            AssetEntry frameEntry;
                            if (subAsset.name.empty())
                            {
                                char frameName[64];
                                snprintf(frameName, sizeof(frameName), Tr("Frame %d"),
                                         static_cast<int>(subAsset.subAssetIndex));
                                frameEntry.name = frameName;
                            }
                            else
                            {
                                frameEntry.name = subAsset.name;
                            }
                            frameEntry.path = file.path;
                            frameEntry.relativePath = file.relativePath;
                            frameEntry.guid = subAsset.guid;
                            frameEntry.isDirectory = false;
                            frameEntry.isSubAsset = true;
                            frameEntry.parentGuid = subAsset.parentGuid;
                            frameEntry.subAssetIndex = subAsset.subAssetIndex;

                            frameEntry.subAssetDepth = subAsset.depth;

                            if (!subAsset.cachePath.empty())
                            {
                                // The sub-asset has its own cache file.
                                frameEntry.cachePath = subAsset.cachePath;
                                frameEntry.hasCachedVersion = fs::exists(subAsset.cachePath);
                            }
                            else
                            {
                                frameEntry.cachePath = file.cachePath;
                                frameEntry.hasCachedVersion = file.hasCachedVersion;
                            }

                            m_Entries.push_back(frameEntry);
                            DrawFileNode(frameEntry, flatIndex);
                            flatIndex++;
                        }
                    }
                }
            }
        }
    }
    DekiEditor::SchematicTreeBodyEnd();
    if (open)
    {
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void AssetBrowserPanel::DrawFileNode(AssetEntry& entry, int flatIndex)
{
    namespace fs = std::filesystem;

    // Files indent by arrowIndent so their icons line up with folder names.
    // Expandable files draw their arrow in that space.
    float arrowIndent = ImGui::GetTreeNodeToLabelSpacing();
    ImGui::Indent(arrowIndent);

    ImGui::PushID(entry.path.c_str());
    if (entry.isSubAsset)
    {
        ImGui::PushID(entry.subAssetIndex);
    }

    // Icon by file type.
    std::string icon;
    std::string ext = fs::path(entry.path).extension().string();

    if (entry.isSubAsset)
    {
        icon = ICON_TI_FRAME " ";
    }
    else if (IsSourceFile(ext))
    {
        icon = ICON_TI_FILE_CODE " ";
    }
    else if (IsHeaderFile(ext))
    {
        icon = ICON_TI_FILE_TEXT " ";
    }
    else if (ext == ".ttf" || ext == ".otf" || ext == ".dfont")
    {
        icon = ICON_TI_TYPOGRAPHY " ";
    }
    else if (ext == ".bdf")
    {
        icon = ICON_TI_FILE_INFO " ";
    }
    else if (ext == ".scene")
    {
        icon = ICON_TI_BOX " ";
    }
    else if (entry.hasCachedVersion)
    {
        icon = ICON_TI_PHOTO " ";
    }
    else if (!entry.guid.empty())
    {
        icon = ICON_TI_PHOTO_OFF " ";
    }
    else
    {
        icon = ICON_TI_FILE " ";
    }

    bool drawExpandArrow = entry.isExpandable;
    float arrowOffset = 0.0f;
    if (entry.isExpandable)
    {
        // Un-indent so the selectable covers the arrow too.
        float arrowSpace = ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.x;
        ImGui::Unindent(arrowSpace);
        arrowOffset = arrowSpace;
    }
    else if (entry.isSubAsset)
    {
        ImGui::Indent(ImGui::GetStyle().IndentSpacing * (1 + entry.subAssetDepth));
    }

    // Select a pending reveal. The file name is compared first, so the
    // filesystem call runs only for a likely row, not for every row.
    if (!m_PendingRevealPath.empty() && fs::path(entry.path).filename() == fs::path(m_PendingRevealPath).filename() &&
        fs::weakly_canonical(fs::path(entry.path)) == fs::path(m_PendingRevealPath))
    {
        m_SelectedIndices.clear();
        m_SelectedIndices.insert(flatIndex);
        m_SelectedIndex = flatIndex;
        m_LastClickedIndex = flatIndex;
        m_SelectedAsset = &m_Entries[flatIndex];
        m_PendingRevealPath.clear();
        ImGui::SetScrollHereY(0.5f);
    }

    bool isSelected = m_SelectedIndices.count(flatIndex) > 0;
    std::string label = icon + entry.name;

    float rowHeight = ImGui::GetFrameHeight();
    ImVec2 selectablePos = ImGui::GetCursorScreenPos();
    // Cyan selection and hover background, fading in and out. The theme's
    // Header colour is transparent.
    const ImVec4 acc = DekiEditor::Palette::Accent;
    const ImGuiID fileRowId = ImGui::GetID("##file");
    const float fileAlpha =
        DekiEditor::Motion::RowHighlight(fileRowId, isSelected, m_RowHover.WasHovered(fileRowId), 0.22f, 0.12f);
    const ImVec4 fileBg(acc.x, acc.y, acc.z, fileAlpha);
    ImGui::PushStyleColor(ImGuiCol_Header, fileBg);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, fileBg);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(acc.x, acc.y, acc.z, 0.18f));
    const bool fileClicked =
        ImGui::Selectable("##file", fileAlpha > 0.0f, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, rowHeight));
    m_RowHover.Track(fileRowId);
    ImGui::PopStyleColor(3);
    // The agent pointer's asset. Compared like a pending reveal above: the
    // file name first, so the filesystem call runs only for a likely row.
    if (const AgentIntent* want = AgentCursor::Get().Wanted();
        want && want->target == AgentIntent::Target::Asset && !want->assetPath.empty() &&
        fs::path(entry.path).filename() == fs::path(want->assetPath).filename() &&
        fs::weakly_canonical(fs::path(entry.path)) == fs::weakly_canonical(fs::path(want->assetPath)))
    {
        AgentCursor::Get().Report(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), AgentCursor::Precision::Exact);
        if (AgentCursor::Get().WantsReveal() && !ImGui::IsItemVisible())
        {
            ImGui::SetScrollHereY(0.5f);
        }
    }
    if (fileClicked)
    {
        bool shiftHeld = ImGui::GetIO().KeyShift;
        bool ctrlHeld = ImGui::GetIO().KeyCtrl;

        if (shiftHeld && m_LastClickedIndex >= 0)
        {
            int start = (std::min)(m_LastClickedIndex, flatIndex);
            int end = (std::max)(m_LastClickedIndex, flatIndex);
            m_SelectedIndices.clear();
            for (int j = start; j <= end; ++j)
            {
                m_SelectedIndices.insert(j);
            }
        }
        else if (ctrlHeld)
        {
            if (m_SelectedIndices.count(flatIndex) > 0)
            {
                m_SelectedIndices.erase(flatIndex);
            }
            else
            {
                m_SelectedIndices.insert(flatIndex);
            }
            m_LastClickedIndex = flatIndex;
        }
        else if (isSelected && m_SelectedIndices.size() > 1)
        {
            // A click on an item of a multi-selection keeps the selection, so
            // it can be dragged. It shrinks to this item on release (below).
            m_DeferredSingleSelect = flatIndex;
        }
        else
        {
            m_SelectedIndices.clear();
            m_SelectedIndices.insert(flatIndex);
            m_LastClickedIndex = flatIndex;
        }

        m_SelectedIndex = flatIndex;
        m_SelectedAsset = &m_Entries[flatIndex];
        m_App->SetSelectedAsset(entry.path, entry.cachePath, entry.hasCachedVersion, entry.guid);

        // Double-click opens the file, except on the arrow.
        bool clickInArrow = drawExpandArrow && ImGui::GetIO().MousePos.x < selectablePos.x + arrowOffset;
        if (ImGui::IsMouseDoubleClicked(0) && !clickInArrow)
        {
            if (entry.isSubAsset && entry.hasCachedVersion)
            {
                m_App->GetToolHost()->OpenToolForFile(entry.path.c_str(), entry.cachePath.c_str());
            }
            else if (IsSceneFile(ext))
            {
                m_App->OpenScene(entry.path);
            }
            else if (IsAnimFile(ext) || ext == ".asset")
            {
                m_App->GetToolHost()->OpenToolForFile(entry.path.c_str());
            }
            else if (IsTextureFile(ext))
            {
                m_App->GetToolHost()->OpenToolForFile(entry.path.c_str(), entry.cachePath.c_str());
            }
            else if (IsSourceFile(ext) || IsHeaderFile(ext))
            {
                OpenInCodeEditor(entry.path);
            }
            else
            {
                // Any registered EditorWindow for this extension.
                m_App->GetToolHost()->OpenToolForFile(entry.path.c_str());
            }
        }
    }

    // Icon and name, centred vertically, and the expand arrow if any.
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
        float centerY = selectablePos.y + rowHeight * 0.5f;

        if (drawExpandArrow)
        {
            bool expanded = m_ExpandedAssets.count(entry.guid) > 0;
            float arrowSize = ImGui::GetFontSize();
            float arrowX = selectablePos.x;
            // Same chevron as folders, the Hierarchy and Inspector headers.
            const char* chev = expanded ? ICON_TI_CHEVRON_DOWN : ICON_TI_CHEVRON_RIGHT;
            ImFont* chevFont = g_FontUI ? g_FontUI : ImGui::GetFont();
            const ImVec2 chevSz = chevFont->CalcTextSizeA(arrowSize, FLT_MAX, 0.0f, chev);
            dl->AddText(chevFont, arrowSize, ImVec2(arrowX + (arrowSize - chevSz.x) * 0.5f, centerY - chevSz.y * 0.5f),
                        IM_COL32(165, 166, 164, 255), chev);

            // Clicking the arrow expands or collapses.
            ImVec2 mousePos = ImGui::GetIO().MousePos;
            if (ImGui::IsMouseClicked(0) && mousePos.x >= arrowX && mousePos.x < arrowX + arrowSize &&
                mousePos.y >= selectablePos.y && mousePos.y < selectablePos.y + rowHeight)
            {
                if (expanded)
                {
                    m_ExpandedAssets.erase(entry.guid);
                }
                else
                {
                    m_ExpandedAssets.insert(entry.guid);
                }
            }
        }

        // Icon, after the arrow if there is one.
        float contentX = selectablePos.x + arrowOffset;
        ImVec2 iconSize = ImGui::CalcTextSize(icon.c_str());
        float iconY = centerY - iconSize.y * 0.5f;
        dl->AddText(ImVec2(contentX, iconY), textCol, icon.c_str());

        float textX = contentX + iconSize.x;
        float textY = centerY - ImGui::GetFontSize() * 0.5f;
        dl->AddText(ImVec2(textX, textY), textCol, entry.name.c_str());
    }

    if (entry.isExpandable)
    {
        ImGui::Indent(arrowOffset);  // undo the un-indent for the arrow
    }
    else if (entry.isSubAsset)
    {
        ImGui::Unindent(ImGui::GetStyle().IndentSpacing * (1 + entry.subAssetDepth));
    }

    // File context menu, styled like the menu bar's dropdowns.
    PushContextMenuPopupStyle();
    if (!entry.isSubAsset && ImGui::BeginPopupContextItem())
    {
        PushContextMenuItemStyle();
        // Right-clicking outside the selection selects only this item.
        if (m_SelectedIndices.count(flatIndex) == 0)
        {
            m_SelectedIndices.clear();
            m_SelectedIndices.insert(flatIndex);
            m_SelectedIndex = flatIndex;
            m_LastClickedIndex = flatIndex;
            m_SelectedAsset = &m_Entries[flatIndex];
            m_App->SetSelectedAsset(entry.path, entry.cachePath, entry.hasCachedVersion, entry.guid);
        }

        if (IsTextureFile(ext) && entry.hasCachedVersion)
        {
            if (ImGui::MenuItem(TrLabel("Edit Spritesheet...")))
            {
                m_App->GetToolHost()->OpenToolForFile(entry.path.c_str(), entry.cachePath.c_str());
            }
            ImGui::Separator();
        }

        if (m_SelectedIndices.size() == 1)
        {
            if (ImGui::MenuItem(TrLabel("Rename"), "Enter"))
            {
                std::string currentName = fs::path(entry.name).stem().string();
                m_RenameDialog->Open(flatIndex, currentName, false);
            }
        }

        if (m_SelectedIndices.size() == 1)
        {
            if (ImGui::MenuItem(TrLabel("Duplicate"), "Ctrl+D"))
            {
                m_DeferredActions.push_back([this, p = entry.path]() { DuplicateAsset(p); });
            }
        }

        if (m_SelectedIndices.size() == 1)
        {
            if (ImGui::MenuItem(TrLabel("Reveal in File Explorer")))
            {
                RevealInFileExplorer(entry.path);
            }
        }

        ImGui::Separator();

        if (m_SelectedIndices.size() > 1)
        {
            char menuLabel[64];
            snprintf(menuLabel, sizeof(menuLabel), Tr("Delete %zu items"), m_SelectedIndices.size());
            if (ImGui::MenuItem(menuLabel, "Del"))
            {
                m_DeleteConfirmDialog->Open();
            }
        }
        else
        {
            if (ImGui::MenuItem(TrLabel("Delete"), "Del"))
            {
                m_DeleteConfirmDialog->Open();
            }
        }

        ImGui::PopStyleVar();  // context menu ItemSpacing
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();  // context menu WindowPadding

    // Released without dragging: the selection shrinks to this item.
    if (m_DeferredSingleSelect == flatIndex && ImGui::IsItemDeactivated() && !ImGui::IsDragDropActive())
    {
        m_SelectedIndices.clear();
        m_SelectedIndices.insert(flatIndex);
        m_LastClickedIndex = flatIndex;
        m_SelectedIndex = flatIndex;
        m_SelectedAsset = &m_Entries[flatIndex];
        m_App->SetSelectedAsset(entry.path, entry.cachePath, entry.hasCachedVersion, entry.guid);
        m_DeferredSingleSelect = -1;
    }

    if (!entry.isSubAsset && ImGui::BeginDragDropSource())
    {
        m_DeferredSingleSelect = -1;  // dragging started, keep the selection

        if (m_SelectedIndices.size() > 1 && m_SelectedIndices.count(flatIndex) > 0 && !m_CachedMultiDragPayload.empty())
        {
            // The payload was built after the last full tree draw.
            ImGui::SetDragDropPayload("ASSET_MULTI", m_CachedMultiDragPayload.c_str(),
                                      m_CachedMultiDragPayload.size() + 1);
            ImGui::Text(Tr("Drag %zu items"), m_SelectedIndices.size());
        }
        else
        {
            if (!entry.guid.empty())
            {
                ImGui::SetDragDropPayload("ASSET_GUID", entry.guid.c_str(), entry.guid.size() + 1);
            }
            ImGui::SetDragDropPayload("ASSET_PATH", entry.relativePath.c_str(), entry.relativePath.size() + 1);
            ImGui::Text(Tr("Drag %s"), entry.name.c_str());
        }
        ImGui::EndDragDropSource();
    }
    else if (entry.isSubAsset && !entry.guid.empty() && ImGui::BeginDragDropSource())
    {
        ImGui::SetDragDropPayload("ASSET_GUID", entry.guid.c_str(), entry.guid.size() + 1);
        ImGui::Text(Tr("Drag %s"), entry.name.c_str());
        ImGui::EndDragDropSource();
    }

    if (entry.isSubAsset)
    {
        ImGui::PopID();
    }
    ImGui::PopID();
    ImGui::Unindent(arrowIndent);
}

void AssetBrowserPanel::BuildScriptFlatList()
{
    namespace fs = std::filesystem;

    // This frees the vectors DrawScriptsList iterates, so mid-draw the
    // rebuild waits (as in BuildFolderTree).
    if (m_DrawingAssetView)
    {
        m_PendingTreeRebuild = true;
        return;
    }

    m_ScriptFlatList.clear();
    m_Entries.clear();
    m_SelectedAsset = nullptr;
    m_SelectedIndex = -1;
    m_LastClickedIndex = -1;
    m_SelectedIndices.clear();

    if (m_SrcRoot.empty() || !fs::exists(m_SrcRoot))
    {
        return;
    }

    // Load the virtual folders once.
    if (m_VirtualFolders.Names().empty() && m_VirtualFolders.Assignments().empty() && !m_VirtualFolders.Dirty())
    {
        m_VirtualFolders.Load(m_ProjectRoot);
    }

    // Every source and header file under src/.
    for (const auto& entry : fs::recursive_directory_iterator(m_SrcRoot))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }

        std::string name = entry.path().filename().string();

        // Skip hidden files.
        if (name[0] == '.')
        {
            continue;
        }

        // Skip the generated folder.
        std::string relToSrc = fs::relative(entry.path(), m_SrcRoot).string();
        for (char& c : relToSrc)
        {
            if (c == '\\')
            {
                c = '/';
            }
        }
        if (relToSrc.rfind("generated/", 0) == 0 || relToSrc.rfind("generated\\", 0) == 0)
        {
            continue;
        }

        if (name == "PluginExports.cpp")
        {
            continue;
        }

        std::string ext = entry.path().extension().string();
        if (!IsSourceFile(ext) && !IsHeaderFile(ext))
        {
            continue;
        }

        ScriptEntry script;
        script.name = name;
        script.path = entry.path().string();
        script.relativePath = relToSrc;
        m_ScriptFlatList.push_back(std::move(script));
    }

    std::sort(m_ScriptFlatList.begin(), m_ScriptFlatList.end(),
              [](const ScriptEntry& a, const ScriptEntry& b) { return a.name < b.name; });

    // Forget folder assignments of files that are gone.
    m_VirtualFolders.PruneMissing(
        [this](const std::string& rel)
        {
            for (const auto& s : m_ScriptFlatList)
            {
                if (s.relativePath == rel)
                {
                    return true;
                }
            }
            return false;
        });
}

void AssetBrowserPanel::MoveScriptToFolder(const std::string& relativePath, const std::string& folder)
{
    m_VirtualFolders.Assign(relativePath, folder);
    m_VirtualFolders.Save(m_ProjectRoot);
    m_VirtualFolders.ClearDirty();
}

void AssetBrowserPanel::DrawScriptsList()
{
    namespace fs = std::filesystem;

    m_HoveredFolderPath.clear();
    m_Entries.clear();

    // Lowercase the filter; matching ignores case.
    std::string filterLower;
    if (m_ScriptFilter[0] != '\0')
    {
        filterLower = m_ScriptFilter;
        for (char& c : filterLower)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }

    // Matches the file name or its path.
    auto passesFilter = [&](const ScriptEntry& script) -> bool
    {
        if (filterLower.empty())
        {
            return true;
        }
        std::string nameLower = script.name;
        for (char& c : nameLower)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        std::string relLower = script.relativePath;
        for (char& c : relLower)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return nameLower.find(filterLower) != std::string::npos || relLower.find(filterLower) != std::string::npos;
    };

    auto getVirtualFolder = [&](const ScriptEntry& script) -> std::string
    {
        auto it = m_VirtualFolders.Assignments().find(script.relativePath);
        if (it != m_VirtualFolders.Assignments().end())
        {
            return it->second;
        }
        return "";
    };

    int flatIndex = 0;
    auto drawScript = [&](const ScriptEntry& script)
    {
        AssetEntry entry;
        entry.name = script.name;
        entry.path = script.path;
        entry.relativePath = "src/" + script.relativePath;
        entry.isDirectory = false;
        entry.hasCachedVersion = false;
        m_Entries.push_back(entry);
        DrawFileNode(m_Entries.back(), flatIndex);

        // Drag a script onto a virtual folder to move it there.
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
        {
            ImGui::SetDragDropPayload("SCRIPT_VFOLDER", script.relativePath.c_str(), script.relativePath.size() + 1);
            ImGui::Text(Tr("Move %s"), script.name.c_str());
            ImGui::EndDragDropSource();
        }

        // Script context menu, styled like the menu bar's dropdowns.
        PushContextMenuPopupStyle();
        if (ImGui::BeginPopupContextItem(("##scriptctx_" + script.relativePath).c_str()))
        {
            PushContextMenuItemStyle();
            // Right-clicking outside the selection selects only this item.
            if (m_SelectedIndices.count(flatIndex) == 0)
            {
                m_SelectedIndices.clear();
                m_SelectedIndices.insert(flatIndex);
                m_SelectedIndex = flatIndex;
                m_LastClickedIndex = flatIndex;
                m_SelectedAsset = &m_Entries[flatIndex];
            }

            if (!m_VirtualFolders.Names().empty() && ImGui::BeginMenu(TrLabel("Move to Virtual Folder")))
            {
                if (ImGui::MenuItem(TrLabel("(root)")))
                {
                    for (int si : m_SelectedIndices)
                    {
                        if (si < (int)m_Entries.size())
                        {
                            std::string rel = m_Entries[si].relativePath;
                            if (rel.rfind("src/", 0) == 0)
                            {
                                rel = rel.substr(4);
                            }
                            MoveScriptToFolder(rel, "");
                        }
                    }
                }
                for (const auto& vf : m_VirtualFolders.Names())
                {
                    if (ImGui::MenuItem(vf.c_str()))
                    {
                        for (int si : m_SelectedIndices)
                        {
                            if (si < (int)m_Entries.size())
                            {
                                std::string rel = m_Entries[si].relativePath;
                                if (rel.rfind("src/", 0) == 0)
                                {
                                    rel = rel.substr(4);
                                }
                                MoveScriptToFolder(rel, vf);
                            }
                        }
                    }
                }
                ImGui::EndMenu();
            }

            if (m_SelectedIndices.size() == 1)
            {
                std::string ext = fs::path(script.path).extension().string();
                if (ImGui::MenuItem(TrLabel("Rename"), "Enter"))
                {
                    std::string currentName = fs::path(script.name).stem().string();
                    m_RenameDialog->Open(flatIndex, currentName, false);
                }
            }

            if (m_SelectedIndices.size() == 1)
            {
                if (ImGui::MenuItem(TrLabel("Reveal in File Explorer")))
                {
                    RevealInFileExplorer(script.path);
                }
            }

            ImGui::Separator();

            if (ImGui::MenuItem(TrLabel("Delete"), "Del"))
            {
                m_DeleteConfirmDialog->Open();
            }

            ImGui::PopStyleVar();  // context menu ItemSpacing
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();  // context menu WindowPadding

        flatIndex++;
    };

    // While searching, a flat list without folders.
    if (!filterLower.empty())
    {
        for (const auto& script : m_ScriptFlatList)
        {
            if (passesFilter(script))
            {
                drawScript(script);
            }
        }
    }
    else
    {
        // Folder names are paths: "A/B" is a child of "A". The tree is
        // rebuilt only when the folder list changes.
        const std::vector<std::string>& folderNames = m_VirtualFolders.Names();
        if (m_VFTreeNames != folderNames)
        {
            m_VFTreeNames = folderNames;
            m_VFTree.clear();

            auto findOrCreateNode = [](std::vector<VFNode>& nodes, const std::string& name,
                                       const std::string& fullPath) -> VFNode&
            {
                for (auto& n : nodes)
                {
                    if (n.displayName == name)
                    {
                        return n;
                    }
                }
                nodes.push_back({ name, fullPath, {} });
                return nodes.back();
            };

            for (const auto& folderName : folderNames)
            {
                std::vector<VFNode>* currentLevel = &m_VFTree;
                std::string pathSoFar;
                size_t start = 0;
                while (start < folderName.size())
                {
                    size_t slash = folderName.find('/', start);
                    std::string segment = (slash == std::string::npos) ? folderName.substr(start)
                                                                       : folderName.substr(start, slash - start);
                    pathSoFar = pathSoFar.empty() ? segment : pathSoFar + "/" + segment;

                    VFNode& node = findOrCreateNode(*currentLevel, segment, pathSoFar);
                    currentLevel = &node.children;
                    start = (slash == std::string::npos) ? folderName.size() : slash + 1;
                }
            }

            // Sort every level.
            auto sortNodes = [](auto&& self, std::vector<VFNode>& nodes) -> void
            {
                std::sort(nodes.begin(), nodes.end(),
                          [](const VFNode& a, const VFNode& b) { return a.displayName < b.displayName; });
                for (auto& n : nodes)
                {
                    self(self, n.children);
                }
            };
            sortNodes(sortNodes, m_VFTree);
        }
        std::vector<VFNode>& rootNodes = m_VFTree;

        // Deleted after the draw.
        std::string folderToDelete;

        // Inline input for a new folder under parentPath.
        auto drawInlineCreate = [&](const std::string& parentPath)
        {
            if (!m_InlineCreatingFolder || m_InlineCreateParentFolder != parentPath)
            {
                return;
            }

            float arrowIndent = ImGui::GetTreeNodeToLabelSpacing();
            ImGui::Indent(arrowIndent);

            ImGui::TextColored(Palette::FolderIcon, ICON_TI_FOLDER);
            ImGui::SameLine();

            if (!m_InlineFolderFocusSet)
            {
                ImGui::SetKeyboardFocusHere();
                m_InlineFolderFocusSet = true;
            }

            ImGui::SetNextItemWidth(-FLT_MIN);
            bool enterPressed = ImGui::InputText("##vfolder_create", m_NewVirtualFolderName,
                                                 sizeof(m_NewVirtualFolderName), ImGuiInputTextFlags_EnterReturnsTrue);

            if (enterPressed && strlen(m_NewVirtualFolderName) > 0)
            {
                std::string newFullPath = parentPath.empty() ? std::string(m_NewVirtualFolderName)
                                                             : parentPath + "/" + m_NewVirtualFolderName;
                m_VirtualFolders.Create(newFullPath);
                m_InlineCreatingFolder = false;
                m_NewVirtualFolderName[0] = '\0';
            }
            else if (ImGui::IsKeyPressed(ImGuiKey_Escape) ||
                     (!ImGui::IsItemActive() && m_InlineFolderFocusSet && ImGui::IsItemDeactivated()))
            {
                m_InlineCreatingFolder = false;
                m_NewVirtualFolderName[0] = '\0';
            }

            ImGui::Unindent(arrowIndent);
        };

        std::function<void(const VFNode&)> drawVFolderNode = [&](const VFNode& node)
        {
            const std::string& folderName = node.fullPath;

            // A folder being renamed shows an input instead.
            if (m_InlineRenamingFolder && m_RenamingVirtualFolder == folderName)
            {
                float arrowIndent = ImGui::GetTreeNodeToLabelSpacing();
                ImGui::Indent(arrowIndent);

                ImGui::TextColored(Palette::FolderIcon, ICON_TI_FOLDER);
                ImGui::SameLine();

                if (!m_InlineFolderFocusSet)
                {
                    ImGui::SetKeyboardFocusHere();
                    m_InlineFolderFocusSet = true;
                }

                ImGui::SetNextItemWidth(-FLT_MIN);
                bool enterPressed =
                    ImGui::InputText("##vfolder_rename", m_NewVirtualFolderName, sizeof(m_NewVirtualFolderName),
                                     ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);

                if (enterPressed && strlen(m_NewVirtualFolderName) > 0)
                {
                    // Replace the last part of the path.
                    size_t lastSlash = folderName.rfind('/');
                    std::string newFullPath = (lastSlash == std::string::npos)
                                                  ? std::string(m_NewVirtualFolderName)
                                                  : folderName.substr(0, lastSlash + 1) + m_NewVirtualFolderName;
                    m_VirtualFolders.Rename(m_RenamingVirtualFolder, newFullPath);
                    m_InlineRenamingFolder = false;
                    m_RenamingVirtualFolder.clear();
                }
                else if (ImGui::IsKeyPressed(ImGuiKey_Escape) ||
                         (!ImGui::IsItemActive() && m_InlineFolderFocusSet && ImGui::IsItemDeactivated()))
                {
                    m_InlineRenamingFolder = false;
                    m_RenamingVirtualFolder.clear();
                }

                ImGui::Unindent(arrowIndent);
                return;
            }

            // Scripts directly in this folder.
            int scriptCount = 0;
            for (const auto& script : m_ScriptFlatList)
            {
                if (getVirtualFolder(script) == folderName)
                {
                    scriptCount++;
                }
            }

            bool hasChildren = !node.children.empty() || scriptCount > 0;

            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                       ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding;
            if (!hasChildren)
            {
                flags |= ImGuiTreeNodeFlags_Leaf;
            }

            bool isExpanded = m_VirtualFolders.IsExpanded(folderName);
            if (isExpanded)
            {
                ImGui::SetNextItemOpen(true, ImGuiCond_Always);
            }

            ImGui::PushID(folderName.c_str());
            const ImU32 vfLblCol = ImGui::GetColorU32(ImGuiCol_Text);
            // The theme's hover colour, fading in and out (see the file rows).
            ImVec4 vfBg = ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered);
            const ImGuiID vfRowId = ImGui::GetID("##vfolder");
            vfBg.w = DekiEditor::Motion::RowHighlight(vfRowId, false, m_RowHover.WasHovered(vfRowId), 0.0f, vfBg.w);
            if (vfBg.w > 0.0f)
            {
                flags |= ImGuiTreeNodeFlags_Selected;  // draws the (fading) background
            }
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));  // hide ImGui triangle
            ImGui::PushStyleColor(ImGuiCol_Header, vfBg);
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, vfBg);
            bool open = ImGui::TreeNodeEx("##vfolder", flags, "%s", "");
            const ImGuiID nodeId = ImGui::GetItemID();
            m_RowHover.Track(vfRowId);
            ImGui::PopStyleColor(3);
            std::string vfolderLabel = std::string(ICON_TI_FOLDER " ") + node.displayName;
            DrawTreeNodeChevronLabel(open, !(flags & ImGuiTreeNodeFlags_Leaf), vfolderLabel.c_str(), vfLblCol);
            TintTreeNodeFolderIcon();

            if (open != isExpanded)
            {
                m_VirtualFolders.SetExpanded(folderName, open);
            }

            // Dropping a script moves it here, with the rest of the selection
            // when it is part of one.
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCRIPT_VFOLDER"))
                {
                    std::string relPath(static_cast<const char*>(payload->Data));
                    bool draggedIsSelected = false;
                    for (int si : m_SelectedIndices)
                    {
                        if (si < (int)m_Entries.size())
                        {
                            std::string rel = m_Entries[si].relativePath;
                            if (rel.rfind("src/", 0) == 0)
                            {
                                rel = rel.substr(4);
                            }
                            if (rel == relPath)
                            {
                                draggedIsSelected = true;
                                break;
                            }
                        }
                    }
                    if (draggedIsSelected && m_SelectedIndices.size() > 1)
                    {
                        for (int si : m_SelectedIndices)
                        {
                            if (si < (int)m_Entries.size())
                            {
                                std::string rel = m_Entries[si].relativePath;
                                if (rel.rfind("src/", 0) == 0)
                                {
                                    rel = rel.substr(4);
                                }
                                MoveScriptToFolder(rel, folderName);
                            }
                        }
                    }
                    else
                    {
                        MoveScriptToFolder(relPath, folderName);
                    }
                }
                ImGui::EndDragDropTarget();
            }

            // Virtual folder context menu, styled like the menu bar's dropdowns.
            PushContextMenuPopupStyle();
            if (ImGui::BeginPopupContextItem())
            {
                PushContextMenuItemStyle();
                if (ImGui::MenuItem(TrLabel("Create Virtual Folder")))
                {
                    m_NewVirtualFolderName[0] = '\0';
                    m_InlineCreateParentFolder = folderName;
                    m_InlineCreatingFolder = true;
                    m_InlineFolderFocusSet = false;
                    // Expand so the inline input shows.
                    m_VirtualFolders.SetExpanded(folderName, true);
                }
                if (ImGui::MenuItem(TrLabel("Rename Virtual Folder")))
                {
                    m_RenamingVirtualFolder = folderName;
                    // The input holds only the last part of the path.
                    size_t lastSlash = node.displayName.rfind('/');
                    std::string leafName =
                        (lastSlash == std::string::npos) ? node.displayName : node.displayName.substr(lastSlash + 1);
                    strncpy(m_NewVirtualFolderName, leafName.c_str(), sizeof(m_NewVirtualFolderName) - 1);
                    m_NewVirtualFolderName[sizeof(m_NewVirtualFolderName) - 1] = '\0';
                    m_InlineRenamingFolder = true;
                    m_InlineFolderFocusSet = false;
                }
                if (ImGui::MenuItem(TrLabel("Delete Virtual Folder")))
                {
                    folderToDelete = folderName;
                }
                ImGui::PopStyleVar();  // context menu ItemSpacing
                ImGui::EndPopup();
            }
            ImGui::PopStyleVar();  // context menu WindowPadding

            const bool drawChildren = DekiEditor::SchematicTreeBodyBegin(open, nodeId);
            if (drawChildren)
            {
                for (const auto& child : node.children)
                {
                    drawVFolderNode(child);
                }

                drawInlineCreate(folderName);

                for (const auto& script : m_ScriptFlatList)
                {
                    if (getVirtualFolder(script) == folderName)
                    {
                        drawScript(script);
                    }
                }
            }
            DekiEditor::SchematicTreeBodyEnd();
            if (open)
            {
                ImGui::TreePop();
            }
            ImGui::PopID();
        };

        for (const auto& node : rootNodes)
        {
            drawVFolderNode(node);
        }

        // Delete now that the iteration is over.
        if (!folderToDelete.empty())
        {
            m_VirtualFolders.Remove(folderToDelete);
        }

        drawInlineCreate("");

        // Scripts in no virtual folder.
        for (const auto& script : m_ScriptFlatList)
        {
            if (getVirtualFolder(script).empty())
            {
                drawScript(script);
            }
        }
    }

    // Fill the remaining space so all of the child scrolls and takes right-clicks.
    {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        if (avail.y > 0)
        {
            ImGui::Dummy(ImVec2(avail.x, avail.y));
        }
    }

    if (flatIndex == 0 && m_ScriptFlatList.empty())
    {
        DekiEditor::EmptyStateText(Tr("No scripts yet. Right-click to create one."));
    }
    else if (flatIndex == 0 && !filterLower.empty())
    {
        DekiEditor::EmptyStateText(Tr("No scripts match the search."));
    }
}

void AssetBrowserPanel::DrawAssetDetails()
{
    if (!m_SelectedAsset)
    {
        ImGui::TextDisabled("%s", Tr("Select an asset to view details"));
        return;
    }

    const auto& asset = *m_SelectedAsset;

    ImGui::TextUnformatted(Tr("Asset Details"));
    ImGui::Separator();

    ImGui::TextWrapped(Tr("Name: %s"), asset.name.c_str());

    ImGui::TextWrapped(Tr("Path: %s"), asset.relativePath.c_str());

    namespace fs = std::filesystem;
    std::string ext = fs::path(asset.path).extension().string();
    ImGui::Text(Tr("Type: %s"), ext.c_str());

    ImGui::Separator();

    if (!asset.guid.empty())
    {
        ImGui::Text("GUID:");
        ImGui::TextWrapped("%s", asset.guid.c_str());
    }
    else if (IsTextureFile(ext))
    {
        ImGui::TextDisabled("%s", Tr("No GUID (asset not imported)"));
    }

    ImGui::Separator();

    // Texture cache state.
    if (IsTextureFile(ext))
    {
        ImGui::TextUnformatted(Tr("Cache Status:"));

        if (asset.hasCachedVersion)
        {
            ImGui::TextColored(DekiEditor::Palette::Green, "%s", Tr("TEX cached"));

            TexData texData;
            if (TextureImporter::ReadTexFile(asset.cachePath, texData))
            {
                ImGui::Separator();
                ImGui::TextUnformatted(Tr("TEX Info:"));
                int imageW = 0, imageH = 0;
                TextureImporter::ReadSourceSize(asset.cachePath, imageW, imageH);
                ImGui::Text(Tr("  Size: %d x %d"), imageW, imageH);
                if (imageW != static_cast<int>(texData.header.width) ||
                    imageH != static_cast<int>(texData.header.height))
                {
                    ImGui::Text(Tr("  Stored: %u x %u (Max Size)"), texData.header.width, texData.header.height);
                }
                ImGui::Text(Tr("  Format: %s"), texData.GetFormatName().c_str());
                ImGui::Text(Tr("  Data: %.2f KB"), texData.header.dataSize / 1024.0f);

                if (texData.HasAlpha())
                {
                    ImGui::TextUnformatted(Tr("  Alpha: Yes"));
                }
                if (texData.HasTransparency())
                {
                    ImGui::TextUnformatted(Tr("  Transparency: Yes"));
                }
            }
        }
        else if (!asset.guid.empty())
        {
            ImGui::TextColored(DekiEditor::Palette::Amber, "%s", Tr("Not cached"));
            ImGui::TextDisabled("%s", Tr("Run asset build to generate"));
        }
        else
        {
            ImGui::TextDisabled("%s", Tr("Create GUID first"));
        }
    }

    // Font cache state.
    if (ext == ".bdf" || ext == ".ttf" || ext == ".otf")
    {
        ImGui::TextUnformatted(Tr("Cache Status:"));

        if (asset.hasCachedVersion)
        {
            ImGui::TextColored(DekiEditor::Palette::Green, "%s", Tr("Font cached"));

            auto* pipeline = m_App ? m_App->GetAssetPipeline() : nullptr;
            if (pipeline && !asset.guid.empty())
            {
                const auto* subAssets = pipeline->GetSubAssets(asset.guid);
                if (subAssets)
                {
                    ImGui::Text(Tr("  Variants: %zu"), subAssets->size());
                }
            }
        }
        else if (!asset.guid.empty())
        {
            ImGui::TextColored(DekiEditor::Palette::Amber, "%s", Tr("Not cached"));
        }
    }
}

void AssetBrowserPanel::DrawContextMenu()
{
    // Context menu for empty space, styled like the menu bar's dropdowns.
    PushContextMenuPopupStyle();
    if (ImGui::BeginPopupContextWindow("AssetBrowserContextMenu",
                                       ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
    {
        PushContextMenuItemStyle();
        if (m_BrowserMode == BrowserMode::Scripts)
        {
            if (ImGui::MenuItem(TrLabel("Create C++ Script")))
            {
                m_ShowCreateScriptPopup = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem(TrLabel("Create Virtual Folder")))
            {
                m_NewVirtualFolderName[0] = '\0';
                m_InlineCreateParentFolder.clear();
                m_InlineCreatingFolder = true;
                m_InlineFolderFocusSet = false;
            }
        }
        else
        {
            if (ImGui::MenuItem(TrLabel("New Folder")))
            {
                m_CreateFolderDialog->Open();
            }

            // Asset types from the registry that can be created, under one
            // "Create..." submenu. The labels leave out "Create ", as the
            // submenu already says it.
            const auto& creatableTypes = AssetTypeRegistry::Instance().GetTypes();
            if (!creatableTypes.empty() && ImGui::BeginMenu(TrLabel("Create...")))
            {
                for (const auto& type : creatableTypes)
                {
                    if (type.defaultContent.empty())
                    {
                        continue;  // imported, not created (see above)
                    }
                    if (ImGui::MenuItem(type.displayName.c_str()))
                    {
                        m_CreateAssetDialog->Open(type.typeName);
                    }
                }
                ImGui::EndMenu();
            }

            ImGui::Separator();
            if (ImGui::MenuItem(TrLabel("Reveal in File Explorer")))
            {
                RevealInFileExplorer(m_CurrentPath);
            }
        }

        ImGui::PopStyleVar();  // context menu ItemSpacing
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();  // context menu WindowPadding
}

bool AssetBrowserPanel::IsTextureFile(const std::string& extension)
{
    // Packages register the categories, so a new texture format needs only a
    // package change (RegisterCategory or an AssetTypeEditor override), not
    // the asset browser.
    return AssetTypeRegistry::Instance().GetCategoryForExtension(extension) == AssetCategory::Texture;
}

bool AssetBrowserPanel::IsSceneFile(const std::string& extension)
{
    return AssetTypeRegistry::Instance().GetCategoryForExtension(extension) == AssetCategory::Scene;
}

bool AssetBrowserPanel::IsAnimFile(const std::string& extension)
{
    return AssetTypeRegistry::Instance().GetCategoryForExtension(extension) == AssetCategory::Animation;
}

bool AssetBrowserPanel::IsBdfFile(const std::string& extension)
{
    std::string ext = extension;
    for (char& c : ext)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    return ext == ".bdf";
}

void AssetBrowserPanel::HandleFileDrop(const std::string& droppedPath)
{
    if (m_CurrentPath.empty())
    {
        return;
    }

    // Into the hovered folder, or else the current one.
    const std::string targetDir = m_HoveredFolderPath.empty() ? m_CurrentPath : m_HoveredFolderPath;

    std::string error;
    if (!AssetImportDrop::Import(droppedPath, targetDir, m_App ? m_App->GetAssetPipeline() : nullptr, error))
    {
        // Say so: a drop that does nothing silently looks like a broken editor.
        DEKI_LOG_ERROR("Import skipped: %s", error.c_str());
        return;
    }

    NavigateTo(m_CurrentPath);
}

void AssetBrowserPanel::DeferMove(std::string srcPath, std::string destPath)
{
    m_DeferredActions.push_back(
        [this, src = std::move(srcPath), dest = std::move(destPath)]()
        {
            // Skip if the destination is taken (two dropped files with one
            // name: the first wins). Checked when the move runs, not when it is
            // queued, so the order holds.
            if (!std::filesystem::exists(dest) &&
                AssetFileOps::MoveWithData(src, dest, m_App ? m_App->GetAssetPipeline() : nullptr, m_ProjectRoot) &&
                m_App)
            {
                m_App->FollowMovedAsset(src, dest);
            }
            m_PendingTreeRebuild = true;
        });
}

void AssetBrowserPanel::RunDeferredActions()
{
    if (m_DeferredActions.empty())
    {
        return;
    }

    // Move the list out first: an action may queue more (they run next frame),
    // which would reallocate the vector being iterated.
    std::vector<std::function<void()>> actions = std::move(m_DeferredActions);
    m_DeferredActions.clear();
    for (auto& action : actions)
    {
        action();
    }
}

void AssetBrowserPanel::DuplicateAsset(const std::string& srcPath)
{
    namespace fs = std::filesystem;

    fs::path src(srcPath);
    if (!fs::exists(src))
    {
        return;
    }

    fs::path parentDir = src.parent_path();
    std::string stem = src.stem().string();
    std::string ext = src.extension().string();
    bool isDir = fs::is_directory(src);

    // First free name of name_1, name_2, ...
    fs::path destPath;
    for (int i = 1; i < 1000; ++i)
    {
        std::string newName = stem + "_" + std::to_string(i);
        if (!isDir)
        {
            newName += ext;
        }
        destPath = parentDir / newName;
        if (!fs::exists(destPath))
        {
            break;
        }
    }

    std::error_code ec;
    if (isDir)
    {
        fs::copy(src, destPath, fs::copy_options::recursive, ec);
    }
    else
    {
        fs::copy_file(src, destPath, ec);
    }
    if (ec)
    {
        return;
    }

    // Give every copied sidecar a new GUID; import settings are kept. For a
    // directory this walks the whole copy: fs::copy copies the .data files as
    // they are, and with two assets on one GUID the cache cannot tell them
    // apart, so scene references resolve to whichever it saw last.
    if (isDir)
    {
        AssetFileOps::ReassignGuidsInTree(destPath.string());
    }
    else
    {
        AssetFileOps::ReassignGuidForCopy(srcPath, destPath.string());
    }

    // Tell the asset pipeline.
    if (m_App && m_App->GetAssetPipeline())
    {
        std::string newRelPath = fs::relative(destPath, m_ProjectRoot).string();
        for (char& c : newRelPath)
        {
            if (c == '\\')
            {
                c = '/';
            }
        }
        m_App->GetAssetPipeline()->RefreshAsset(newRelPath);
    }

    // Rebuild the tree and select the copy.
    m_PendingRevealPath = destPath.string();
    m_PendingTreeRebuild = true;
}

void AssetBrowserPanel::RenameAssetWithData(const std::string& oldPath, const std::string& newName)
{
    std::string newPath;
    if (AssetFileOps::RenameWithData(oldPath, newName, m_App ? m_App->GetAssetPipeline() : nullptr, m_ProjectRoot,
                                     &newPath) &&
        m_App)
    {
        m_App->FollowMovedAsset(oldPath, newPath);
    }
    m_PendingTreeRebuild = true;
}

bool AssetBrowserPanel::IsSourceFile(const std::string& extension)
{
    std::string ext = extension;
    for (char& c : ext)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    return ext == ".cpp" || ext == ".c" || ext == ".cxx" || ext == ".cc";
}

bool AssetBrowserPanel::IsHeaderFile(const std::string& extension)
{
    std::string ext = extension;
    for (char& c : ext)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    return ext == ".h" || ext == ".hpp" || ext == ".hxx";
}

void AssetBrowserPanel::CreateScript(const std::string& directory, const std::string& name)
{
    std::string error;
    if (!ScriptTemplate::CreateBehaviourScript(directory, name, error))
    {
        DEKI_LOG_ERROR("Create script: %s", error.c_str());
    }
}

void AssetBrowserPanel::SaveObjectAsScene(Deki::Object* obj, const std::string& folderPath)
{
    namespace fs = std::filesystem;

    const std::string targetPath = SceneExport::SaveObjectAsScene(obj, folderPath);
    if (targetPath.empty())
    {
        return;
    }

    // Forward slashes.
    std::string relativePath = fs::relative(targetPath, m_ProjectRoot).string();
    for (char& c : relativePath)
    {
        if (c == '\\')
        {
            c = '/';
        }
    }

    if (auto* pipeline = m_App->GetAssetPipeline())
    {
        pipeline->RefreshAsset(relativePath);

        // The new scene's GUID, for the link prompt.
        const AssetInfo* newAssetInfo = pipeline->GetAssetInfo(relativePath);
        if (newAssetInfo && !newAssetInfo->guid.empty())
        {
            m_PendingLinkObject = obj;
            m_PendingLinkSceneGuid = newAssetInfo->guid;
            m_ShowLinkPrompt = true;
        }
    }

    m_PendingTreeRebuild = true;
}

}  // namespace DekiEditor
