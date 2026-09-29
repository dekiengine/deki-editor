#pragma once

#include "core/SceneStats.h"

#include <string>
#include <memory>
#include <cstdint>
#include <vector>
#include <map>
#include <atomic>
#include <unordered_map>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <imgui.h>

class Sprite;
struct SDL_Window;

#include "core/Project.h"
#include <deki/Scene.h>  // Engine's Scene class
#include "panels/IPanel.h"
#include "panels/HierarchyPanel.h"
#include "panels/InspectorPanel.h"
#include "panels/SceneViewPanel.h"
#include "panels/AssetBrowserPanel.h"
#include "panels/ProjectSettingsPanel.h"
#include "panels/ConsolePanel.h"
#include "panels/BuildPanel.h"
#include "panels/NewProjectDialog.h"
#include "panels/ProjectManagerScreen.h"
#include "panels/PackageManagerPanel.h"
#include "panels/PlatformManagerPanel.h"
#include "panels/ProfilerPanel.h"
#include "panels/FrameCapturePanel.h"
#include "panels/SerialMonitorPanel.h"
#include "panels/SDCardDeployPanel.h"
#include "panels/DeployAnalysisPanel.h"
#include "panels/ActivityPanel.h"
#include <deki-editor/build/BuildActivitySink.h>
#include <deki-editor/build/BuilderRegistry.h>
#include "panels/PlayViewPanel.h"
#include <deki-editor/AssetPipeline.h>
#include <deki-editor/EditorApplication.h>
#include "services/ToolHost.h"
#include <deki-editor/EditorSettings.h>
#include "services/PluginLoader.h"
#include "services/PackageLoader.h"
#include "services/HotReloadService.h"
#include "services/FrameCaptureService.h"
#include "core/EditorSelection.h"
#include "core/EditorRecovery.h"
#include "core/UnsavedChangesPrompt.h"
#include "services/ProjectMaintenance.h"
#include "services/ProjectLock.h"
#include "PlayMode.h"
#include <optional>

namespace Deki { class LifecycleManager; } // Forward declaration (from engine)
namespace Deki { class IRenderSystem; } // Forward declaration (from engine)
namespace Deki { class ICamera; } // Forward declaration (from engine)

namespace DekiEditor
{

class McpServer;  // Forward declaration

class EditorApp
{
public:
    /// Asset-cache epoch. Bump when a change makes previously imported assets
    /// stale, to force a reimport on next open; leave alone otherwise.
    ///
    /// This is NOT the editor's release version - that lives in
    /// `project(DekiEditor VERSION ...)` in CMakeLists.txt, and the two are
    /// free to differ. It was named EDITOR_VERSION and stored in
    /// settings/editor_version.txt, which made it look like the release
    /// version and made the disagreement with CMake look like a bug.
    static constexpr const char* ASSET_CACHE_EPOCH = "1.1.0";

    EditorApp();
    ~EditorApp();

    void Update();
    void DrawStatusBar();
    void SaveLayout();

    /// Open a project. Returns false (and logs why) if the .dproj could not be
    /// loaded - it used to return void and bail silently, so pointing the editor
    /// at a corrupt deki.json did visibly nothing at all - or if another editor
    /// has it open, in which case the current project stays open.
    bool OpenProject(const std::string& path);
    void CloseProject();
    // Why the last OpenProject returned false, in words for the user; empty
    // after a successful open.
    const std::string& GetOpenProjectError() const { return m_OpenProjectError; }

    // OpenProject with the progress card up for all of it, the plugin build it
    // queues included. What every windowed open goes through.
    bool OpenProjectShowingProgress(const std::string& path);
    // Opens `path` on the next deferred-task pass, outside the ImGui frame:
    // the progress card cannot draw from inside one. For callers that run in
    // a frame (the project list, dialogs).
    void RequestOpenProject(const std::string& path);
    // Runs the plugin build an open queued, and waits out any other build.
    void FinishPendingBuilds();

    void NewScene();

