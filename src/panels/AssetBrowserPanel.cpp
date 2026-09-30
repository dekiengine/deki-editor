#include "AssetBrowserPanel.h"
#include "asset_browser/VirtualFolders.h"
#include "asset_browser/AssetFileOps.h"
#include <deki-editor/ProcessLaunch.h>
#include "../services/ScriptTemplate.h"
#include "../services/AssetImportDrop.h"
#include "../core/SceneExport.h"
#include "../widgets/DragScroll.h"
#include "../PanelMaximize.h"
#include "asset_browser/AssetBrowserDialogs.h"
#include "EditorApp.h"
#include <deki-editor/EditorTheme.h>
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
// RAII: hold a bool at true for the scope, restore its prior value on exit
// (including exceptions). Used to mark "asset view is mid-iteration" so a draw
// path that throws - caught upstream by SafeDispatch - can't leave the flag
// stuck true, which would freeze all future tree rebuilds.
struct ScopedTrue
{
    bool& flag;
    bool prev;
    explicit ScopedTrue(bool& f)
        : flag(f), prev(f) { flag = true; }
    ~ScopedTrue() { flag = prev; }
    ScopedTrue(const ScopedTrue&) = delete;
    ScopedTrue& operator=(const ScopedTrue&) = delete;
};

// Repaint the folder glyph baked into a TreeNodeEx label with the palette
// folder tint. Call immediately after the TreeNodeEx so GetItemRect* refer to
// the node just drawn. Draws the same glyph at the same position the tree node
// placed its label, so layout and hit-testing are untouched - it only recolors.
void TintTreeNodeFolderIcon()
{
    ImVec2 mn = ImGui::GetItemRectMin();
    ImVec2 mx = ImGui::GetItemRectMax();
    float iconX = mn.x + ImGui::GetTreeNodeToLabelSpacing();
    float iconY = (mn.y + mx.y) * 0.5f - ImGui::GetFontSize() * 0.5f;
    ImGui::GetWindowDrawList()->AddText(ImVec2(iconX, iconY),
                                        ImGui::GetColorU32(Palette::FolderIcon), ICON_TI_FOLDER);
}

// Context-menu styling helpers (PushContextMenuPopupStyle /
// PushContextMenuItemStyle) now live in EditorTheme so every panel shares one
// look. The callsites below are unchanged - they resolve to the shared versions.

// Open the OS file explorer at the given path. For files, the parent folder is
// opened with the file selected. For folders, the folder itself is opened.
void RevealInFileExplorer(const std::string& absolutePath)
{
    // Delegates to the shared argv-based launcher. The per-platform system()
    // calls this replaced pasted an asset path into a shell command line, so a
    // quote in a filename ran as a command.
    DekiEditor::RevealInFileManager(absolutePath);
}
}  // namespace

AssetBrowserPanel::AssetBrowserPanel(EditorApp* app)
    : m_App(app), m_CreateFolderDialog(std::make_unique<CreateFolderDialog>(this)), m_CreateAssetDialog(std::make_unique<CreateAssetDialog>(app, this)), m_DeleteConfirmDialog(std::make_unique<DeleteConfirmDialog>(this)), m_RenameDialog(std::make_unique<RenameDialog>(this))
{
}

AssetBrowserPanel::~AssetBrowserPanel() = default;

