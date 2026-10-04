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

// The asset browser's tabs.
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
    std::string guid;       // from the .data sidecar file
    std::string cachePath;  // the cached .tex file
    bool isDirectory;
    bool hasCachedVersion;  // the .tex exists in the cache

    // Sub-assets, such as spritesheet frames or font variants.
    bool isExpandable = false;  // has sub-assets
    bool isExpanded = false;    // sub-assets are shown
    bool isSubAsset = false;
    std::string parentGuid;  // sub-assets only
    int subAssetIndex = -1;  // sub-assets only
    int subAssetDepth = 0;   // 0 = direct child, 1 = grandchild
};

// One folder in the asset tree.
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

    /// Marks the asset tree stale so the next Draw rebuilds it. Project open
    /// uses this so the disk walk runs on the panel's first drawn frame, not
    /// during startup.
    void MarkAssetsDirty() { m_AssetsDirty = true; }

    void HandleFileDrop(const std::string& droppedPath);

    // For child dialogs (DPI scaling and so on).
    EditorApp* GetApp() const { return m_App; }

    // For the dialog classes.
    const std::string& GetCurrentPath() const { return m_CurrentPath; }
    /// Project root. Delete commands put their trash directory under it so it
    /// is on the same volume as the assets: std::filesystem::rename cannot
    /// cross volumes.
    const std::string& GetProjectRoot() const { return m_ProjectRoot; }
    const std::string& GetHoveredFolderPath() const { return m_HoveredFolderPath; }
    const std::vector<AssetEntry>& GetEntries() const { return m_Entries; }
    const std::set<int>& GetSelectedIndices() const { return m_SelectedIndices; }
    int GetSelectedIndex() const { return m_SelectedIndex; }
    void NavigateTo(const std::string& path);
    void RevealAsset(const std::string& absolutePath);
    void RenameAssetWithData(const std::string& oldPath, const std::string& newName);

    // For opening the dialogs from context menus.
    CreateFolderDialog* GetCreateFolderDialog() { return m_CreateFolderDialog.get(); }
    CreateAssetDialog* GetCreateAssetDialog() { return m_CreateAssetDialog.get(); }
    DeleteConfirmDialog* GetDeleteConfirmDialog() { return m_DeleteConfirmDialog.get(); }
    RenameDialog* GetRenameDialog() { return m_RenameDialog.get(); }