    /// Quit, routed through the unsaved-changes prompt like every other action
    /// that discards the scene. main.cpp used to drop out of its loop on the
    /// raw SDL event, so the close button, Alt+F4 and File > Exit all threw
    /// away unsaved edits with no question asked. When nothing is dirty this
    /// sets the flag immediately; otherwise the prompt sets it on Save/Discard.
    void RequestQuit();
    bool ConsumeQuitRequest()
    {
        const bool q = m_QuitRequested;
        m_QuitRequested = false;
        // A granted quit is a normal exit: the scene was saved, discarded at
        // the prompt, or never changed, so this session's autosave snapshots
        // would only come back as a false "recovery" on the next open.
        if (q)
            m_Recovery.DiscardSessionSnapshots();
        return q;
    }

    // The two file dialogs the File menu and the keyboard shortcuts share, so
    // Ctrl+O / Ctrl+Shift+S cannot drift from the menu items that advertise them.
    void OpenProjectDialog();
    void SaveSceneAsDialog();

    /// Delete `obj` through the command system, with selection follow-up.
    /// Refuses protected objects (scene root, boot-scene Setup, instance
    /// children). This is the one rule; the Hierarchy and Scene View delete
    /// keys both call it - they used to implement it separately and the Scene
    /// View's copy applied no protection at all.
    bool DeleteObject(Deki::Object* obj);
    bool IsProtectedObject(Deki::Object* obj);

    void NavigateIntoScene(const std::string& path);
    void NavigateBack();
    void NavigateBackTo(size_t stackIndex);

    // Scene file operations. These report whether the file was actually read
    // or written - callers used to have no way to tell a successful save from a
    // path that went nowhere, which is how a scripted save could report success
    // while writing nothing.
    //
    // A relative path is resolved against the project root (see
    // ResolveProjectPath), not the process working directory.
    //
    // OpenScene returns false when the load failed AND when the open was
    // deferred behind the unsaved-changes dialog - in both cases the requested
    // scene is not what is open when it returns.
    bool OpenScene(const std::string& path);
    bool SaveScene();
    bool SaveSceneAs(const std::string& path);

    // Before a save: a 2D device drops depth on load, so name the objects that
    // carry it when the project has not declared transform_3d.
    void WarnIfDepthInFlatProject() const;

    // Absolute paths pass through unchanged; relative ones are taken as
    // project-relative.
    std::string ResolveProjectPath(const std::string& path) const;
    void EnsurePlatformSetup();

    // Export all assets from cache/ to storage/ for runtime, stored as the
    // active platform wants them
    bool ExportToStorage();

    // Point the asset cache at the project's active platform: images set to
    // Automatic are stored as that target's screen wants them. Call after the
    // active platform changes; with `reimport`, re-imports what that changed.
    void ApplyEditorTarget(bool reimport = true);

    // Propagate changes to all scenes that contain instances of the given scene
    void PropagateInstanceChanges(const std::string& changedSceneGuid);

    // Window (set by EditorBoot for the custom title bar; null when headless)
    void SetWindow(SDL_Window* window) { m_Window = window; }
    SDL_Window* GetWindow() const { return m_Window; }

    // Headless: started from the CLI, with no window, no GL context and no
    // ImGui context. The project still opens in full - this flag guards only
    // the few steps that exist for a UI that is not there. Every such site is
    // an explicit, commented decision; anything not marked runs identically in
    // both modes, which is what makes a CLI run and a GUI run comparable.
    void SetHeadless(bool headless);
    bool IsHeadless() const { return m_Headless; }

    // DPI scale (set from main.cpp, used by SceneView for screen-space conversions)
    void SetDpiScale(float scale) { m_DpiScale = scale; }
    float GetDpiScale() const { return m_DpiScale; }
    // Logical pixels → physical pixels at current ImGui DPI. Reads
    // GetWindowDpiScale() so per-monitor DPI changes are picked up live
    // (the m_DpiScale field captured at startup would not update).
    float Dpi(float logicalPixels) const { return logicalPixels * ImGui::GetWindowDpiScale(); }

    // Accessors
    Project* GetProject() { return m_Project.get(); }

