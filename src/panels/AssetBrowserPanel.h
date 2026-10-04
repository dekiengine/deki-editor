#pragma once

#include "IPanel.h"
#include <deki-editor/Motion.h>

#include <string>
#include <vector>
#include <map>
#include <set>
#include <memory>
#include <filesystem>
#include <functional>

#include "asset_browser/VirtualFolders.h"

namespace Deki
{
class Object;
}

namespace DekiEditor
{

class EditorApp;
class CreateFolderDialog;
class CreateAssetDialog;
class DeleteConfirmDialog;
class RenameDialog;

// Browser modes for the asset browser tabs
enum class BrowserMode
{
    Assets,  // project/assets/ - textures, scenes, animations, etc.
    Scripts  // project/src/ - C++ source files
};

struct AssetEntry
{
    std::string name;
    std::string path;
    std::string relativePath;
    std::string guid;       // Asset GUID from .data sidecar file
    std::string cachePath;  // Path to cached .tex file
    bool isDirectory;
    bool hasCachedVersion;  // True if .tex exists in cache

    // Sub-asset support (e.g., spritesheet frames, font variants)
    bool isExpandable = false;  // Has sub-assets (spritesheet frames)
    bool isExpanded = false;    // Currently showing sub-assets
    bool isSubAsset = false;    // This is a sub-asset
    std::string parentGuid;     // Parent asset GUID (for sub-assets)
    int subAssetIndex = -1;     // Sub-asset index (for sub-assets)
    int subAssetDepth = 0;      // Nesting depth (0=direct child, 1=grandchild)
};

// Tree node for folder hierarchy
struct FolderNode
{
    std::string name;
    std::string path;
    std::vector<FolderNode> subfolders;
    std::vector<AssetEntry> files;
};

class AssetBrowserPanel : public IPanel
{
public:
    const char* Id() const override { return "AssetBrowser"; }
    bool OpenByDefault() const override { return true; }
    AssetBrowserPanel(EditorApp* app);
    ~AssetBrowserPanel();

    void Draw();
    void RefreshAssets();

    // Mark the asset tree as stale so the next Draw rebuilds it. Used at
    // project open to push the asset tree walk off the critical startup path
    // and run it lazily on the first frame the panel actually renders.
    void MarkAssetsDirty() { m_AssetsDirty = true; }

    void HandleFileDrop(const std::string& droppedPath);

    // App accessor (for child dialogs to access DPI scaling, etc.)
    EditorApp* GetApp() const { return m_App; }

    // Public accessors for dialog classes
    const std::string& GetCurrentPath() const { return m_CurrentPath; }
    /// Project root. Delete commands need it so their trash directory lands on
    /// the same volume as the assets - std::filesystem::rename cannot cross one.
    const std::string& GetProjectRoot() const { return m_ProjectRoot; }
    const std::string& GetHoveredFolderPath() const { return m_HoveredFolderPath; }
    const std::vector<AssetEntry>& GetEntries() const { return m_Entries; }
    const std::set<int>& GetSelectedIndices() const { return m_SelectedIndices; }
    int GetSelectedIndex() const { return m_SelectedIndex; }
    void NavigateTo(const std::string& path);
    void RevealAsset(const std::string& absolutePath);
    void RenameAssetWithData(const std::string& oldPath, const std::string& newName);

    // Dialog accessors for triggering from context menus
    CreateFolderDialog* GetCreateFolderDialog() { return m_CreateFolderDialog.get(); }
    CreateAssetDialog* GetCreateAssetDialog() { return m_CreateAssetDialog.get(); }
    DeleteConfirmDialog* GetDeleteConfirmDialog() { return m_DeleteConfirmDialog.get(); }
    RenameDialog* GetRenameDialog() { return m_RenameDialog.get(); }

private:
    void DrawModeTabs();
    // Hierarchy-style search bar (dim magnifier + borderless transparent input).
    void DrawHierarchyStyleSearch(const char* id, char* buf, size_t bufSize);
    void DrawAssetTree();
    void DrawFolderNode(FolderNode& node, int& flatIndex);
    void DrawFileNode(AssetEntry& file, int flatIndex);
    void BuildFolderTree();
    void BuildFolderTreeRecursive(FolderNode& node, const std::filesystem::path& dirPath);

    // Scripts flat view with filter and virtual folders
    void DrawScriptsList();
    void BuildScriptFlatList();

    // Virtual folder tree for the Scripts view. Rebuilt (and sorted at every
    // level) when the folder list changes; it used to be rebuilt every frame.
    struct VFNode
    {
        std::string displayName;  // leaf name for display
        std::string fullPath;     // full path like "A/B/C"
        std::vector<VFNode> children;
    };
    std::vector<VFNode> m_VFTree;
    std::vector<std::string> m_VFTreeNames;  // the folder names m_VFTree was built from
    /// Assign a script and persist immediately - the panel has no other save
    /// point, so a grouping change must survive a crash before the next open.
    void MoveScriptToFolder(const std::string& relativePath, const std::string& folder);
    void DrawAssetDetails();
    void DrawContextMenu();
    bool IsTextureFile(const std::string& extension);
    bool IsSceneFile(const std::string& extension);
    bool IsAnimFile(const std::string& extension);
    bool IsBdfFile(const std::string& extension);
    bool IsSourceFile(const std::string& extension);
    bool IsHeaderFile(const std::string& extension);

    // Helper to move asset with its .data sidecar file