void AssetBrowserPanel::Draw()
{
    if (!m_Open)
        return;

    ImGui::Begin("ASSET BROWSER###Asset Browser", &m_Open, DekiEditor::MaximizedWindowFlags("ASSET BROWSER###Asset Browser"));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 4.0f));
    m_RowHover.BeginFrame();  // rows fade their hover from last frame's

    auto* project = m_App->GetProject();
    if (!project)
    {
        ImGui::TextDisabled("No project loaded");
        ImGui::PopStyleVar(1);
        ImGui::End();
        return;
    }

    // Ensure we have the root paths set
    if (m_AssetsRoot.empty())
    {
        m_ProjectRoot = project->GetPath();
        m_AssetsRoot = project->GetAssetsPath();
        m_SrcRoot = project->GetSrcPath();
        m_RootPath = m_AssetsRoot;  // Default to assets
        m_CurrentPath = m_RootPath;
        BuildFolderTree();
    }

    // Lazy refresh: project open marks the tree dirty rather than walking the
    // asset tree on the critical startup path. The walk happens here, on the
    // first frame the panel actually draws.
    if (m_AssetsDirty)
    {
        m_AssetsDirty = false;
        RefreshAssets();
    }

    // Draw mode tabs (Assets / Scripts) - OUTSIDE scroll region
    DrawModeTabs();

    // Search bar (outside the scroll region), styled like the Hierarchy search.
    if (m_BrowserMode == BrowserMode::Scripts)
        DrawHierarchyStyleSearch("##scriptfilter", m_ScriptFilter, sizeof(m_ScriptFilter));
    else
        DrawHierarchyStyleSearch("##assetfilter", m_AssetFilter, sizeof(m_AssetFilter));

    ImGui::Separator();

    // Scrollable content area as a child window
    ImGui::BeginChild("AssetBrowserContent", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_AlwaysVerticalScrollbar);

    // Auto-scroll while dragging items near the top/bottom edge.
    AutoScrollOnDrag();

    // Taller rows (matching the Hierarchy panel's ~28px) + no vertical gap
    // between entries so selection/hover highlights form a continuous list.
    // FramePadding.x uses the theme value (same left inset as the Hierarchy
    // tree); FramePadding.y is DPI-scaled (the old raw 6px was tiny at high
    // DPI). ItemSpacing.y is zeroed only for the tree, not the tabs.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(ImGui::GetStyle().FramePadding.x, 6.0f * m_App->Dpi(1.0f)));
    const ImVec2 abItemSp = ImGui::GetStyle().ItemSpacing;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(abItemSp.x, 0.0f));
    // Mark the iteration window so any asset mutation triggered from inside the
    // tree/list (drop targets, context menus) defers its rebuild - the Build*
    // mutators check this flag. RAII so a throwing draw (caught by SafeDispatch)
    // can't leave it stuck true.
    {
        ScopedTrue drawing(m_DrawingAssetView);
        if (m_BrowserMode == BrowserMode::Scripts)
            DrawScriptsList();
        else
            DrawAssetTree();
    }
    ImGui::PopStyleVar(2);

    // Window-level drop target for hierarchy objects into current folder
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
                        m_DeferredActions.push_back(
                            [this, obj, folder]()
                            { SaveObjectAsScene(obj, folder); });
                    }
                }
                ImGui::EndDragDropTarget();
            }
        }
    }

    // Drop target on child window background: remove script from virtual folder
    if (m_BrowserMode == BrowserMode::Scripts)
    {
        // Use window-level drop target (covers all empty space in the child)
        if (ImGui::BeginDragDropTargetCustom(ImGui::GetCurrentWindow()->ContentRegionRect, ImGui::GetID("##scripts_child_drop")))
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCRIPT_VFOLDER"))
            {
                std::string relPath(static_cast<const char*>(payload->Data));
                MoveScriptToFolder(relPath, "");
            }
            ImGui::EndDragDropTarget();
        }
    }

    // Context menu must be inside child window so right-click on empty space works
    DrawContextMenu();

    ImGui::EndChild();  // End scrollable content area

    // Run actions queued from within the tree/list draw (moves, duplicates,
    // save-as-scene). Past the iteration now, so their pipeline side effects
    // can safely rebuild the tree.
    RunDeferredActions();

    // Deferred rebuild - must happen after tree iteration is complete
    if (m_PendingTreeRebuild)
    {
        m_PendingTreeRebuild = false;
        BuildFolderTree();
        if (m_BrowserMode == BrowserMode::Scripts)
            BuildScriptFlatList();
    }

    m_CreateFolderDialog->Draw();
    m_CreateAssetDialog->Draw();
    m_DeleteConfirmDialog->Draw();
    m_RenameDialog->Draw();

    // Folder delete confirmation popup
    if (!m_PendingDeleteFolderPath.empty())
    {
        ImGui::OpenPopup("Delete Folder?");
    }
    {
        if (BeginSchematicModal("Delete Folder?", 400.0f))
        {
            namespace fs = std::filesystem;
            std::string folderName = fs::path(m_PendingDeleteFolderPath).filename().string();
            ImGui::TextWrapped("Delete the folder '%s'?", folderName.c_str());
            ImGui::TextColored(Palette::Dim, "Ctrl+Z brings it back.");

            const int button = SchematicDialogButtons({ { "Cancel" }, { "Delete", DialogButton::Danger } });
            if (button == 1)
            {
                std::string folderPath = m_PendingDeleteFolderPath;
                m_PendingDeleteFolderPath.clear();
                // Trash inside the project: rename cannot cross volumes, so a
                // %TEMP% on another drive would fail every move silently.
                auto command = std::make_unique<DeleteAssetsCommand>(
                    std::vector<std::string>{ folderPath },
                    [this]()
                    {
                        BuildFolderTree();
                    },
                    m_ProjectRoot);
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

    // Folder rename popup
    if (!m_PendingRenameFolderPath.empty())
    {
        ImGui::OpenPopup("Rename Folder");
    }
    if (BeginSchematicModal("Rename Folder", 440.0f))
    {
        namespace fs = std::filesystem;
        std::string oldName = fs::path(m_PendingRenameFolderPath).filename().string();
        ImGui::Text("Rename folder '%s'.", oldName.c_str());
        ImGui::Spacing();
        ImGui::TextColored(Palette::Dim, "New name");

        // Focus request must precede the item: the after-item form
        // (SetKeyboardFocusHere(-1)) doesn't take on a modal's appearing
        // frame, leaving the input inactive so typing goes nowhere.
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();

        ImGui::SetNextItemWidth(-FLT_MIN);
        bool enterPressed = ImGui::InputText("##renamefolder", m_RenameFolderBuffer,
                                             sizeof(m_RenameFolderBuffer), ImGuiInputTextFlags_EnterReturnsTrue);

        ImGui::Spacing();
        ImGui::Spacing();

        bool canRename = m_RenameFolderBuffer[0] != '\0' && std::string(m_RenameFolderBuffer) != oldName;

        float availW = ImGui::GetContentRegionAvail().x;
        const float gap = m_App->Dpi(8.0f);
        const float btnW = (availW - gap) * 0.5f;

        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
        if (ImGui::Button("Cancel", ImVec2(btnW, 0)))
        {
            m_PendingRenameFolderPath.clear();
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine(0.0f, gap);

        if (!canRename)
            ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Button, Palette::AccentDim);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Palette::Accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Palette::Accent);

        if (ImGui::Button("Rename", ImVec2(btnW, 0)) || (enterPressed && canRename))
        {
            RenameAssetWithData(m_PendingRenameFolderPath, m_RenameFolderBuffer);
            m_PendingRenameFolderPath.clear();
            m_PendingTreeRebuild = true;
            ImGui::CloseCurrentPopup();
        }

        ImGui::PopStyleColor(3);
        if (!canRename)
            ImGui::EndDisabled();
        ImGui::PopStyleVar();  // ButtonTextAlign

        EndSchematicModal();
    }

    // Link to Scene prompt after drag-drop save
    if (m_ShowLinkPrompt)
    {
        ImGui::OpenPopup("Link to Scene?");
        m_ShowLinkPrompt = false;
    }

    if (BeginSchematicModal("Link to Scene?", 400.0f))
    {
        ImGui::TextWrapped("Link this object to the scene just created from it?");

        const int button = SchematicDialogButtons({ { "Don't Link" }, { "Link", DialogButton::Primary } });
        if (button == 1)
        {
            if (m_PendingLinkObject && !m_PendingLinkSceneGuid.empty())
            {
                auto* scene = m_App->GetCurrentScene();
                if (scene)
                {
                    CommandHistory::Instance().Execute(
                        std::make_unique<LinkInstanceCommand>(
                            scene, m_PendingLinkObject, m_PendingLinkSceneGuid));
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

    // Script creation popup
    if (m_ShowCreateScriptPopup)
    {
        ImGui::OpenPopup("Create Script");
        m_ShowCreateScriptPopup = false;
    }

    if (BeginSchematicModal("Create Script", 440.0f))
    {
        ImGui::Text("Create a new C++ script.");
        ImGui::Spacing();
        ImGui::TextColored(Palette::Dim, "Script name");

        // Focus request must precede the item: the after-item form
        // (SetKeyboardFocusHere(-1)) doesn't take on a modal's appearing
        // frame, leaving the input inactive so typing goes nowhere.
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();

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
        if (ImGui::Button("Cancel", ImVec2(btnW, 0)))
        {
            m_NewScriptName[0] = '\0';
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine(0.0f, gap);

        if (!canCreate)
            ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Button, Palette::AccentDim);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Palette::Accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Palette::Accent);

        if (ImGui::Button("Create", ImVec2(btnW, 0)) || (enterPressed && canCreate))
        {
            // In Scripts mode, always create in src root
            std::string scriptDir = (m_BrowserMode == BrowserMode::Scripts) ? m_SrcRoot : m_CurrentPath;
            CreateScript(scriptDir, m_NewScriptName);
            m_NewScriptName[0] = '\0';
            if (m_BrowserMode == BrowserMode::Scripts)
                BuildScriptFlatList();
            else
                NavigateTo(m_CurrentPath);
            ImGui::CloseCurrentPopup();
        }

        ImGui::PopStyleColor(3);
        if (!canCreate)
            ImGui::EndDisabled();
        ImGui::PopStyleVar();  // ButtonTextAlign

        EndSchematicModal();
    }

    ImGui::PopStyleVar(1);
    ImGui::End();
}

void AssetBrowserPanel::DrawModeTabs()
{
    namespace fs = std::filesystem;

    // Mockup-style sub-nav row: NOT a tab bar - two text links with a 2px
    // accent bottom underline on the active one, dim hover on the inactive,
    // plus a right-aligned mono item count. See React mockup line 444-464.
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

    auto drawLink = [&](const char* label, bool active, float& cursorX) -> bool
    {
        ImVec2 textSize = ImGui::CalcTextSize(label);
        const float linkX0 = cursorX;
        const float linkX1 = linkX0 + textSize.x;
        ImGui::SetCursorScreenPos(ImVec2(linkX0, rowMin.y));
        ImGui::PushID(label);
        bool clicked = ImGui::InvisibleButton(label, ImVec2(textSize.x, rowH));
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const ImU32 col = active ? colFg : (hovered ? colFg : colDim);
        const float textY = rowMin.y + (rowH - textSize.y) * 0.5f;
        dl->AddText(ImVec2(linkX0, textY), col, label);
        if (active)
        {
            dl->AddRectFilled(ImVec2(linkX0, rowMax.y - 2.0f),
                              ImVec2(linkX1, rowMax.y),
                              colAccent);
        }
        cursorX = linkX1;
        return clicked;
    };

    float cx = rowMin.x + padX;
    if (drawLink("Assets", m_BrowserMode == BrowserMode::Assets, cx) &&
        m_BrowserMode != BrowserMode::Assets)
    {
        m_BrowserMode = BrowserMode::Assets;
        m_RootPath = m_AssetsRoot;
        m_CurrentPath = m_RootPath;
        BuildFolderTree();
    }
    cx += gapX;
    if (drawLink("Scripts", m_BrowserMode == BrowserMode::Scripts, cx) &&
        m_BrowserMode != BrowserMode::Scripts)
    {
        m_BrowserMode = BrowserMode::Scripts;
        m_RootPath = m_SrcRoot;
        if (!m_RootPath.empty() && !fs::exists(m_RootPath))
            fs::create_directories(m_RootPath);
        m_CurrentPath = m_RootPath;
        BuildScriptFlatList();
    }

    // Right-aligned item count in mono dim2: the files in the current folder.
    // None at all is left unsaid - beside a tree of folders, "0 items" read as
    // an empty project.
    if (!m_Entries.empty())
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%zu items", m_Entries.size());
        if (DekiEditor::g_FontMonoSmall) ImGui::PushFont(DekiEditor::g_FontMonoSmall, DekiEditor::Fonts::CssPx(DekiEditor::g_FontMonoSmall, 11.5f));
        ImVec2 sz = ImGui::CalcTextSize(buf);
        const float countX = rowMax.x - sz.x - padX;
        const float countY = rowMin.y + (rowH - sz.y) * 0.5f;
        dl->AddText(ImVec2(countX, countY),
                    ImGui::ColorConvertFloat4ToU32(DekiEditor::Palette::Dim2), buf);
        if (DekiEditor::g_FontMonoSmall) ImGui::PopFont();
    }

    // 1px hairline along the bar's bottom edge.
    dl->AddLine(ImVec2(rowMin.x, rowMax.y), ImVec2(rowMax.x, rowMax.y), colLine, 1.0f);

    // Reserve the row in layout flow so the tree renders below it.
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

    // Set root based on current mode
    if (m_BrowserMode == BrowserMode::Scripts)
        m_RootPath = m_SrcRoot;
    else
        m_RootPath = m_AssetsRoot;

    if (m_CurrentPath.empty())
        m_CurrentPath = m_RootPath;

    if (m_BrowserMode == BrowserMode::Scripts)
        BuildScriptFlatList();
    else
        BuildFolderTree();
}

void AssetBrowserPanel::NavigateTo(const std::string& path)
{
    // NavigateTo now just rebuilds the tree (used by dialogs after creating content)
    m_CurrentPath = path;
    BuildFolderTree();
}

void AssetBrowserPanel::RevealAsset(const std::string& absolutePath)
{
    namespace fs = std::filesystem;

    // Normalize the path to match what the tree uses (native separators)
    fs::path normalized = fs::weakly_canonical(fs::path(absolutePath));
    fs::path assetDir = normalized.parent_path();
    fs::path root = fs::weakly_canonical(fs::path(m_RootPath));

    // Expand all parent folders from root to the asset's directory
    for (fs::path p = assetDir; p != root && p.has_parent_path() && p != p.parent_path(); p = p.parent_path())
    {
        m_ExpandedFolders.insert(p.string());
    }
    m_ExpandedFolders.insert(m_RootPath);

    // Queue this path for selection on next draw (store normalized)
    m_PendingRevealPath = normalized.string();
}

void AssetBrowserPanel::BuildFolderTree()
{
    namespace fs = std::filesystem;

    // Reentrancy guard: this frees m_RootNode's vectors. If called while
    // DrawAssetTree is iterating them (an in-tree action's pipeline side effect
    // fired OnAssetCreated -> RefreshAssets -> here), defer to the post-draw
    // rebuild in Draw() instead of reallocating under the live iterator.
    if (m_DrawingAssetView)
    {
        m_PendingTreeRebuild = true;
        return;
    }

    if (m_SelectedAsset && m_SelectedIndex >= 0 && m_SelectedIndex < static_cast<int>(m_Entries.size()))
        m_ReselectPath = m_Entries[m_SelectedIndex].path;

    m_RootNode = FolderNode();
    m_Entries.clear();
    m_SelectedAsset = nullptr;
    m_SelectedIndex = -1;
    m_LastClickedIndex = -1;
    m_SelectedIndices.clear();

    if (m_RootPath.empty() || !fs::exists(m_RootPath))
        return;

    // Initialize root node
    m_RootNode.name = (m_BrowserMode == BrowserMode::Scripts) ? "src" : "assets";
    m_RootNode.path = m_RootPath;

    // Recursively build the tree
    BuildFolderTreeRecursive(m_RootNode, fs::path(m_RootPath));

    // Expand root folder by default
    m_ExpandedFolders.insert(m_RootPath);
}

void AssetBrowserPanel::BuildFolderTreeRecursive(FolderNode& node, const std::filesystem::path& dirPath)
{
    namespace fs = std::filesystem;

    if (!fs::exists(dirPath) || !fs::is_directory(dirPath))
        return;

    std::vector<FolderNode> subfolders;
    std::vector<AssetEntry> files;

    for (const auto& entry : fs::directory_iterator(dirPath))
    {
        std::string name = entry.path().filename().string();

        // Skip hidden files
        if (name[0] == '.')
            continue;

        if (entry.is_directory())
        {
            // In Scripts mode, hide generated folder
            if (m_BrowserMode == BrowserMode::Scripts && name == "generated")
                continue;

            FolderNode subfolder;
            subfolder.name = name;
            subfolder.path = entry.path().string();
            BuildFolderTreeRecursive(subfolder, entry.path());
            subfolders.push_back(std::move(subfolder));
        }
        else
        {
            std::string ext = entry.path().extension().string();

            // Skip metadata files
            if (ext == ".dekiguid" || ext == ".data")
                continue;

            // In Scripts mode, hide PluginExports.cpp
            if (m_BrowserMode == BrowserMode::Scripts && name == "PluginExports.cpp")
                continue;

            AssetEntry asset;
            asset.name = name;
            asset.path = entry.path().string();
            asset.relativePath = fs::relative(entry.path(), m_ProjectRoot).string();
            // Normalize slashes
            for (char& c : asset.relativePath)
            {
                if (c == '\\') c = '/';
            }
            asset.isDirectory = false;

            // Populate GUID and cache info for texture files
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

                // Check for spritesheet frames
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
                // Font files have their GUID in a .data sidecar file
                asset.guid = ReadAssetGuid(asset.path);

                // Check for baked font variant sub-assets
                auto* pipeline = m_App->GetAssetPipeline();
                if (pipeline && !asset.guid.empty())
                {
                    const auto* subAssets = pipeline->GetSubAssets(asset.guid);
                    if (subAssets && !subAssets->empty())
                    {
                        asset.isExpandable = true;
                        asset.isExpanded = m_ExpandedAssets.count(asset.guid) > 0;
                        asset.hasCachedVersion = true;  // Variants baked = cached
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
                        asset.hasCachedVersion = true;  // Variants baked = cached
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

    // Sort alphabetically
    std::sort(subfolders.begin(), subfolders.end(),
              [](const FolderNode& a, const FolderNode& b)
              { return a.name < b.name; });
    std::sort(files.begin(), files.end(),
              [](const AssetEntry& a, const AssetEntry& b)
              { return a.name < b.name; });

    node.subfolders = std::move(subfolders);
    node.files = std::move(files);
}

void AssetBrowserPanel::DrawHierarchyStyleSearch(const char* id, char* buf, size_t bufSize)
{
    // Shared with the Package Manager and any other panel that wants the same
    // borderless magnifier search - see DekiEditor::SearchField (EditorTheme).
    DekiEditor::SearchField(id, buf, bufSize);
}

// True if `name` (case-insensitive) contains the already-lowercased needle.
static bool AssetNameMatches(const std::string& name, const std::string& needleLower)
{
    if (needleLower.empty()) return true;
    std::string n = name;
    std::transform(n.begin(), n.end(), n.begin(),
                   [](unsigned char c)
                   { return (char)std::tolower(c); });
    return n.find(needleLower) != std::string::npos;
}

// True if the folder has any matching file in its subtree (recursive).
static bool FolderHasMatchingFile(const FolderNode& node, const std::string& needleLower)
{
    for (const auto& f : node.files)
        if (AssetNameMatches(f.name, needleLower)) return true;
    for (const auto& sub : node.subfolders)
        if (FolderHasMatchingFile(sub, needleLower)) return true;
    return false;
}

void AssetBrowserPanel::DrawAssetTree()
{
    // Lowercase the active filter once per frame; node drawing reads it.
    m_AssetFilterLower.clear();
    if (m_AssetFilter[0])
    {
        m_AssetFilterLower = m_AssetFilter;
        std::transform(m_AssetFilterLower.begin(), m_AssetFilterLower.end(),
                       m_AssetFilterLower.begin(),
                       [](unsigned char c)
                       { return (char)std::tolower(c); });
    }

    m_HoveredFolderPath.clear();
    m_Entries.clear();  // Rebuild flat entries for selection tracking
    int flatIndex = 0;
    DrawFolderNode(m_RootNode, flatIndex);

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

    // Cache multi-drag payload now that m_Entries is fully populated.
    // Can't build this inside DrawFileNode because m_Entries is only partial mid-traversal.
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

    // Keyboard navigation (macOS-style)
    if (ImGui::IsWindowFocused() && !m_InlineRenamingFolder && !m_Entries.empty())
    {
        namespace fs = std::filesystem;

        // Enter to rename selected asset
        if (ImGui::IsKeyPressed(ImGuiKey_Enter) && m_SelectedIndices.size() == 1)
        {
            int idx = *m_SelectedIndices.begin();
            if (idx >= 0 && idx < (int)m_Entries.size())
            {
                std::string currentName = fs::path(m_Entries[idx].name).stem().string();
                m_RenameDialog->Open(idx, currentName, m_Entries[idx].isDirectory);
            }
        }

        // Up/Down arrow keys to navigate
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
        else if (m_SelectedIndex < 0 && (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow)))
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

    // Asset search: hide non-root folders that have no matching descendant.
    const bool filtering = !m_AssetFilterLower.empty();
    if (filtering && node.path != m_RootPath &&
        !FolderHasMatchingFile(node, m_AssetFilterLower))
        return;

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_OpenOnDoubleClick |
                               ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_FramePadding;

    bool hasChildren = !node.subfolders.empty() || !node.files.empty();
    if (!hasChildren)
        flags |= ImGuiTreeNodeFlags_Leaf;

    // Check if this folder is expanded. While filtering, force matching
    // folders open so results show without manual expansion.
    bool isExpanded = m_ExpandedFolders.count(node.path) > 0;
    if (isExpanded || filtering)
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);

    ImGui::PushID(node.path.c_str());

    const ImU32 folderLblCol = ImGui::GetColorU32(ImGuiCol_Text);
    // Cyan hover background (global Header is transparent), fading in and out.
    const ImVec4 acc = DekiEditor::Palette::Accent;
    const ImGuiID folderRowId = ImGui::GetID("##folder");
    const float folderAlpha = DekiEditor::Motion::RowHighlight(folderRowId, false, m_RowHover.WasHovered(folderRowId), 0.22f, 0.12f);
    if (folderAlpha > 0.0f)
        flags |= ImGuiTreeNodeFlags_Selected;  // draws the (fading) background
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

    // Track expansion state (but not while filtering force-opened the folder,
    // so the user's manual expansion state is preserved when the filter clears).
    if (!filtering && open != isExpanded)
    {
        if (open)
            m_ExpandedFolders.insert(node.path);
        else
            m_ExpandedFolders.erase(node.path);
    }

    // Track hovered folder for external drops
    if (ImGui::IsItemHovered())
    {
        m_HoveredFolderPath = node.path;
    }

    // Drag source for folders (root folder cannot be moved)
    if (node.path != m_RootPath && ImGui::BeginDragDropSource())
    {
        ImGui::SetDragDropPayload("FOLDER_PATH", node.path.c_str(), node.path.size() + 1);
        ImGui::TextColored(Palette::FolderIcon, ICON_TI_FOLDER);
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::Text(" %s", node.name.c_str());
        ImGui::EndDragDropSource();
    }

    // Drop target for folders
    if (ImGui::BeginDragDropTarget())
    {
        // All mutations below are queued, not performed now: this drop target
        // fires while DrawFolderNode is iterating the tree, so moving/saving here
        // would rebuild it under the live iterator. RunDeferredActions runs them
        // after EndChild. The payload is only valid this frame, so its bytes are
        // copied into owned strings that the queued lambdas capture.
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_MULTI"))
        {
            // Multi-asset drop: parse all paths and queue each move.
            std::string multiData(static_cast<const char*>(payload->Data));
            std::istringstream stream(multiData);
            std::string line;
            while (std::getline(stream, line))
            {
                if (line.empty()) continue;
                size_t tabPos = line.find('\t');
                if (tabPos == std::string::npos) continue;
                std::string relPath = line.substr(tabPos + 1);
                std::string srcPath = (fs::path(m_ProjectRoot) / relPath).string();
                fs::path dest = fs::path(node.path) / fs::path(srcPath).filename();
                if (fs::path(srcPath) != dest.parent_path())
                    DeferMove(srcPath, dest.string());
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
                DeferMove(droppedPath, dest.string());
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
            bool isDescendant = nodeNorm.size() > srcNorm.size() &&
                                nodeNorm.compare(0, srcNorm.size(), srcNorm) == 0 &&
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
                m_DeferredActions.push_back(
                    [this, obj, folder]()
                    { SaveObjectAsScene(obj, folder); });
            }
        }
        ImGui::EndDragDropTarget();
    }

    // Context menu for folder. Styled to match the top menu bar's dropdowns.
    PushContextMenuPopupStyle();
    if (ImGui::BeginPopupContextItem())
    {
        PushContextMenuItemStyle();
        if (ImGui::MenuItem("New Folder"))
        {
            m_CurrentPath = node.path;
            m_CreateFolderDialog->Open();
        }

        if (m_BrowserMode == BrowserMode::Scripts)
        {
            if (ImGui::MenuItem("Create C++ Script"))
            {
                m_CurrentPath = node.path;
                m_ShowCreateScriptPopup = true;
            }
        }
        else
        {
            // Creatable asset types grouped under a "Create..." submenu.
            const auto& creatableTypes = AssetTypeRegistry::Instance().GetTypes();
            if (!creatableTypes.empty() && ImGui::BeginMenu("Create..."))
            {
                for (const auto& type : creatableTypes)
                {
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
        if (ImGui::MenuItem("Reveal in File Explorer"))
        {
            RevealInFileExplorer(node.path);
        }

        // Rename / Delete folder (not allowed on root)
        if (node.path != m_RootPath)
        {
            ImGui::Separator();
            if (ImGui::MenuItem("Rename", "Enter"))
            {
                m_PendingRenameFolderPath = node.path;
                std::string folderName = fs::path(node.path).filename().string();
                strncpy(m_RenameFolderBuffer, folderName.c_str(), sizeof(m_RenameFolderBuffer) - 1);
                m_RenameFolderBuffer[sizeof(m_RenameFolderBuffer) - 1] = '\0';
            }
            if (ImGui::MenuItem("Delete", "Del"))
            {
                m_PendingDeleteFolderPath = node.path;
            }
        }

        ImGui::PopStyleVar();  // context menu ItemSpacing
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();  // context menu WindowPadding

    // Subfolders and files as an animated body. The id scope is popped AFTER
    // the subtree now (it used to be popped here, which pushed the children
    // into a different id scope than the one the tree node opened) so that a
    // folder's children keep one identity whether it is open, closing or open
    // again.
    const bool drawChildren = DekiEditor::SchematicTreeBodyBegin(open, nodeId);
    if (drawChildren)
    {
        // Draw subfolders first
        for (auto& subfolder : node.subfolders)
        {
            DrawFolderNode(subfolder, flatIndex);
        }

        // Draw files
        for (auto& file : node.files)
        {
            // Asset search: only show files whose name matches the filter.
            if (filtering && !AssetNameMatches(file.name, m_AssetFilterLower))
                continue;

            // Add to flat entries for selection tracking
            m_Entries.push_back(file);
            DrawFileNode(file, flatIndex);
            flatIndex++;

            // Handle expanded spritesheet frames (check live state, not cached)
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
                            frameEntry.name = subAsset.name.empty()
                                                  ? "Frame " + std::to_string(subAsset.subAssetIndex)
                                                  : subAsset.name;
                            frameEntry.path = file.path;
                            frameEntry.relativePath = file.relativePath;
                            frameEntry.guid = subAsset.guid;
                            frameEntry.isDirectory = false;
                            frameEntry.isSubAsset = true;
                            frameEntry.parentGuid = subAsset.parentGuid;
                            frameEntry.subAssetIndex = subAsset.subAssetIndex;

                            // Use sub-asset metadata for display
                            frameEntry.subAssetDepth = subAsset.depth;

                            if (!subAsset.cachePath.empty())
                            {
                                // Sub-asset has its own cache file
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
    if (open) ImGui::TreePop();
    ImGui::PopID();
}

void AssetBrowserPanel::DrawFileNode(AssetEntry& entry, int flatIndex)
{
    namespace fs = std::filesystem;

    // All files indent by arrowIndent so icons align with folder text.
    // Expandable files draw their arrow in the space to the left.
    float arrowIndent = ImGui::GetTreeNodeToLabelSpacing();
    ImGui::Indent(arrowIndent);

    ImGui::PushID(entry.path.c_str());
    if (entry.isSubAsset)
        ImGui::PushID(entry.subAssetIndex);

    // Icon prefix based on file type
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
        // Un-indent so the selectable covers the arrow area too
        float arrowSpace = ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.x;
        ImGui::Unindent(arrowSpace);
        arrowOffset = arrowSpace;
    }
    else if (entry.isSubAsset)
    {
        ImGui::Indent(ImGui::GetStyle().IndentSpacing * (1 + entry.subAssetDepth));
    }

    // Handle pending reveal selection. The filename is compared first so the
    // filesystem call runs for one candidate row, not for every row drawn
    // while a reveal is pending.
    if (!m_PendingRevealPath.empty() &&
        fs::path(entry.path).filename() == fs::path(m_PendingRevealPath).filename() &&
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
    // Cyan selection/hover background (global Header is transparent), fading
    // in and out with selection and hover.
    const ImVec4 acc = DekiEditor::Palette::Accent;
    const ImGuiID fileRowId = ImGui::GetID("##file");
    const float fileAlpha = DekiEditor::Motion::RowHighlight(fileRowId, isSelected, m_RowHover.WasHovered(fileRowId), 0.22f, 0.12f);
    const ImVec4 fileBg(acc.x, acc.y, acc.z, fileAlpha);
    ImGui::PushStyleColor(ImGuiCol_Header, fileBg);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, fileBg);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(acc.x, acc.y, acc.z, 0.18f));
    const bool fileClicked = ImGui::Selectable("##file", fileAlpha > 0.0f, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, rowHeight));
    m_RowHover.Track(fileRowId);
    ImGui::PopStyleColor(3);
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
                m_SelectedIndices.erase(flatIndex);
            else
                m_SelectedIndices.insert(flatIndex);
            m_LastClickedIndex = flatIndex;
        }
        else if (isSelected && m_SelectedIndices.size() > 1)
        {
            // Clicked on an already-selected item in a multi-selection:
            // Defer deselect to allow drag-drop to work.
            // The selection collapses to single on release (handled below).
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

        // Double-click to open files (not on the arrow area)
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
                // Try opening with any registered EditorWindow that handles this extension
                m_App->GetToolHost()->OpenToolForFile(entry.path.c_str());
            }
        }
    }

    // Draw icon and text centered vertically, plus expand arrow if needed
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
        float centerY = selectablePos.y + rowHeight * 0.5f;

        // Expand arrow for spritesheets - centered in the row
        if (drawExpandArrow)
        {
            bool expanded = m_ExpandedAssets.count(entry.guid) > 0;
            float arrowSize = ImGui::GetFontSize();
            float arrowX = selectablePos.x;
            // Tabler chevron (matches folders, hierarchy and inspector headers).
            const char* chev = expanded ? ICON_TI_CHEVRON_DOWN : ICON_TI_CHEVRON_RIGHT;
            ImFont* chevFont = g_FontUI ? g_FontUI : ImGui::GetFont();
            const ImVec2 chevSz = chevFont->CalcTextSizeA(arrowSize, FLT_MAX, 0.0f, chev);
            dl->AddText(chevFont, arrowSize,
                        ImVec2(arrowX + (arrowSize - chevSz.x) * 0.5f, centerY - chevSz.y * 0.5f),
                        IM_COL32(165, 166, 164, 255), chev);

            // Click on arrow region toggles expand
            ImVec2 mousePos = ImGui::GetIO().MousePos;
            if (ImGui::IsMouseClicked(0) &&
                mousePos.x >= arrowX && mousePos.x < arrowX + arrowSize &&
                mousePos.y >= selectablePos.y && mousePos.y < selectablePos.y + rowHeight)
            {
                if (expanded)
                    m_ExpandedAssets.erase(entry.guid);
                else
                    m_ExpandedAssets.insert(entry.guid);
            }
        }

        // Icon - center vertically, offset past arrow if expandable
        float contentX = selectablePos.x + arrowOffset;
        ImVec2 iconSize = ImGui::CalcTextSize(icon.c_str());
        float iconY = centerY - iconSize.y * 0.5f;
        dl->AddText(ImVec2(contentX, iconY), textCol, icon.c_str());

        // Text - center vertically in the row
        float textX = contentX + iconSize.x;
        float textY = centerY - ImGui::GetFontSize() * 0.5f;
        dl->AddText(ImVec2(textX, textY), textCol, entry.name.c_str());
    }

    if (entry.isExpandable)
    {
        ImGui::Indent(arrowOffset);  // Restore indent we removed for the arrow
    }
    else if (entry.isSubAsset)
    {
        ImGui::Unindent(ImGui::GetStyle().IndentSpacing * (1 + entry.subAssetDepth));
    }

    // Context menu for files. Styled to match the top menu bar's dropdowns.
    PushContextMenuPopupStyle();
    if (!entry.isSubAsset && ImGui::BeginPopupContextItem())
    {
        PushContextMenuItemStyle();
        // If right-clicked item is not in selection, select only this item
        if (m_SelectedIndices.count(flatIndex) == 0)
        {
            m_SelectedIndices.clear();
            m_SelectedIndices.insert(flatIndex);
            m_SelectedIndex = flatIndex;
            m_LastClickedIndex = flatIndex;
            m_SelectedAsset = &m_Entries[flatIndex];
            m_App->SetSelectedAsset(entry.path, entry.cachePath, entry.hasCachedVersion, entry.guid);
        }

        // Spritesheet editor option
        if (IsTextureFile(ext) && entry.hasCachedVersion)
        {
            if (ImGui::MenuItem("Edit Spritesheet..."))
            {
                m_App->GetToolHost()->OpenToolForFile(entry.path.c_str(), entry.cachePath.c_str());
            }
            ImGui::Separator();
        }

        // Rename
        if (m_SelectedIndices.size() == 1)
        {
            if (ImGui::MenuItem("Rename", "Enter"))
            {
                std::string currentName = fs::path(entry.name).stem().string();
                m_RenameDialog->Open(flatIndex, currentName, false);
            }
        }

        // Duplicate (single file/folder only)
        if (m_SelectedIndices.size() == 1)
        {
            if (ImGui::MenuItem("Duplicate", "Ctrl+D"))
            {
                m_DeferredActions.push_back(
                    [this, p = entry.path]()
                    { DuplicateAsset(p); });
            }
        }

        if (m_SelectedIndices.size() == 1)
        {
            if (ImGui::MenuItem("Reveal in File Explorer"))
            {
                RevealInFileExplorer(entry.path);
            }
        }

        ImGui::Separator();

        // Delete
        if (m_SelectedIndices.size() > 1)
        {
            char menuLabel[64];
            snprintf(menuLabel, sizeof(menuLabel), "Delete %zu items", m_SelectedIndices.size());
            if (ImGui::MenuItem(menuLabel, "Del"))
            {
                m_DeleteConfirmDialog->Open();
            }
        }
        else
        {
            if (ImGui::MenuItem("Delete", "Del"))
            {
                m_DeleteConfirmDialog->Open();
            }
        }

        ImGui::PopStyleVar();  // context menu ItemSpacing
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();  // context menu WindowPadding

    // Handle deferred single-select: if mouse released without dragging, collapse to single
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

    // Drag source
    if (!entry.isSubAsset && ImGui::BeginDragDropSource())
    {
        m_DeferredSingleSelect = -1;  // Cancel deferred select - dragging started

        if (m_SelectedIndices.size() > 1 && m_SelectedIndices.count(flatIndex) > 0 && !m_CachedMultiDragPayload.empty())
        {
            // Multi-selection drag: use cached payload built after full tree draw
            ImGui::SetDragDropPayload("ASSET_MULTI", m_CachedMultiDragPayload.c_str(),
                                      m_CachedMultiDragPayload.size() + 1);
            ImGui::Text("Drag %zu items", m_SelectedIndices.size());
        }
        else
        {
            // Single item drag
            if (!entry.guid.empty())
            {
                ImGui::SetDragDropPayload("ASSET_GUID", entry.guid.c_str(), entry.guid.size() + 1);
            }
            ImGui::SetDragDropPayload("ASSET_PATH", entry.relativePath.c_str(), entry.relativePath.size() + 1);
            ImGui::Text("Drag %s", entry.name.c_str());
        }
        ImGui::EndDragDropSource();
    }
    else if (entry.isSubAsset && !entry.guid.empty() && ImGui::BeginDragDropSource())
    {
        ImGui::SetDragDropPayload("ASSET_GUID", entry.guid.c_str(), entry.guid.size() + 1);
        ImGui::Text("Drag %s", entry.name.c_str());
        ImGui::EndDragDropSource();
    }

    if (entry.isSubAsset)
        ImGui::PopID();
    ImGui::PopID();
    ImGui::Unindent(arrowIndent);
}

void AssetBrowserPanel::BuildScriptFlatList()
{
    namespace fs = std::filesystem;

    // Reentrancy guard: this frees the vectors DrawScriptsList iterates. Defer
    // if called mid-draw (see BuildFolderTree for the full rationale).
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
        return;

    // Load virtual folder assignments if not yet loaded
    if (m_VirtualFolders.Names().empty() && m_VirtualFolders.Assignments().empty() && !m_VirtualFolders.Dirty())
        m_VirtualFolders.Load(m_ProjectRoot);

    // Recursively collect all source/header files
    for (const auto& entry : fs::recursive_directory_iterator(m_SrcRoot))
    {
        if (!entry.is_regular_file())
            continue;

        std::string name = entry.path().filename().string();

        // Skip hidden files
        if (name[0] == '.')
            continue;

        // Skip generated folder
        std::string relToSrc = fs::relative(entry.path(), m_SrcRoot).string();
        for (char& c : relToSrc)
        {
            if (c == '\\') c = '/';
        }
        if (relToSrc.rfind("generated/", 0) == 0 || relToSrc.rfind("generated\\", 0) == 0)
            continue;

        // Skip PluginExports.cpp
        if (name == "PluginExports.cpp")
            continue;

        std::string ext = entry.path().extension().string();
        if (!IsSourceFile(ext) && !IsHeaderFile(ext))
            continue;

        ScriptEntry script;
        script.name = name;
        script.path = entry.path().string();
        script.relativePath = relToSrc;
        m_ScriptFlatList.push_back(std::move(script));
    }

    // Sort by name
    std::sort(m_ScriptFlatList.begin(), m_ScriptFlatList.end(),
              [](const ScriptEntry& a, const ScriptEntry& b)
              { return a.name < b.name; });

    // Clean up assignments for files that no longer exist
    m_VirtualFolders.PruneMissing([this](const std::string& rel)
                                  {
        for (const auto& s : m_ScriptFlatList)
            if (s.relativePath == rel) return true;
        return false; });
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

    // Convert filter to lowercase for case-insensitive matching
    std::string filterLower;
    if (m_ScriptFilter[0] != '\0')
    {
        filterLower = m_ScriptFilter;
        for (char& c : filterLower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    // Helper lambda to check if a script passes the filter
    auto passesFilter = [&](const ScriptEntry& script) -> bool
    {
        if (filterLower.empty())
            return true;
        std::string nameLower = script.name;
        for (char& c : nameLower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string relLower = script.relativePath;
        for (char& c : relLower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return nameLower.find(filterLower) != std::string::npos ||
               relLower.find(filterLower) != std::string::npos;
    };

    // Helper to get the virtual folder for a script
    auto getVirtualFolder = [&](const ScriptEntry& script) -> std::string
    {
        auto it = m_VirtualFolders.Assignments().find(script.relativePath);
        if (it != m_VirtualFolders.Assignments().end())
            return it->second;
        return "";
    };

    // Helper lambda to draw a single script entry
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

        // Drag source for moving to virtual folders
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
        {
            ImGui::SetDragDropPayload("SCRIPT_VFOLDER", script.relativePath.c_str(), script.relativePath.size() + 1);
            ImGui::Text("Move %s", script.name.c_str());
            ImGui::EndDragDropSource();
        }

        // Context menu: move to virtual folder. Styled like the top menu bar.
        PushContextMenuPopupStyle();
        if (ImGui::BeginPopupContextItem(("##scriptctx_" + script.relativePath).c_str()))
        {
            PushContextMenuItemStyle();
            // If right-clicked item is not in selection, select only this item
            if (m_SelectedIndices.count(flatIndex) == 0)
            {
                m_SelectedIndices.clear();
                m_SelectedIndices.insert(flatIndex);
                m_SelectedIndex = flatIndex;
                m_LastClickedIndex = flatIndex;
                m_SelectedAsset = &m_Entries[flatIndex];
            }

            if (!m_VirtualFolders.Names().empty() && ImGui::BeginMenu("Move to Virtual Folder"))
            {
                if (ImGui::MenuItem("(root)"))
                {
                    for (int si : m_SelectedIndices)
                    {
                        if (si < (int)m_Entries.size())
                        {
                            std::string rel = m_Entries[si].relativePath;
                            if (rel.rfind("src/", 0) == 0) rel = rel.substr(4);
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
                                if (rel.rfind("src/", 0) == 0) rel = rel.substr(4);
                                MoveScriptToFolder(rel, vf);
                            }
                        }
                    }
                }
                ImGui::EndMenu();
            }

            // Rename (single selection)
            if (m_SelectedIndices.size() == 1)
            {
                std::string ext = fs::path(script.path).extension().string();
                if (ImGui::MenuItem("Rename", "Enter"))
                {
                    std::string currentName = fs::path(script.name).stem().string();
                    m_RenameDialog->Open(flatIndex, currentName, false);
                }
            }

            if (m_SelectedIndices.size() == 1)
            {
                if (ImGui::MenuItem("Reveal in File Explorer"))
                {
                    RevealInFileExplorer(script.path);
                }
            }

            ImGui::Separator();

            // Delete
            if (ImGui::MenuItem("Delete", "Del"))
            {
                m_DeleteConfirmDialog->Open();
            }

            ImGui::PopStyleVar();  // context menu ItemSpacing
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();  // context menu WindowPadding

        flatIndex++;
    };

    // When filtering, show flat list (skip folder grouping)
    if (!filterLower.empty())
    {
        for (const auto& script : m_ScriptFlatList)
        {
            if (passesFilter(script))
                drawScript(script);
        }
    }
    else
    {
        // Virtual folder tree from path-based names: only top-level folders
        // are direct children of root; "A/B" is a child of "A". Built when the
        // folder list changes, not every frame.
        const std::vector<std::string>& folderNames = m_VirtualFolders.Names();
        if (m_VFTreeNames != folderNames)
        {
            m_VFTreeNames = folderNames;
            m_VFTree.clear();

            auto findOrCreateNode = [](std::vector<VFNode>& nodes, const std::string& name, const std::string& fullPath) -> VFNode&
            {
                for (auto& n : nodes)
                {
                    if (n.displayName == name) return n;
                }
                nodes.push_back({ name, fullPath, {} });
                return nodes.back();
            };

            for (const auto& folderName : folderNames)
            {
                // Split path by '/'
                std::vector<VFNode>* currentLevel = &m_VFTree;
                std::string pathSoFar;
                size_t start = 0;
                while (start < folderName.size())
                {
                    size_t slash = folderName.find('/', start);
                    std::string segment = (slash == std::string::npos)
                                              ? folderName.substr(start)
                                              : folderName.substr(start, slash - start);
                    pathSoFar = pathSoFar.empty() ? segment : pathSoFar + "/" + segment;

                    VFNode& node = findOrCreateNode(*currentLevel, segment, pathSoFar);
                    currentLevel = &node.children;
                    start = (slash == std::string::npos) ? folderName.size() : slash + 1;
                }
            }

            // Sort nodes at each level recursively
            auto sortNodes = [](auto&& self, std::vector<VFNode>& nodes) -> void
            {
                std::sort(nodes.begin(), nodes.end(), [](const VFNode& a, const VFNode& b)
                          { return a.displayName < b.displayName; });
                for (auto& n : nodes)
                    self(self, n.children);
            };
            sortNodes(sortNodes, m_VFTree);
        }
        std::vector<VFNode>& rootNodes = m_VFTree;

        // Deferred delete
        std::string folderToDelete;

        // Helper to draw inline create input
        auto drawInlineCreate = [&](const std::string& parentPath)
        {
            if (!m_InlineCreatingFolder || m_InlineCreateParentFolder != parentPath)
                return;

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
            bool enterPressed = ImGui::InputText("##vfolder_create", m_NewVirtualFolderName, sizeof(m_NewVirtualFolderName),
                                                 ImGuiInputTextFlags_EnterReturnsTrue);

            if (enterPressed && strlen(m_NewVirtualFolderName) > 0)
            {
                std::string newFullPath = parentPath.empty()
                                              ? std::string(m_NewVirtualFolderName)
                                              : parentPath + "/" + m_NewVirtualFolderName;
                m_VirtualFolders.Create(newFullPath);
                m_InlineCreatingFolder = false;
                m_NewVirtualFolderName[0] = '\0';
            }
            else if (ImGui::IsKeyPressed(ImGuiKey_Escape) || (!ImGui::IsItemActive() && m_InlineFolderFocusSet && ImGui::IsItemDeactivated()))
            {
                m_InlineCreatingFolder = false;
                m_NewVirtualFolderName[0] = '\0';
            }

            ImGui::Unindent(arrowIndent);
        };

        // Recursive draw function
        std::function<void(const VFNode&)> drawVFolderNode = [&](const VFNode& node)
        {
            const std::string& folderName = node.fullPath;

            // If this folder is being renamed inline, show input instead
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
                bool enterPressed = ImGui::InputText("##vfolder_rename", m_NewVirtualFolderName, sizeof(m_NewVirtualFolderName),
                                                     ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);

                if (enterPressed && strlen(m_NewVirtualFolderName) > 0)
                {
                    // Reconstruct full path: replace the leaf name
                    size_t lastSlash = folderName.rfind('/');
                    std::string newFullPath = (lastSlash == std::string::npos)
                                                  ? std::string(m_NewVirtualFolderName)
                                                  : folderName.substr(0, lastSlash + 1) + m_NewVirtualFolderName;
                    m_VirtualFolders.Rename(m_RenamingVirtualFolder, newFullPath);
                    m_InlineRenamingFolder = false;
                    m_RenamingVirtualFolder.clear();
                }
                else if (ImGui::IsKeyPressed(ImGuiKey_Escape) || (!ImGui::IsItemActive() && m_InlineFolderFocusSet && ImGui::IsItemDeactivated()))
                {
                    m_InlineRenamingFolder = false;
                    m_RenamingVirtualFolder.clear();
                }

                ImGui::Unindent(arrowIndent);
                return;
            }

            // Count scripts directly in this folder
            int scriptCount = 0;
            for (const auto& script : m_ScriptFlatList)
            {
                if (getVirtualFolder(script) == folderName) scriptCount++;
            }

            bool hasChildren = !node.children.empty() || scriptCount > 0;

            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                       ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                       ImGuiTreeNodeFlags_SpanAvailWidth |
                                       ImGuiTreeNodeFlags_FramePadding;
            if (!hasChildren)
                flags |= ImGuiTreeNodeFlags_Leaf;

            bool isExpanded = m_VirtualFolders.IsExpanded(folderName);
            if (isExpanded)
                ImGui::SetNextItemOpen(true, ImGuiCond_Always);

            ImGui::PushID(folderName.c_str());
            const ImU32 vfLblCol = ImGui::GetColorU32(ImGuiCol_Text);
            // The theme's hover colour, fading in and out (see the file rows).
            ImVec4 vfBg = ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered);
            const ImGuiID vfRowId = ImGui::GetID("##vfolder");
            vfBg.w = DekiEditor::Motion::RowHighlight(vfRowId, false, m_RowHover.WasHovered(vfRowId), 0.0f, vfBg.w);
            if (vfBg.w > 0.0f)
                flags |= ImGuiTreeNodeFlags_Selected;  // draws the (fading) background
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

            // Drop target: move scripts into this virtual folder
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
                            if (rel.rfind("src/", 0) == 0) rel = rel.substr(4);
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
                                if (rel.rfind("src/", 0) == 0) rel = rel.substr(4);
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

            // Context menu for virtual folder. Styled like the top menu bar.
            PushContextMenuPopupStyle();
            if (ImGui::BeginPopupContextItem())
            {
                PushContextMenuItemStyle();
                if (ImGui::MenuItem("Create Virtual Folder"))
                {
                    m_NewVirtualFolderName[0] = '\0';
                    m_InlineCreateParentFolder = folderName;
                    m_InlineCreatingFolder = true;
                    m_InlineFolderFocusSet = false;
                    // Auto-expand so the inline input is visible
                    m_VirtualFolders.SetExpanded(folderName, true);
                }
                if (ImGui::MenuItem("Rename Virtual Folder"))
                {
                    m_RenamingVirtualFolder = folderName;
                    // Show only the leaf name in the input
                    size_t lastSlash = node.displayName.rfind('/');
                    std::string leafName = (lastSlash == std::string::npos) ? node.displayName : node.displayName.substr(lastSlash + 1);
                    strncpy(m_NewVirtualFolderName, leafName.c_str(), sizeof(m_NewVirtualFolderName) - 1);
                    m_NewVirtualFolderName[sizeof(m_NewVirtualFolderName) - 1] = '\0';
                    m_InlineRenamingFolder = true;
                    m_InlineFolderFocusSet = false;
                }
                if (ImGui::MenuItem("Delete Virtual Folder"))
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
                // Draw child folders
                for (const auto& child : node.children)
                    drawVFolderNode(child);

                // Draw inline create if creating inside this folder
                drawInlineCreate(folderName);

                // Draw scripts assigned to this folder
                for (const auto& script : m_ScriptFlatList)
                {
                    if (getVirtualFolder(script) == folderName)
                        drawScript(script);
                }
            }
            DekiEditor::SchematicTreeBodyEnd();
            if (open) ImGui::TreePop();
            ImGui::PopID();
        };

        // Draw root-level virtual folder nodes
        for (const auto& node : rootNodes)
            drawVFolderNode(node);

        // Deferred delete (safe after iteration)
        if (!folderToDelete.empty())
            m_VirtualFolders.Remove(folderToDelete);

        // Inline folder creation at root level
        drawInlineCreate("");

        // Draw scripts at root (no virtual folder assigned)
        for (const auto& script : m_ScriptFlatList)
        {
            if (getVirtualFolder(script).empty())
                drawScript(script);
        }
    }

    // Fill remaining space so the child window is scrollable and right-clickable
    {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        if (avail.y > 0)
            ImGui::Dummy(ImVec2(avail.x, avail.y));
    }

    if (flatIndex == 0 && m_ScriptFlatList.empty())
    {
        ImGui::TextDisabled("No scripts found");
    }
    else if (flatIndex == 0 && !filterLower.empty())
    {
        ImGui::TextDisabled("No scripts match filter");
    }
}

void AssetBrowserPanel::DrawAssetDetails()
{
    if (!m_SelectedAsset)
    {
        ImGui::TextDisabled("Select an asset to view details");
        return;
    }

    const auto& asset = *m_SelectedAsset;

    ImGui::Text("Asset Details");
    ImGui::Separator();

    // Name
    ImGui::TextWrapped("Name: %s", asset.name.c_str());

    // Path
    ImGui::TextWrapped("Path: %s", asset.relativePath.c_str());

    // Extension
    namespace fs = std::filesystem;
    std::string ext = fs::path(asset.path).extension().string();
    ImGui::Text("Type: %s", ext.c_str());

    ImGui::Separator();

    // GUID info
    if (!asset.guid.empty())
    {
        ImGui::Text("GUID:");
        ImGui::TextWrapped("%s", asset.guid.c_str());
    }
    else if (IsTextureFile(ext))
    {
        ImGui::TextDisabled("No GUID (asset not imported)");
    }

    ImGui::Separator();

    // DTEX cache info
    if (IsTextureFile(ext))
    {
        ImGui::Text("Cache Status:");

        if (asset.hasCachedVersion)
        {
            ImGui::TextColored(DekiEditor::Palette::Green, "TEX cached");

            // Read and display TEX info
            TexData texData;
            if (TextureImporter::ReadTexFile(asset.cachePath, texData))
            {
                ImGui::Separator();
                ImGui::Text("TEX Info:");
                int imageW = 0, imageH = 0;
                TextureImporter::ReadSourceSize(asset.cachePath, imageW, imageH);
                ImGui::Text("  Size: %d x %d", imageW, imageH);
                if (imageW != static_cast<int>(texData.header.width) || imageH != static_cast<int>(texData.header.height))
                    ImGui::Text("  Stored: %u x %u (Max Size)", texData.header.width, texData.header.height);
                ImGui::Text("  Format: %s", texData.getFormatName().c_str());
                ImGui::Text("  Data: %.2f KB", texData.header.dataSize / 1024.0f);

                if (texData.hasAlpha())
                    ImGui::Text("  Alpha: Yes");
                if (texData.hasTransparency())
                    ImGui::Text("  Transparency: Yes");
            }
        }
        else if (!asset.guid.empty())
        {
            ImGui::TextColored(DekiEditor::Palette::Amber, "Not cached");
            ImGui::TextDisabled("Run asset build to generate");
        }
        else
        {
            ImGui::TextDisabled("Create GUID first");
        }
    }

    // Font cache info (BDF, TTF, OTF)
    if (ext == ".bdf" || ext == ".ttf" || ext == ".otf")
    {
        ImGui::Text("Cache Status:");

        if (asset.hasCachedVersion)
        {
            ImGui::TextColored(DekiEditor::Palette::Green, "Font cached");

            auto* pipeline = m_App ? m_App->GetAssetPipeline() : nullptr;
            if (pipeline && !asset.guid.empty())
            {
                const auto* subAssets = pipeline->GetSubAssets(asset.guid);
                if (subAssets)
                    ImGui::Text("  Variants: %zu", subAssets->size());
            }
        }
        else if (!asset.guid.empty())
        {
            ImGui::TextColored(DekiEditor::Palette::Amber, "Not cached");
        }
    }
}

void AssetBrowserPanel::DrawContextMenu()
{
    // Context menu for empty space in asset browser. Styled to match the top
    // menu bar's dropdowns (WindowPadding inset + taller, full-width-hover rows).
    PushContextMenuPopupStyle();
    if (ImGui::BeginPopupContextWindow("AssetBrowserContextMenu", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
    {
        PushContextMenuItemStyle();
        if (m_BrowserMode == BrowserMode::Scripts)
        {
            if (ImGui::MenuItem("Create C++ Script"))
            {
                m_ShowCreateScriptPopup = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Create Virtual Folder"))
            {
                m_NewVirtualFolderName[0] = '\0';
                m_InlineCreateParentFolder.clear();
                m_InlineCreatingFolder = true;
                m_InlineFolderFocusSet = false;
            }
        }
        else
        {
            if (ImGui::MenuItem("New Folder"))
            {
                m_CreateFolderDialog->Open();
            }

            // Creatable asset types from the registry, grouped under a single
            // "Create..." submenu (Assets mode only). The labels drop the
            // "Create " prefix since the submenu already conveys the verb.
            const auto& creatableTypes = AssetTypeRegistry::Instance().GetTypes();
            if (!creatableTypes.empty() && ImGui::BeginMenu("Create..."))
            {
                for (const auto& type : creatableTypes)
                {
                    if (ImGui::MenuItem(type.displayName.c_str()))
                    {
                        m_CreateAssetDialog->Open(type.typeName);
                    }
                }
                ImGui::EndMenu();
            }

            ImGui::Separator();
            if (ImGui::MenuItem("Reveal in File Explorer"))
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
    // Package-owned categories drive these checks. Adding a new texture format
    // is now a package change (one call to RegisterCategory / AssetTypeEditor
    // override) and does not require touching the asset browser.
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
        return;

    // Use hovered folder if available, otherwise use current directory
    const std::string targetDir = m_HoveredFolderPath.empty() ? m_CurrentPath : m_HoveredFolderPath;

    std::string error;
    if (!AssetImportDrop::Import(droppedPath, targetDir,
                                 m_App ? m_App->GetAssetPipeline() : nullptr, error))
    {
        // Not silently: a drop that produces nothing and says nothing reads
        // as a broken editor.
        DEKI_LOG_ERROR("Import skipped: %s", error.c_str());
        return;
    }

    // Refresh to show the new file
    NavigateTo(m_CurrentPath);
}

void AssetBrowserPanel::DeferMove(std::string srcPath, std::string destPath)
{
    m_DeferredActions.push_back(
        [this, src = std::move(srcPath), dest = std::move(destPath)]()
        {
            // Skip if something already occupies the destination (e.g. two
            // dropped files share a name - the first move wins). Checked at run
            // time, not enqueue time, so the order is respected.
            if (!std::filesystem::exists(dest))
                AssetFileOps::MoveWithData(src, dest, m_App ? m_App->GetAssetPipeline() : nullptr, m_ProjectRoot);
            m_PendingTreeRebuild = true;
        });
}

void AssetBrowserPanel::RunDeferredActions()
{
    if (m_DeferredActions.empty())
        return;

    // Move out first: an action may enqueue more (those run next frame), and we
    // must not iterate a vector an action could reallocate.
    std::vector<std::function<void()>> actions = std::move(m_DeferredActions);
    m_DeferredActions.clear();
    for (auto& action : actions)
        action();
}

void AssetBrowserPanel::DuplicateAsset(const std::string& srcPath)
{
    namespace fs = std::filesystem;

    fs::path src(srcPath);
    if (!fs::exists(src))
        return;

    fs::path parentDir = src.parent_path();
    std::string stem = src.stem().string();
    std::string ext = src.extension().string();
    bool isDir = fs::is_directory(src);

    // Find unique name: name_1, name_2, etc.
    fs::path destPath;
    for (int i = 1; i < 1000; ++i)
    {
        std::string newName = stem + "_" + std::to_string(i);
        if (!isDir)
            newName += ext;
        destPath = parentDir / newName;
        if (!fs::exists(destPath))
            break;
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
        return;

    // Give every copied sidecar a fresh GUID (import settings are preserved).
    //
    // For a directory this walks the whole copied tree: fs::copy clones the
    // .data files verbatim, so without this every asset inside the duplicate
    // carries the original's GUID. The cache then cannot tell the two apart and
    // scene references silently resolve to whichever one it saw last.
    if (isDir)
        AssetFileOps::ReassignGuidsInTree(destPath.string());
    else
        AssetFileOps::ReassignGuidForCopy(srcPath, destPath.string());

    // Register with asset pipeline
    if (m_App && m_App->GetAssetPipeline())
    {
        std::string newRelPath = fs::relative(destPath, m_ProjectRoot).string();
        for (char& c : newRelPath)
        {
            if (c == '\\') c = '/';
        }
        m_App->GetAssetPipeline()->RefreshAsset(newRelPath);
    }

    // Rebuild tree and reveal the new file
    m_PendingRevealPath = destPath.string();
    m_PendingTreeRebuild = true;
}

void AssetBrowserPanel::RenameAssetWithData(const std::string& oldPath, const std::string& newName)
{
    AssetFileOps::RenameWithData(oldPath, newName,
                                 m_App ? m_App->GetAssetPipeline() : nullptr, m_ProjectRoot);
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
        DEKI_LOG_ERROR("Create script: %s", error.c_str());
}

void AssetBrowserPanel::SaveObjectAsScene(Deki::Object* obj, const std::string& folderPath)
{
    namespace fs = std::filesystem;

    const std::string targetPath = SceneExport::SaveObjectAsScene(obj, folderPath);
    if (targetPath.empty())
        return;

    // Normalize path separators (Windows backslashes → forward slashes)
    std::string relativePath = fs::relative(targetPath, m_ProjectRoot).string();
    for (char& c : relativePath)
    {
        if (c == '\\') c = '/';
    }

    if (auto* pipeline = m_App->GetAssetPipeline())
    {
        pipeline->RefreshAsset(relativePath);

        // Get the GUID of the newly created scene for link prompt
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