    // The transform width the project's packages and deki.json declare, which
    // is what its simulator and firmware builds compile (COMPATIBILITY.md). The
    // editor itself always has every axis; this says what the device will have,
    // so the inspector can hide what a 2D device would drop on load.
    int GetProjectTransformWidth() const { return m_ProjectTransformWidth; }
    bool ProjectDeclaresDepth() const { return m_ProjectTransformWidth >= 3; }
    ::Deki::Scene* GetCurrentScene();
    const std::string& GetCurrentScenePath() const { return m_CurrentScenePath; }
    const std::vector<std::string>& GetSceneNavigationStack() const { return m_SceneNavigationStack; }
    AssetPipeline* GetAssetPipeline() { return m_AssetPipeline.get(); }
    PackageLoader* GetPackageLoader() { return m_PackageLoader.get(); }
    PluginLoader* GetPluginLoader() { return m_PluginLoader.get(); }
    ToolHost* GetToolHost() { return m_ToolHost.get(); }
    SceneViewPanel* GetSceneViewPanel() { return m_SceneViewPanel.get(); }
    InspectorPanel* GetInspectorPanel() { return m_InspectorPanel.get(); }
    AssetBrowserPanel* GetAssetBrowserPanel() { return m_AssetBrowserPanel.get(); }
    BuildPanel* GetBuildPanel() { return m_BuildPanel.get(); }
    const std::vector<IPanel*>& GetPanels() const { return m_Panels; }
    PackageManagerPanel* GetPackageManagerPanel() { return m_PackageManagerPanel.get(); }

    // Put packages that are NOT marked editable back the way the project pinned
    // them, stashing any local edits first, and toast whatever changed. Called
    // on window focus (scoped to packages the watchers saw change) and on
    // project open (sweepAll, since the watchers were not running).
    void ReportIncompatiblePackages();
    void RunPackageIntegrityCheck(bool sweepAll);
    PlatformManagerPanel* GetPlatformManagerPanel() { return m_PlatformManagerPanel.get(); }
    PlayViewPanel* GetPlayViewPanel() { return m_PlayViewPanel.get(); }
    FrameCaptureService* GetFrameCaptureService() { return m_FrameCaptureService.get(); }
    HotReloadService* GetHotReloadService() { return m_HotReloadService.get(); }

    // Selection - Multi-selection API
    const std::vector<Deki::Object*>& GetSelectedObjects() const { return m_Selection.GetSelectedObjects(); }
    Deki::Object* GetSelectedObject();  // Returns first selected or nullptr (backward compat)
    bool IsSelected(Deki::Object* obj) const { return m_Selection.IsSelected(obj); }
    void SetSelectedObject(Deki::Object* obj);  // Clear and set single
    void SetSelectedObjects(const std::vector<Deki::Object*>& objects);
    void AddToSelection(Deki::Object* obj);
    void RemoveFromSelection(Deki::Object* obj);
    void ToggleSelection(Deki::Object* obj);
    void ClearSelection();
    bool HasMultipleSelected() const { return m_Selection.HasMultipleSelected(); }
    size_t GetSelectionCount() const { return m_Selection.GetSelectionCount(); }

    // Called by Scene when objects are about to be destroyed. Drops the
    // doomed objects from selection and from HierarchyPanel caches before
    // their pointers go invalid.
    void OnObjectsDestroyed(const std::vector<Deki::Object*>& dying);

    // Asset selection (from asset browser)
    const std::string& GetSelectedAssetPath() const { return m_Selection.GetSelectedAssetPath(); }
    void SetSelectedAsset(const std::string& path, const std::string& cachePath, bool hasCached,
                          const std::string& guid = "");
    const std::string& GetSelectedAssetCachePath() const { return m_Selection.GetSelectedAssetCachePath(); }
    bool GetSelectedAssetHasCached() const { return m_Selection.GetSelectedAssetHasCached(); }
    const std::string& GetSelectedAssetGuid() const { return m_Selection.GetSelectedAssetGuid(); }

    // Play mode control
    void Play();
    void Pause();
    void Stop();
    PlayModeState GetPlayModeState() const { return m_PlayModeState; }
    bool IsPlaying() const { return m_PlayModeState == PlayModeState::Playing; }
    bool IsPaused() const { return m_PlayModeState == PlayModeState::Paused; }
    bool IsStopped() const { return m_PlayModeState == PlayModeState::Stopped; }