private:
    void DrawModeTabs();
    // Search bar styled like the Hierarchy's: dim magnifier, borderless input.
    void DrawHierarchyStyleSearch(const char* id, char* buf, size_t bufSize);
    void DrawAssetTree();
    void DrawFolderNode(FolderNode& node, int& flatIndex);
    void DrawFileNode(AssetEntry& file, int flatIndex);
    void BuildFolderTree();
    void BuildFolderTreeRecursive(FolderNode& node, const std::filesystem::path& dirPath);

    // Scripts tab: a flat, filtered list grouped by virtual folders.
    void DrawScriptsList();
    void BuildScriptFlatList();

    // Virtual folder tree for the Scripts tab. Rebuilt, and sorted at every
    // level, only when the folder list changes.
    struct VFNode
    {
        std::string displayName;  // last part of the path
        std::string fullPath;     // like "A/B/C"
        std::vector<VFNode> children;
    };
    std::vector<VFNode> m_VFTree;
    std::vector<std::string> m_VFTreeNames;  // the folder names m_VFTree was built from
    /// Puts a script in a virtual folder and saves at once: the panel has no
    /// other save point, and the change must survive a crash.
    void MoveScriptToFolder(const std::string& relativePath, const std::string& folder);
    void DrawAssetDetails();
    void DrawContextMenu();
    bool IsTextureFile(const std::string& extension);
    bool IsSceneFile(const std::string& extension);
    bool IsAnimFile(const std::string& extension);
    bool IsBdfFile(const std::string& extension);
    bool IsSourceFile(const std::string& extension);
    bool IsHeaderFile(const std::string& extension);

    // Queues a file or folder move to run after this frame's tree draw. Drop
    // targets fire mid-iteration, and moving at once would rebuild the tree
    // under the live iterator.
    void DeferMove(std::string srcPath, std::string destPath);

    // Runs and clears the actions queued during the tree/list draw. Draw()
    // calls it once after EndChild, past the iteration. Actions queued by
    // these actions run next frame.
    void RunDeferredActions();

    // Duplicates a file or folder with its .data sidecar. The copy gets a new
    // GUID: two files with one identity make every reference to either ambiguous.
    void DuplicateAsset(const std::string& srcPath);

    // Saves a hierarchy object as a new .scene file in the folder.
    void SaveObjectAsScene(Deki::Object* obj, const std::string& folderPath);

    void CreateScript(const std::string& directory, const std::string& name);

    EditorApp* m_App;

    BrowserMode m_BrowserMode = BrowserMode::Assets;

    std::string m_CurrentPath;
    std::string m_RootPath;     // assets/ or src/, by tab
    std::string m_ProjectRoot;  // project/, for relative paths
    std::string m_AssetsRoot;   // project/assets/
    std::string m_SrcRoot;      // project/src/
    std::vector<AssetEntry> m_Entries;
    AssetEntry* m_SelectedAsset = nullptr;
    int m_SelectedIndex = -1;
    int m_LastClickedIndex = -1;      // anchor for shift-click ranges
    int m_DeferredSingleSelect = -1;  // single-select applied on release, so a multi-selection can be dragged
    std::set<int> m_SelectedIndices;
    std::string m_CachedMultiDragPayload;  // built at the end of the tree draw, once m_Entries is complete
    // Path of the selected asset when the tree was last rebuilt. The next draw
    // selects it again once m_Entries exists, so a rebuild keeps the selection
    // and the Inspector.
    std::string m_ReselectPath;

    // Hovered folder, the target for drops from outside the editor.
    std::string m_HoveredFolderPath;

    // Which row the mouse is over, for the rows' hover fade.
    DekiEditor::Motion::RowHover m_RowHover;

    // Expanded spritesheets, by GUID.
    std::set<std::string> m_ExpandedAssets;

    // Tree view
    FolderNode m_RootNode;
    std::set<std::string> m_ExpandedFolders;
    std::string m_PendingRevealPath;  // absolute path to select on the next draw
    bool m_PendingTreeRebuild = false;
    // True only while DrawAssetTree/DrawScriptsList iterate the node and entry
    // vectors. The Build* functions check it and put off the rebuild, which
    // frees those vectors, until after the draw. This is the backstop for an
    // action that forgets to defer itself.
    bool m_DrawingAssetView = false;

    // Actions queued during the tree/list draw (moves, duplicates, save as
    // scene). RunDeferredActions() runs them after EndChild, so no draw code
    // changes the model mid-frame. m_DrawingAssetView is the backstop.
    std::vector<std::function<void()>> m_DeferredActions;

    // Set by MarkAssetsDirty. Draw clears it after calling RefreshAssets, so
    // the disk walk runs only when the panel is about to draw.
    bool m_AssetsDirty = false;

    // Script creation
    bool m_ShowCreateScriptPopup = false;
    char m_NewScriptName[256] = "";

    // Scripts tab list.
    struct ScriptEntry
    {
        std::string name;          // file name only
        std::string path;          // absolute
        std::string relativePath;  // relative to src/, forward slashes; shown to the user
    };
    std::vector<ScriptEntry> m_ScriptFlatList;
    char m_ScriptFilter[256] = "";

    // Assets tab search filter. DrawAssetTree lowercases it into
    // m_AssetFilterLower each frame for the node draw to match against.
    char m_AssetFilter[256] = "";
    std::string m_AssetFilterLower;

    // Scripts tab grouping: script relativePath -> virtual folder name
    // ("" = root). Saved to script_folders.json; see
    // asset_browser/VirtualFolders.h.
    VirtualFolders m_VirtualFolders;
    char m_NewVirtualFolderName[256] = "";
    bool m_InlineCreatingFolder = false;     // inline input for a new folder is shown
    bool m_InlineRenamingFolder = false;     // inline input for a rename is shown
    bool m_InlineFolderFocusSet = false;     // the input has been given focus
    std::string m_RenamingVirtualFolder;     // folder being renamed
    std::string m_InlineCreateParentFolder;  // parent of the new folder ("" = root)

    // Folder delete/rename
    std::string m_PendingDeleteFolderPath;
    std::string m_PendingRenameFolderPath;
    char m_RenameFolderBuffer[256] = "";

    // After an object is dragged in and saved as a scene, offers to link it.
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