    // Queue a file/folder move to run after the current frame's tree draw. Drop
    // targets fire mid-iteration; moving now would rebuild the tree under the
    // live iterator (see RunDeferredActions / BuildFolderTree guard).
    void DeferMove(std::string srcPath, std::string destPath);

    // Run and clear actions queued from within the tree/list draw. Called once
    // in Draw() after EndChild, so every queued mutation happens past the
    // iteration. Actions may enqueue more; those run next frame.
    void RunDeferredActions();

    // Helper to duplicate an asset (file or folder) with its .data sidecar
    void DuplicateAsset(const std::string& srcPath);

    // A copied asset must never inherit the source's GUID - two files claiming
    // one identity make every reference to either of them ambiguous.

    // Save a hierarchy object as a new .scene file in the given folder
    void SaveObjectAsScene(Deki::Object* obj, const std::string& folderPath);

    // Create a new C++ script file
    void CreateScript(const std::string& directory, const std::string& name);

    EditorApp* m_App;

    // Browser mode (Assets or Scripts tab)
    BrowserMode m_BrowserMode = BrowserMode::Assets;

    std::string m_CurrentPath;
    std::string m_RootPath;     // Current root (assets or src based on mode)
    std::string m_ProjectRoot;  // project/ - used for computing relative paths
    std::string m_AssetsRoot;   // project/assets/
    std::string m_SrcRoot;      // project/src/
    std::vector<AssetEntry> m_Entries;
    AssetEntry* m_SelectedAsset = nullptr;
    int m_SelectedIndex = -1;
    int m_LastClickedIndex = -1;      // For shift-selection range
    int m_DeferredSingleSelect = -1;  // Deferred single-select for multi-drag support
    std::set<int> m_SelectedIndices;
    std::string m_CachedMultiDragPayload;  // Built at end of tree draw when m_Entries is complete
    // Path of the selected asset when the tree was last rebuilt; the selection
    // is restored on the next draw once m_Entries exists again. Rebuilds used
    // to drop the selection (and with it the Inspector) on any asset event.
    std::string m_ReselectPath;

    // Track hovered folder for external drops
    std::string m_HoveredFolderPath;

    // Which row the mouse is over, for the rows' hover fade.
    DekiEditor::Motion::RowHover m_RowHover;

    // Track expanded spritesheets (by GUID)
    std::set<std::string> m_ExpandedAssets;

    // Tree view
    FolderNode m_RootNode;
    std::set<std::string> m_ExpandedFolders;
    std::string m_PendingRevealPath;  // absolute path to select on next draw
    bool m_PendingTreeRebuild = false;
    // True only while DrawAssetTree/DrawScriptsList is iterating the node/entry
    // vectors. The Build* mutators check this and defer the rebuild (which would
    // free those vectors) to the post-draw pending-rebuild step. This is the hard
    // backstop: even if an action forgets to defer, the tree can't be rebuilt
    // mid-iteration.
    bool m_DrawingAssetView = false;

    // Actions queued from within the tree/list draw (moves, duplicates, save-as-
    // scene). RunDeferredActions() runs them once, after EndChild, so no draw
    // code mutates the model mid-frame. This is the primary layer; m_DrawingAssetView
    // is the backstop behind it.
    std::vector<std::function<void()>> m_DeferredActions;

    // Set by MarkAssetsDirty (e.g. at project open). Cleared by Draw after the
    // first RefreshAssets call, so the disk walk only runs when the panel is
    // actually about to render.
    bool m_AssetsDirty = false;

    // Script creation
    bool m_ShowCreateScriptPopup = false;
    char m_NewScriptName[256] = "";

    // Scripts flat view with filter and virtual folders
    struct ScriptEntry
    {
        std::string name;          // filename only
        std::string path;          // absolute path
        std::string relativePath;  // relative to src/ (forward slashes, used as display path)
    };
    std::vector<ScriptEntry> m_ScriptFlatList;
    char m_ScriptFilter[256] = "";

    // Assets-mode search filter. m_AssetFilterLower is the lowercased needle,
    // recomputed each frame in DrawAssetTree and read while drawing nodes.
    char m_AssetFilter[256] = "";
    std::string m_AssetFilterLower;

    // Virtual folders: maps script relativePath -> virtual folder name ("" = root)
    // Scripts-tab grouping. Persisted to script_folders.json; see
    // asset_browser/VirtualFolders.h.
    VirtualFolders m_VirtualFolders;
    char m_NewVirtualFolderName[256] = "";
    bool m_InlineCreatingFolder = false;     // showing inline input for new folder
    bool m_InlineRenamingFolder = false;     // showing inline input for rename
    bool m_InlineFolderFocusSet = false;     // track if we set focus on the input
    std::string m_RenamingVirtualFolder;     // folder being renamed
    std::string m_InlineCreateParentFolder;  // parent path for subfolder creation ("" = root)

    // Folder delete/rename
    std::string m_PendingDeleteFolderPath;
    std::string m_PendingRenameFolderPath;
    char m_RenameFolderBuffer[256] = "";

    // Link-to-scene prompt after drag-drop save
    bool m_ShowLinkPrompt = false;
    Deki::Object* m_PendingLinkObject = nullptr;
    std::string m_PendingLinkSceneGuid;

    // Dialog subsystems
    std::unique_ptr<CreateFolderDialog> m_CreateFolderDialog;
    std::unique_ptr<CreateAssetDialog> m_CreateAssetDialog;
    std::unique_ptr<DeleteConfirmDialog> m_DeleteConfirmDialog;
    std::unique_ptr<RenameDialog> m_RenameDialog;
};

}  // namespace DekiEditor