    // Play mode rendering
    uint32_t GetPlayModeTextureId() const { return m_Play.textureId; }
    int GetPlayModeWidth() const { return m_Play.width; }
    int GetPlayModeHeight() const { return m_Play.height; }
    bool IsPlayModeReady() const { return m_Play.ready; }

    // Smoothed game FPS (updated each game tick from m_Play.lastGameUpdateTime
    // deltas). Returns 0 when not in play mode or no samples yet.
    float GetGameFPS() const { return m_Play.gameFps; }

    // Play mode input (called from PlayViewPanel when in play mode)
    void HandlePlayModeInput(float gameX, float gameY, bool isDown, bool isMove, bool isUp);

    // Play mode keyboard (called from PlayViewPanel while the play view is
    // focused). Key codes follow InputKeys.h. Key-downs only reach the game
    // while playing; key-ups always pass so no key sticks pressed across a
    // focus change or stop.
    void HandlePlayModeKey(uint32_t key, bool down);

    // File drop handling
    void HandleFileDrop(const std::string& path);

    // Window focus handling (for hot reload)
    void OnWindowFocusGained();

    // Restore dock tab focus (call from main loop BEFORE DockSpace)
    void RestoreTabFocus();

    // Process deferred tasks (call from main loop after window is visible)
    void ProcessDeferredTasks();

    // 36px global toolbar (RUN/PAUSE/STOP transport on the left, plus future
    // mode/target chips). Render this BEFORE ImGui::DockSpace so it claims
    // a strip of vertical space above the docked panels.
    void DrawGlobalToolbar();

    // No project open means the project manager screen is up instead of the
    // editor. main.cpp asks before it builds the toolbar, dockspace and status
    // bar - none of which that screen wants behind it.
    bool IsProjectManagerMode() const { return m_Project == nullptr; }

    // Opens the new-project wizard. Reachable from File > New Project and from
    // the project manager screen, which has no menu bar to go through.
    void ShowNewProjectDialog();

private:
    void DrawMenuBar();

    // The project manager's own stripped-down title bar: wordmark and window
    // controls, no menus. Same bar geometry as DrawMenuBar so the borderless
    // window's drag and hit-test regions are unchanged.
    void DrawProjectManagerMenuBar();

    // The three Chromium-style window buttons, drawn into the bar spanning
    // barMin..barMax. Shared by both bars, and the only writer of the
    // hit-test globals the platform layer reads.
    void DrawWindowControls(const ImVec2& barMin, const ImVec2& barMax);

    void HandleKeyboardShortcuts();
    void RegisterAssetTypes();

    // Wire Scene notifications (object destruction) to editor-side cache cleanup.
    // Call after every assignment to m_CurrentScene.
    void RegisterSceneCallbacks(::Deki::Scene* scene);

    // The one way to install (or clear, with nullptr) the edited scene.
    //
    // Owning m_CurrentScene, publishing it to the EditorApplication facade and
    // registering its callbacks are three steps that must happen together. They
    // were open-coded at six sites and two of them skipped a step: the
    // hot-reload path reset the scene without clearing the facade and rebuilt it
    // without re-publishing, so EditorApplication::GetActiveScene() - public API
    // for package DLLs - held a freed pointer from the first reload onward.
    void SetCurrentScene(std::unique_ptr<::Deki::Scene> scene);

    // Check if editor version changed and return true if reimport is needed
    bool CheckAssetCacheEpochChanged(const std::string& projectPath);

    SDL_Window* m_Window = nullptr;
    bool m_Headless = false;
    bool m_QuitRequested = false;
    float m_DpiScale = 1.0f;
    std::unique_ptr<Project> m_Project;
    ProjectLock m_ProjectLock;  // held while m_Project is open
    std::string m_OpenProjectError;
    std::string m_PendingOpenProject;  // RequestOpenProject
    int m_ProjectTransformWidth = 2;  // 0, 2 or 3; see GetProjectTransformWidth
    std::unique_ptr<::Deki::Scene> m_CurrentScene;
    std::unique_ptr<AssetPipeline> m_AssetPipeline;
    std::unique_ptr<PluginLoader> m_PluginLoader;
    std::unique_ptr<PackageLoader> m_PackageLoader;
    std::unique_ptr<HotReloadService> m_HotReloadService;
    std::unique_ptr<FrameCaptureService> m_FrameCaptureService;

