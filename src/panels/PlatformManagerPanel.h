#pragma once

#include "IPanel.h"

#include <atomic>
#include <string>
#include <vector>
#include <memory>
#include "PlatformEditorDialog.h"

namespace DekiEditor
{

class EditorApp;
class BoardPackManager;
class ITargetBuilder;

/**
 * @brief Panel for managing build platforms and toolchains
 *
 * Provides UI for:
 * - Viewing all available platforms (Project Platforms tab)
 * - Creating/editing/deleting platforms
 * - Browsing and installing platform packs (Platform Packs tab)
 * - Importing platforms from packs into the project
 * - Toolchain installation and management (Toolchains tab)
 */
class PlatformManagerPanel : public IPanel
{
public:
    const char* Id() const override { return "PlatformManager"; }
    PlatformManagerPanel(EditorApp* app);
    ~PlatformManagerPanel();

    void Draw();



    /// Access the board pack manager
    BoardPackManager& GetPackManager() { return *m_PackManager; }

private:
    // Tab: Project Platforms
    void DrawProjectPlatformsTab();
    void DrawPlatformDetails();
    void RefreshPlatformList();

    // Tab: Platform Packs
    void DrawPlatformPacksTab();
    void DrawPackDetails();

    // Tab: Toolchains
    void DrawToolchainsTab();
    void DrawBuilderToolchains(ITargetBuilder* builder);
    void RefreshToolchains();

    EditorApp* m_App;
    PlatformEditorDialog m_PlatformEditor;
    std::unique_ptr<BoardPackManager> m_PackManager;

    // Active sub-nav tab: 0 = Project Platforms, 1 = Toolchains
    int m_ActiveTab = 0;

    // Project Platforms tab state
    std::string m_SelectedPlatformId;
    int m_SelectedIndex = -1;
    std::vector<std::pair<std::string, std::string>> m_PlatformList;
    bool m_ShowDeleteConfirm = false;
    std::string m_PlatformToDelete;

    // Resizable splitter
    float m_LeftPanelWidth = 240.0f;  // the list; the form beside it needs the room

    // Platform Packs tab state
    int m_SelectedPackIndex = -1;
    std::string m_SelectedPackId;
    bool m_HasFetchedPacks = false;

    // Toolchains tab state
    bool m_HasCheckedVersions = false;
    // Set by an install's completion callback, which runs on the installer's
    // worker thread; the refresh itself runs on the main thread in Draw.
    // Refreshing from the worker joined and reassigned other builders'
    // worker threads while the main thread could do the same: std::terminate.
    std::atomic<bool> m_RefreshToolchainsPending{ false };
    int m_SelectedBuilder = 0;            // selected toolchain in the left list
    float m_ToolchainLeftWidth = 220.0f;  // splitter width for the toolchains split
};

}  // namespace DekiEditor