    // Panels
    std::unique_ptr<HierarchyPanel> m_HierarchyPanel;
    std::unique_ptr<InspectorPanel> m_InspectorPanel;
    std::unique_ptr<SceneViewPanel> m_SceneViewPanel;
    std::unique_ptr<AssetBrowserPanel> m_AssetBrowserPanel;
    std::unique_ptr<ProjectSettingsPanel> m_ProjectSettingsPanel;
    std::unique_ptr<ConsolePanel> m_ConsolePanel;
    std::unique_ptr<BuildPanel> m_BuildPanel;
    std::unique_ptr<NewProjectDialog> m_NewProjectDialog;
    std::unique_ptr<ProjectManagerScreen> m_ProjectManagerScreen;
    std::unique_ptr<PackageManagerPanel> m_PackageManagerPanel;
    std::unique_ptr<PlatformManagerPanel> m_PlatformManagerPanel;
    std::unique_ptr<ProfilerPanel> m_ProfilerPanel;
    std::unique_ptr<FrameCapturePanel> m_FrameCapturePanel;
    std::unique_ptr<SerialMonitorPanel> m_SerialMonitorPanel;
    std::unique_ptr<SDCardDeployPanel> m_SDCardDeployPanel;
    std::unique_ptr<DeployAnalysisPanel> m_DeployAnalysisPanel;
    std::unique_ptr<ActivityPanel> m_ActivityPanel;
    std::unique_ptr<PlayViewPanel> m_PlayViewPanel;
    // Every dockable panel above, in draw order (non-owning): drawn, restored
    // and saved through IPanel.
    std::vector<IPanel*> m_Panels;
    std::unique_ptr<ToolHost> m_ToolHost;

    // Selection state (object + asset, mutually exclusive)
    EditorSelection m_Selection;

    // Scene state
    std::string m_CurrentScenePath;
    std::vector<std::string> m_SceneNavigationStack;
    std::string m_NavigationSourcePath;  // Breadcrumb to push when scene actually opens
    bool m_IsNavigating = false;         // Flag to prevent stack clearing during navigation

    // Help > About Deki Editor: the version line a bug report should quote.
    bool m_ShowAboutWindow = false;
    void DrawAboutWindow();

#ifndef NDEBUG
    // ImGui debug windows - debug builds only (see EditorMenuBar's View menu).
    bool m_ShowDemoWindow = false;
    bool m_ShowMetricsWindow = false;
#endif

    // View > Restore Layout. Set from the menu, acted on by the frame loop:
    // rebuilding the dock tree has to happen next to the DockSpace() call, not
    // in the middle of drawing a menu.
    bool m_RestoreLayoutRequested = false;

public:
    /** @brief Ask for the default dock layout to be rebuilt on the next frame. */
    void RequestRestoreLayout() { m_RestoreLayoutRequested = true; }
    /** @brief True once per request, for the frame loop to act on. */
    bool ConsumeRestoreLayoutRequest()
    {
        const bool r = m_RestoreLayoutRequested;
        m_RestoreLayoutRequested = false;
        return r;
    }

private:
    // Play/pause/stop. Stays here rather than in PlayModeRuntime because six
    // sites outside PlayMode.cpp read it - including GetCurrentScene(), which
    // returns the engine root scene while not Stopped.
    PlayModeState m_PlayModeState = PlayModeState::Stopped;

    // Everything else play mode needs while running. Only PlayMode.cpp touches
    // it; see PlayMode.h for why this is a state bag rather than a controller.
    PlayModeRuntime m_Play;

    // Play mode helpers
    void InitPlayMode();
    void ShutdownPlayMode();
    void UpdatePlayMode();
    void RenderPlayMode();

    // Hot reload: serialize the scene before a plugin swap and restore it after.
    // Declared here with the rest of hot reload rather than among the play-mode
    // helpers, where they used to sit despite being implemented in
    // EditorApp.ProjectManager.cpp and having nothing to do with play mode.
    void OnBeforePluginReload();
    void OnAfterPluginReload();

    // Scene backup during hot reload (JSON string in memory)
    std::string m_HotReloadSceneBackup;
    std::string m_HotReloadScenePath;
    uint32_t m_HotReloadSelectedObjectId = 0;

    // Edit > Maintenance. The menu only records the request; the confirmation
    // modal (drawn from Update, outside the menu's draw scope) runs it. These
    // actions delete generated state that can cost a full rebuild to restore,
    // so they no longer run on a single click.
    std::optional<MaintenanceAction> m_PendingMaintenance;
    void DrawMaintenanceConfirm();

    // Unsaved changes dialog
    void DrawUnsavedChangesDialog();
    bool CheckUnsavedChanges(PendingSceneAction action, const std::string& path = "");
    void ExecutePendingAction();

    UnsavedChangesPrompt m_UnsavedPrompt;

    // Set by the Save As handler the prompt raised, once the write has landed.
    // The prompt itself has to do the closing, because ImGui::CloseCurrentPopup
    // only means anything inside the popup's own frame, and the dialog result
    // arrives outside one.
    bool m_UnsavedPromptSaved = false;

public:
    // Is the unsaved-changes dialog holding an operation? A scripted caller that
    // does not check this can keep issuing commands that appear to succeed while
    // the editor is still on the scene the dialog is guarding.
    bool IsUnsavedChangesDialogOpen() const { return m_UnsavedPrompt.IsOpen(); }

    // Answer it without a mouse. Takes effect on the next frame.
    void AnswerUnsavedChangesDialog(DialogAnswer answer) { m_UnsavedPrompt.Answer(answer); }
    // The crash-recovery window ("Recover Scenes"): open, and its "Later" -
    // dismiss for now, keeping every recovered file for the next open.
    bool IsRecoveryDialogOpen() const { return m_Recovery.DialogRequested(); }
    void PostponeRecoveryDialog() { m_Recovery.SetDialogRequested(false); }

private:
    // Tab focus restoration on startup
    std::string m_FocusRestoreWindow;

    // Crash recovery and autosave. File discovery, sidecar parsing, snapshot
    // writing and the autosave clock all live in EditorRecovery (unit-tested);
    // what stays here is the UI that presents them and the scene swap on
    // restore, which needs EditorApp's own state.
    EditorRecovery m_Recovery;
    void DrawRecoveryDialog();
    void DrawRecoveryDiscardConfirm();
    void DrawRecoveryPreview();
    /// Load the selected recovery, rebind it to its original path, and consume
    /// only that entry - the others stay on disk and are offered again.
    void RestoreSelectedRecovery();
    /// Write the current scene to the recovery directory. Used by the crash
    /// handler and the autosave timer alike, so a hard kill (which never reaches
    /// the crash handler) still leaves something to recover.
    bool WriteRecoverySnapshot();
    /// Advance the autosave timer and snapshot when it expires. Once per frame.
    void TickAutosave(float deltaSeconds);

    // MCP server
    std::unique_ptr<McpServer> m_McpServer;

    // For PropagateInstanceChanges: the GUIDs each scene file mentions, keyed
    // by absolute path and validated by the file's write time. Every save used
    // to read every scene in the project in full to search for one GUID.
    struct SceneGuidRefs
    {
        std::filesystem::file_time_type writeTime;
        std::vector<std::string> guids;
    };
    std::unordered_map<std::string, SceneGuidRefs> m_SceneGuidRefs;
    const std::vector<std::string>& SceneGuidsIn(const std::string& absPath);

public:
    // Port 0 (the default) takes any free one, which is what lets every
    // windowed editor run a server at once; --mcp asks for a fixed port. A
    // running server is published for the command line (LiveSession.h) and
    // republished whenever the open project changes.
    bool StartMcpServer(int port = 0);
    void StopMcpServer();
    // Clears the ImGui context pointer in deki-editor.dll and every package,
    // which each hold their own ImGui copy. Call before the context is
    // destroyed: those DLLs' static destructors free through it at exit.
    void ReleaseImGuiContext();
    bool IsMcpServerRunning() const;
    // Loopback SSE endpoint including the session token, or "" when not running.
    // This is what an MCP client must be pointed at - the token changes on every
    // Start(), so it cannot be baked into a config file.
    std::string GetMcpSseUrl() const;

private:
    void PublishLiveSession();
};

}  // namespace DekiEditor
