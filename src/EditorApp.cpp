#include "EditorApp.h"
#include "EditorVersion.h"
#include "core/SceneStats.h"
#include <deki-editor/EditorTheme.h>
#include <deki-editor/EditorUI.h>
#include <deki-editor/EditorPaths.h>
#include <deki-editor/CommandHistory.h>
#include "core/SceneGuarantee.h"
#include <algorithm>
#include <chrono>
#include <functional>
#include <ctime>
#include <deki-editor/EditorApplication.h>
#include <deki-editor/EditorDLL.h>  // Deki::SetImGuiContext
#include <deki-editor/EditorAssets.h>
#include <deki/assets/AssetManager.h>
#include <deki-editor/build/PlatformConfig.h>
#include <deki-editor/EditorSettings.h>
#include <deki/profiling/Profiler.h>
#include "imgui.h"
#include "imgui_internal.h"

// Logging system
#include <deki/LogSystem.h>
#include "services/EditorLogFile.h"
#include "services/UpdateCheck.h"
// Engine (for GetCurrentScene play mode check)
#include <deki/Engine.h>
// Component registry (for FreezeEngineComponents before package load)
#include <deki/reflection/ComponentRegistry.h>
// Complete types needed to size the dying-memory ranges in OnObjectsDestroyed.
#include <deki/Object.h>
#include <deki/Component.h>
// Asset type registration
#include <deki-editor/AssetTypeRegistry.h>

// Force linker to include component registrations from deki-engine-core
#include <deki-editor/EditorExtension.h>
// Startup progress
#include "services/StartupProgress.h"
// Transient notifications (package restores, etc.)
#include "widgets/Toast.h"
#include "services/PackageIntegrity.h"
#include <deki-editor/Paths.h>
#include "services/installers/DekiPackageInstaller.h"
// Extracted services
#include "services/FileDialogs.h"
#include "core/Commands.h"
#include <filesystem>
#include "services/EngineEditorSetup.h"
// MCP server
#include "mcp/McpServer.h"
#include "services/LiveSession.h"
// Crash recovery
#include "services/CrashHandler.h"
// Per-frame exception barrier
#include "services/SafeDispatch.h"
// Recovery sidecar metadata
#include <nlohmann/json.hpp>
#include <fstream>

namespace DekiEditor
{

namespace
{

/// Where crash recovery for a project is kept: ~/.deki-editor/Recovery/<key>.
///
/// Not inside the project. It is editor state, not part of the game, and it
/// showed up as an untracked Recovery/ folder in every git status - one stray
/// `git add .` from being committed into someone's repo.
///
/// Not the OS temp directory either, tempting as that is for scratch state:
/// /tmp is cleared on boot on most Linux distributions, and Windows' Storage
/// Sense and Disk Cleanup both empty %TEMP%. A crash is often followed by a
/// reboot, which is precisely when the recovery still has to be there.
/// ~/.deki-editor is where the editor already keeps per-user state (toolchains,
/// board packs, builders), and it survives both.
///
}  // namespace

EditorApp::EditorApp()
{
    // EditorSelection owns selection; EditorApplication is the facade package
    // DLLs read. Mirror one into the other here, once, instead of at each
    // mutator - three of them used to forget, leaving GetActiveObject() stale.
    m_Selection.SetOnChanged([](Deki::Object* active)
                             { EditorApplication::Get().SetActiveObject(active); });

    // Open the persistent Editor.log first so any logging from the constructor
    // path (package builds, panel init) is captured to disk in addition to the
    // Console panel's in-memory ring buffer.
    EditorLogFile::Initialize(EditorSettings::GetSettingsDir() + "/Editor.log",
                              EditorSettings::GetBool("editorLog.verbose", false));

    // Create console panel and register log callback FIRST so that all startup
    // errors (including package builds) are captured in the Console panel.
    m_ConsolePanel = std::make_unique<ConsolePanel>(this);
    Deki::LogSystem::SetLogCallback([](Deki::LogLevel level, const std::string& message,
                                     const char* file, int line)
                                  {
        EditorLogFile::OnLog(level, message, file, line);
        ConsolePanel::EngineLogCallback(level, message, file, line); });

    // Create plugin loader (loads plugins when project is opened)
    m_PluginLoader = std::make_unique<PluginLoader>();

    // Build stale engine package DLLs (ensures they exist in exe dir).
    // Actual loading is deferred to OpenProject for per-project isolation.
    StartupProgress::Update("Building packages...");
    m_PackageLoader = std::make_unique<PackageLoader>();
    {
        namespace fs = std::filesystem;
        std::string exeDir;
#ifdef _WIN32
        char exePath[MAX_PATH];
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        exeDir = fs::path(exePath).parent_path().string();
#else
        exeDir = fs::current_path().string();
#endif
        // Snapshot engine-core components before any packages load.
        // ClearNonEngineComponents() uses this to safely remove all package/plugin
        // entries during hot reload without leaving stale pointers.
        Deki::ComponentRegistry::Instance().FreezeEngineComponents();
    }

    // Create frame capture service for recording play mode output
    m_FrameCaptureService = std::make_unique<FrameCaptureService>();

    // Create panels
    StartupProgress::Update("Creating panels...");
    m_HierarchyPanel = std::make_unique<HierarchyPanel>(this);
    m_InspectorPanel = std::make_unique<InspectorPanel>(this);
    m_SceneViewPanel = std::make_unique<SceneViewPanel>(this);
    m_AssetBrowserPanel = std::make_unique<AssetBrowserPanel>(this);
    m_ProjectSettingsPanel = std::make_unique<ProjectSettingsPanel>(this);

    // Register asset types for right-click creation
    RegisterAssetTypes();

    // Construct ToolHost and wire the per-tool progress callback. The actual
    // DiscoverTools() walk is deferred to OpenProject - bin/ packages may
    // register additional windows and that path always rebuilds the list
    // anyway, so doing it here just duplicates work.
    m_ToolHost = std::make_unique<ToolHost>(this);
    m_ToolHost->OnToolDiscovered = [](const std::string& name)
    {
        if (!StartupProgress::IsComplete())
            StartupProgress::Update("Discovering tools...", name.c_str());
    };

    // Create hot reload handler for plugin rebuilds and package reloads
    StartupProgress::Update("Setting up hot reload...");
    m_HotReloadService = std::make_unique<HotReloadService>(m_PluginLoader.get(), m_PackageLoader.get(), m_ConsolePanel.get());

    // Set up hot reload callbacks to preserve component state
    m_HotReloadService->SetReloadCallbacks(
        [this]()
        { OnBeforePluginReload(); },
        [this]()
        { OnAfterPluginReload(); });

    // Initialize platform registry with built-in platforms and board packs
    StartupProgress::Update("Registering platforms...");
    StartupProgress::Update("Loading board packs...");
    PlatformRegistry::Instance().LoadPlatformsFromBoardPacks();

    // Create build panel
    StartupProgress::Update("Creating panels...");
    m_BuildPanel = std::make_unique<BuildPanel>(this);

    // Create new project dialog
    m_NewProjectDialog = std::make_unique<NewProjectDialog>(this);
    m_NewProjectDialog->SetOnProjectCreated([this](const std::string& projectPath)
                                            {
        // Open the newly created project
        RequestOpenProject(projectPath); });

    // Create the project manager screen. It is what the editor shows until a
    // project is open, which on a normal launch is the very first frame.
    m_ProjectManagerScreen = std::make_unique<ProjectManagerScreen>(this);
    m_ProjectManagerScreen->OnShown();

    // Create package manager panel
    m_PackageManagerPanel = std::make_unique<PackageManagerPanel>(this);

    // Create platform manager panel
    m_PlatformManagerPanel = std::make_unique<PlatformManagerPanel>(this);

    // Create profiler panel
    m_ProfilerPanel = std::make_unique<ProfilerPanel>(this);

    // Create frame capture panel
    m_FrameCapturePanel = std::make_unique<FrameCapturePanel>(this);

    // Create serial monitor panel
    m_SerialMonitorPanel = std::make_unique<SerialMonitorPanel>(this);

    // Create SD card deploy panel
    m_SDCardDeployPanel = std::make_unique<SDCardDeployPanel>(this);

    // Create deploy analysis panel
    m_DeployAnalysisPanel = std::make_unique<DeployAnalysisPanel>(this);

    // Create activity panel
    m_ActivityPanel = std::make_unique<ActivityPanel>(this);

    // The build machinery reports progress through a sink rather than calling
    // the panel directly, so it can move into deki-editor.dll and be linked by
    // a third-party builder. The GUI's sink is the panel; a headless run
    // installs its own, or none.
    static struct PanelActivitySink : IBuildActivitySink
    {
        void Log(const std::string& process, const std::string& message) override
        {
            ActivityPanel::Log(process, message);
        }
        void Complete(const std::string& process) override
        {
            ActivityPanel::CompleteProcess(process);
        }
        void Fail(const std::string& process, const std::string& error) override
        {
            ActivityPanel::FailProcess(process, error);
        }
    } s_PanelSink;
    SetBuildActivitySink(&s_PanelSink);

    // Create play view panel
    m_PlayViewPanel = std::make_unique<PlayViewPanel>(this);

    m_Panels = {
        m_HierarchyPanel.get(), m_InspectorPanel.get(), m_SceneViewPanel.get(),
        m_AssetBrowserPanel.get(), m_ProjectSettingsPanel.get(), m_ConsolePanel.get(),
        m_BuildPanel.get(), m_PackageManagerPanel.get(), m_PlatformManagerPanel.get(),
        m_ProfilerPanel.get(), m_FrameCapturePanel.get(), m_SerialMonitorPanel.get(),
        m_SDCardDeployPanel.get(), m_DeployAnalysisPanel.get(), m_ActivityPanel.get(),
        m_PlayViewPanel.get()
    };

    // Restore panel open states from settings
    StartupProgress::Update("Restoring layout...");
    for (IPanel* panel : m_Panels)
    {
        panel->m_Open = panel->PersistOpenState()
                            ? EditorSettings::GetPanelOpen(panel->Id(), panel->OpenByDefault())
                            : panel->OpenByDefault();
    }

    // Load focused window name for tab restoration on startup
    m_FocusRestoreWindow = EditorSettings::GetFocusedWindow();

    // Register crash recovery callback to save current scene + flush log.
    // Flush always runs (even when no scene is open) so the log file captures
    // everything up to the crash before the dialog blocks the main thread.
    CrashHandler::SetPreCrashCallback([this]()
                                      {
        EditorLogFile::Flush();
        WriteRecoverySnapshot(); });
}

void EditorApp::SetHeadless(bool headless)
{
    m_Headless = headless;
    // Created before the boot says whether this run has a window.
    if (m_HotReloadService)
        m_HotReloadService->SetHeadless(headless);
}

EditorApp::~EditorApp()
{
    CrashHandler::SetPreCrashCallback(nullptr);
    StopMcpServer();

    // Explicitly, while the editor's statics are still alive. The registry is
    // a function-local static destroyed after main returns, and unloading a
    // builder plugin then would run its detach against dead statics. It also
    // cancels any build still running in a plugin's code before that module
    // is freed. Same discipline as PluginLoader::UnloadAllPlugins.
    BuilderRegistry::Instance().Shutdown();
    SetBuildActivitySink(nullptr);

    // Unregister the log callback before tearing down the file sink so a
    // late-firing log doesn't touch a closed FILE*. ConsolePanel is destroyed
    // along with this EditorApp, so the prior callback's reference to it
    // would also dangle.
    Deki::LogSystem::SetLogCallback(nullptr);
    EditorLogFile::Shutdown();
}

void EditorApp::SaveLayout()
{
    // Save panel open states to settings.json
    for (IPanel* panel : m_Panels)
    {
        if (panel->PersistOpenState())
            EditorSettings::SetPanelOpen(panel->Id(), panel->m_Open);
    }

    // Save focused window name for tab restoration on next launch
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (ctx && ctx->NavWindow)
    {
        EditorSettings::SetFocusedWindow(ctx->NavWindow->Name);
    }

    // Save imgui.ini (docking layout, selected tabs, window positions)
    ImGui::SaveIniSettingsToDisk(ImGui::GetIO().IniFilename);

    // Save project settings if dirty
    if (m_Project && m_Project->IsDirty())
    {
        m_Project->Save();
    }

    // One write for everything the setters above marked dirty.
    EditorSettings::FlushIfDirty();
}

void EditorApp::RestoreTabFocus()
{
    // Restore dock tab selection from last session.
    // DockNodeUpdateTabBar() overrides SelectedTabId with NavWindow's tab every frame,
    // so we set NavWindow to our target window to make that work in our favor.
    if (m_FocusRestoreWindow.empty())
        return;

    ImGuiWindow* win = ImGui::FindWindowByName(m_FocusRestoreWindow.c_str());
    if (!win || !win->DockId)
        return;

    ImGuiDockNode* node = ImGui::DockBuilderGetNode(win->DockId);
    if (!node || !node->TabBar || !win->TabId)
        return;

    node->SelectedTabId = win->TabId;
    node->TabBar->NextSelectedTabId = win->TabId;

    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (ctx)
        ctx->NavWindow = win;

    m_FocusRestoreWindow.clear();
}

void EditorApp::Update()
{
    DEKI_PROFILE_SCOPE_N("EditorApp::Update");

    // No project: the window belongs to the project manager screen, and none
    // of the work below has anything to act on. Everything the screen needs is
    // drawn here - its title bar, itself, and the new-project wizard it opens.
    if (IsProjectManagerMode())
    {
        DrawProjectManagerMenuBar();

        DekiEditor::Guard("ProjectManagerScreen", [&]
                          { m_ProjectManagerScreen->Draw(); });
        DekiEditor::Guard("NewProjectDialog", [&]
                          { m_NewProjectDialog->Draw(); });

        // Opening is deferred out of the screen's draw: OpenProject loads
        // package DLLs and imports assets, which has no business running from
        // the middle of a widget tree.
        const std::string requested = m_ProjectManagerScreen->ConsumePendingOpen();
        if (!requested.empty())
            RequestOpenProject(requested);

        // A refused open (the project is open in another editor) says so here.
        DekiEditor::Guard("Toast", [] { DekiEditor::Toast::DrawInFrame(); });

        return;
    }

    // Drive HotReloadService's per-project lifecycle (start/stop file watchers
    // when the active project changes) and drain pending source-watcher events
    // into m_SourcesDirty.
    if (m_HotReloadService)
    {
        m_HotReloadService->Tick(m_Project ? m_Project->GetPath() : std::string{});
    }

    // Drain the asset file watcher. This used to happen only on window focus,
    // so an asset edited in another tool showed up after an alt-tab and back,
    // and the pending event map grew until then. A drain with nothing pending
    // is a mutex and an empty check.
    if (m_AssetPipeline)
        m_AssetPipeline->Tick();

    // Handle keyboard shortcuts
    HandleKeyboardShortcuts();

    // Periodic flush of the editor log file (see EditorLogFile::OnLog).
    {
        static float s_LogFlushTimer = 0.0f;
        s_LogFlushTimer += ImGui::GetIO().DeltaTime;
        if (s_LogFlushTimer >= 0.5f)
        {
            s_LogFlushTimer = 0.0f;
            EditorLogFile::Flush();
        }
    }

    // Periodic crash-recovery snapshot of unsaved work.
    TickAutosave(ImGui::GetIO().DeltaTime);

    // Update play mode if active
    if (m_PlayModeState == PlayModeState::Playing)
    {
        UpdatePlayMode();
    }

    // Update custom editors every frame (e.g., ScrollComponent layout sync)
    if (m_PlayModeState != PlayModeState::Playing && m_SceneViewPanel)
        m_SceneViewPanel->UpdateCustomEditors();

    DrawMenuBar();

    // Draw panels - each guarded so a crash in one doesn't take the rest down.
    for (IPanel* panel : m_Panels)
    {
        DekiEditor::Guard(panel->Id(), [&]
                          { panel->Draw(); });
    }
    DekiEditor::Guard("NewProjectDialog", [&]
                      { if (m_NewProjectDialog) m_NewProjectDialog->Draw(); });

    // Draw modal dialogs
    DrawUnsavedChangesDialog();
    DrawMaintenanceConfirm();
    if (m_HotReloadService)
        m_HotReloadService->DrawBuildProgress();
    DrawRecoveryDialog();

    // Toasts last: they anchor to the same bottom-right corner as the build
    // progress card and stack above it, so that card must already be drawn.
    // The daily release check: started on the first frame of a windowed
    // session, delivered as a toast when its thread reports back.
    if (!m_Headless)
        DekiEditor::Guard("UpdateCheck", [] { UpdateCheck::Start(); UpdateCheck::Poll(); });

    DekiEditor::Guard("Toast", [] { DekiEditor::Toast::DrawInFrame(); });

    // Update all open tools (from engine/plugins)
    m_ToolHost->Update(ImGui::GetIO().DeltaTime);

    // Check if a package-DLL editor asked us to dispatch a tool window
    {
        std::string toolFile, toolCache;
        if (EditorApplication::Get().PopOpenToolRequest(toolFile, toolCache))
        {
            m_ToolHost->OpenToolForFile(toolFile.c_str(), toolCache.c_str());
        }
    }

    // Check if a package-DLL editor asked us to switch the scene-view tool
    // (e.g. ClipComponent's "Edit Rect" button activating the Rect resize tool).
    {
        SceneEditTool reqTool;
        if (EditorApplication::Get().PopSceneToolRequest(reqTool) && m_SceneViewPanel)
        {
            SceneTool tool = SceneTool::Select;
            switch (reqTool)
            {
                case SceneEditTool::Select: tool = SceneTool::Select; break;
                case SceneEditTool::Move: tool = SceneTool::Move; break;
                case SceneEditTool::Rect: tool = SceneTool::Rect; break;
            }
            m_SceneViewPanel->SetCurrentTool(tool);
        }
    }

    // A tool just modified files and asked us to surface the changes. Drain
    // the watcher (asset Tick is otherwise focus-driven) and rebuild the asset
    // browser tree.
    if (EditorApplication::Get().PopAssetRefreshRequest())
    {
        if (m_AssetPipeline)
            m_AssetPipeline->Tick();
        m_AssetBrowserPanel->RefreshAssets();
    }

    // Check if any tool requested full asset invalidation (e.g., font re-bake)
    if (EditorApplication::Get().PopAssetInvalidationRequest())
    {
        // 1. Clear component AssetRef pointers (TextComponent, SpriteComponent, etc.)
        if (GetSceneViewPanel())
            GetSceneViewPanel()->InvalidateAllAssets();

        // 2. Clear engine font cache (old BitmapFont* from stale .dfont files)
        Deki::AssetManager::Get()->ClearCache("BitmapFont");

        // 3. Clear editor GPU font atlas cache
        EditorAssets::Get()->ClearFontCache();
    }

    if (m_ShowAboutWindow)
        DrawAboutWindow();

#ifndef NDEBUG
    // ImGui's own debug windows, reachable from View in debug builds only.
    // The Metrics window carries Tools > Item Picker for measuring widget sizes.
    if (m_ShowDemoWindow)
    {
        ImGui::ShowDemoWindow(&m_ShowDemoWindow);
    }
    if (m_ShowMetricsWindow)
    {
        ImGui::ShowMetricsWindow(&m_ShowMetricsWindow);
    }
#endif
}

void EditorApp::DrawAboutWindow()
{
    if (!ImGui::IsPopupOpen("About Deki Editor"))
        ImGui::OpenPopup("About Deki Editor");
    if (BeginSchematicModal("About Deki Editor", 440.0f))
    {
        const std::string line = EditorVersionLine();
        ImGui::TextWrapped("%s", line.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, Palette::Dim);
        ImGui::TextWrapped("Compiler: %s", DEKI_EDITOR_CXX_COMPILER);
        const std::string logPath = EditorLogFile::GetLogPath();
        if (!logPath.empty())
            ImGui::TextWrapped("Log: %s", logPath.c_str());
        ImGui::PopStyleColor();
        // A bug report wants the exact line, not a retyped one.
        const int button = SchematicDialogButtons({ { "Copy Version" }, { "Close", DialogButton::Primary } });
        if (button == 0)
            ImGui::SetClipboardText(line.c_str());
        if (button == 1)
        {
            m_ShowAboutWindow = false;
            ImGui::CloseCurrentPopup();
        }
        EndSchematicModal();
    }
    else
    {
        m_ShowAboutWindow = false;
    }
}

void EditorApp::DrawStatusBar()
{
    m_ActivityPanel->DrawStatusBar();
}

void EditorApp::HandleKeyboardShortcuts()
{
    ImGuiIO& io = ImGui::GetIO();

    // Don't process shortcuts when typing in a text field
    if (io.WantTextInput)
        return;

    // Ctrl+Z - Undo
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z))
    {
        if (io.KeyShift)
        {
            // Ctrl+Shift+Z - Redo (alternative to Ctrl+Y)
            CommandHistory::Instance().Redo();
        }
        else
        {
            CommandHistory::Instance().Undo();
        }
    }

    // Ctrl+Y - Redo
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y))
    {
        CommandHistory::Instance().Redo();
    }

    // File shortcuts, exactly the ones the File menu advertises. They were
    // printed next to the menu items for a long time without being bound.
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S))
    {
        if (io.KeyShift)
            SaveSceneAsDialog();
        else if (m_CurrentScene && !m_CurrentScenePath.empty())
            SaveScene();
        else if (m_CurrentScene)
            SaveSceneAsDialog();  // untitled: Ctrl+S needs a destination
    }
    if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_O))
        OpenProjectDialog();
    if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_N))
        ShowNewProjectDialog();

    // Ctrl+Shift+P - Stop
    // (Check Shift combo first so Ctrl+P without Shift can be handled separately)
    if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_P) && !IsStopped())
    {
        Stop();
    }
    // Ctrl+P - Play/Pause toggle
    else if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_P) && GetCurrentScene())
    {
        if (IsPlaying())
            Pause();
        else
            Play();  // Works for both stopped and paused states
    }
}

// Scene accessor - returns the active container's scene
// During play: returns the isolated play copy (same one the runtime uses)
// When stopped: returns the edit scene
Deki::Scene* EditorApp::GetCurrentScene()
{
    if (m_PlayModeState != PlayModeState::Stopped)
    {
        Deki::Scene* engineScene = Deki::Engine::GetInstance().GetRootScene();
        if (engineScene)
            return engineScene;
    }

    // The editor always has a scene to edit - there is no usable "no scene"
    // state. On the first access with no project open (m_CurrentScene still
    // null), install a fresh empty scene and wire it up exactly like New Scene.
    // This makes the invariant real so the rest of the editor never null-checks.
    if (!m_CurrentScene)
    {
        EnsureScene(m_CurrentScene);
        // EnsureScene assigned m_CurrentScene directly; re-seat it through the
        // one owner so the facade and callbacks follow.
        SetCurrentScene(std::move(m_CurrentScene));
    }
    return m_CurrentScene.get();
}

// Selection implementation
Deki::Object* EditorApp::GetSelectedObject()
{
    return m_Selection.GetSelectedObject();
}

void EditorApp::SetSelectedObject(Deki::Object* obj)
{
    m_Selection.SetSelectedObject(obj);
}

void EditorApp::SetSelectedObjects(const std::vector<Deki::Object*>& objects)
{
    m_Selection.SetSelectedObjects(objects);
}

void EditorApp::AddToSelection(Deki::Object* obj)
{
    m_Selection.AddToSelection(obj);
}

void EditorApp::RemoveFromSelection(Deki::Object* obj)
{
    m_Selection.RemoveFromSelection(obj);
}

void EditorApp::ToggleSelection(Deki::Object* obj)
{
    m_Selection.ToggleSelection(obj);
}

void EditorApp::ClearSelection()
{
    m_Selection.ClearObjectSelection();
}

void EditorApp::SetCurrentScene(std::unique_ptr<::Deki::Scene> scene)
{
    m_CurrentScene = std::move(scene);
    // Publish before registering callbacks: both read m_CurrentScene, and the
    // facade must never name a scene that has been freed.
    EditorApplication::Get().SetActiveScene(m_CurrentScene.get());
    if (m_CurrentScene)
        RegisterSceneCallbacks(m_CurrentScene.get());
}

void EditorApp::RegisterSceneCallbacks(::Deki::Scene* scene)
{
    if (!scene) return;
    scene->SetOnObjectsAboutToBeDestroyed(
        [this](const std::vector<Deki::Object*>& dying)
        { OnObjectsDestroyed(dying); });
}

void EditorApp::OnObjectsDestroyed(const std::vector<Deki::Object*>& dying)
{
    if (dying.empty()) return;

    // The scene view's cached camera may be among the dying objects.
    if (m_SceneViewPanel)
        m_SceneViewPanel->InvalidateSceneCamera();

    // Drop undo/redo entries that point into the memory about to be freed.
    //
    // Commands address their targets by raw pointer - objects, components, and
    // (via the EditorUI field helpers) interior addresses like &obj->x. Without
    // this, a command left on either stack after its target dies writes through
    // a dangling pointer the next time it is undone or redone.
    {
        DeadMemory dead;
        for (Deki::Object* obj : dying)
        {
            if (!obj) continue;
            dead.Add(obj, sizeof(Deki::Object));

            // Components die with their object. instanceSize comes from the
            // reflection metadata; when a component's type isn't registered we
            // can only cover the base subobject, which still catches a command
            // holding the component pointer itself.
            auto& registry = Deki::ComponentRegistry::Instance();
            for (Deki::Component* comp : obj->GetComponents())
            {
                if (!comp) continue;
                const Deki::ComponentMeta* meta = registry.GetMeta(comp->GetType());
                const size_t size = (meta && meta->instanceSize > 0)
                                        ? meta->instanceSize
                                        : sizeof(Deki::Component);
                dead.Add(comp, size);
            }
        }
        CommandHistory::Instance().DiscardCommandsReferencing(dead);
    }

    // Trimming the selection reseats the EditorApplication facade through
    // EditorSelection's change callback.
    m_Selection.RemoveObjects(dying);

    if (m_HierarchyPanel)
        m_HierarchyPanel->OnObjectsDestroyed(dying);
}

void EditorApp::SetSelectedAsset(const std::string& path, const std::string& cachePath, bool hasCached,
                                 const std::string& guid)
{
    m_Selection.SetSelectedAsset(path, cachePath, hasCached, guid);

    // Auto-compile asset if not cached
    if (!hasCached && m_AssetPipeline && !path.empty())
    {
        m_AssetPipeline->RefreshAsset(path);

        // Update cached state after refresh
        auto* info = m_AssetPipeline->GetAssetInfo(path);
        if (info)
        {
            // Update cache state directly via SetSelectedAsset to keep fields consistent
            m_Selection.SetSelectedAsset(path, info->cachePath, info->hasCachedVersion, guid);
        }
    }
}

void EditorApp::HandleFileDrop(const std::string& path)
{
    if (m_AssetBrowserPanel)
    {
        m_AssetBrowserPanel->HandleFileDrop(path);
    }
}

void EditorApp::OnWindowFocusGained()
{
    DEKI_LOG_EDITOR("Window focus gained%s",
                    m_Project ? " (requesting package + build check)"
                              : " (no project; requesting package check only)");

    // Always check for package DLL changes (even without a project open)
    if (m_HotReloadService)
    {
        m_HotReloadService->RequestPackageCheck();
    }

    if (!m_Project)
        return;

    // Request build check - will be processed next frame when window is visible
    // This ensures the progress bar is visible during compilation
    if (m_HotReloadService)
    {
        m_HotReloadService->RequestBuildCheck(m_Project->GetPath());
    }

    // The asset watcher is drained every frame in Update(); nothing is
    // pending only because focus was lost. The whole-tree browser rescan that
    // used to run here (a sidecar read and an exists() per asset, and it wiped
    // the asset selection) is gone: discovered, changed and deleted assets
    // reach the browser through the pipeline callbacks.
}

void EditorApp::ReportIncompatiblePackages()
{
    if (!m_Project)
        return;

    // The installer checks this when a package is ADDED, which misses the
    // common case entirely: cloning a project whose packages are committed, or
    // restoring them from the lock, puts packages on disk without ever going
    // through an install. Reporting here means a mismatch arrives once, naming
    // the package and both versions, instead of surfacing later as a compile
    // error inside a project build with nothing pointing at the cause.
    //
    // Deliberately not part of RunPackageIntegrityCheck: that one is about git
    // state, needs the hot-reload service, and returns early without it, so
    // headless verbs would never have run this.
    //
    // Editable packages are checked too - a working tree from an older
    // generation does not build either - and nothing here changes disk.
    const std::string projectPath = m_Project->GetPath();
    PackageManager mgr(this);
    mgr.LoadPackages(projectPath);  // also refreshes installed status

    const std::filesystem::path packagesDir =
        std::filesystem::path(DekiEditor::GetPackagesDirectory(projectPath));

    for (const PackageEntry& pkg : mgr.GetPackages())
    {
        if (!pkg.isInstalled)
            continue;
        const std::string dirName = DekiPackageInstaller::GetRepoName(pkg);
        if (dirName.empty())
            continue;
        std::string reason;
        if (DekiPackageInstaller::EngineSatisfiesPackage(packagesDir / dirName, reason))
            continue;

        const std::string message = "'" + dirName + "': " + reason;
        DEKI_LOG_ERROR("Package compatibility: %s", message.c_str());
        if (!m_Headless)
        {
            DekiEditor::Toast::Push(DekiEditor::Toast::Level::Error,
                                    "Package is not compatible with this editor", message, 15.0f);
            if (m_ConsolePanel)
                m_ConsolePanel->LogError(message);
        }
    }
}

void EditorApp::RunPackageIntegrityCheck(bool sweepAll)
{
    if (!m_Project || !m_HotReloadService)
        return;

    // Always drain, even when sweeping: leaving entries behind would make the
    // next focus re-check packages this pass already handled.
    bool allSuspect = false;
    const std::vector<std::string> dirty = m_HotReloadService->TakeDirtyPackageDirs(allSuspect);
    const bool checkEverything = sweepAll || allSuspect;

    // Bail before any file IO. This is the every-alt-tab case.
    if (!checkEverything && dirty.empty())
        return;

    // Its own manager rather than the Package Manager panel's: the panel loads
    // lazily when it first draws, so in a headless run (and before the panel is
    // ever opened) its list is empty and every package would be skipped. The
    // headless verbs read the manifest the same way.
    const std::string projectPath = m_Project->GetPath();
    PackageManager mgr(this);
    mgr.LoadPackages(projectPath);  // also refreshes installed status

    const std::filesystem::path packagesDir =
        std::filesystem::path(DekiEditor::GetPackagesDirectory(projectPath));

    for (const PackageEntry& pkg : mgr.GetPackages())
    {
        if (!pkg.isInstalled)
            continue;

        // The developer's own package. Hands off entirely - no status check,
        // no checkout, nothing to report.
        if (pkg.editable)
            continue;

        const std::string dirName = DekiPackageInstaller::GetRepoName(pkg);
        if (dirName.empty())
            continue;

        if (!checkEverything &&
            std::find(dirty.begin(), dirty.end(), dirName) == dirty.end())
        {
            continue;
        }

        // The lock is the committed pin; installedVersion is re-derived from
        // .deki-package.json on disk and so follows whatever is checked out.
        const std::string pinned =
            !pkg.lockedVersion.empty() ? pkg.lockedVersion : pkg.installedVersion;

        const DekiEditor::PackageIntegrityResult result =
            DekiEditor::RestorePackage(packagesDir / dirName, dirName, pinned);

        if (!result.NeedsReport())
            continue;

        if (result.action == DekiEditor::PackageIntegrityResult::Action::Failed)
        {
            DekiEditor::Toast::Push(DekiEditor::Toast::Level::Error,
                                    "Package restore failed", result.detail, 12.0f);
            if (m_ConsolePanel)
                m_ConsolePanel->LogWarning(result.detail);
            else
                DEKI_LOG_ERROR("Package restore failed: %s", result.detail.c_str());
            continue;
        }

        // One sentence covering both halves: a single pass can stash edits and
        // move HEAD back, and the user needs to hear about both.
        std::string message = "'" + dirName + "' is not marked editable, so it was restored";
        if (!result.restoredRef.empty())
            message += " to " + result.restoredRef;
        if (!result.stashMessage.empty())
            message += ". Your changes are safe in git stash (\"" + result.stashMessage + "\")";
        else
            message += ".";

        DekiEditor::Toast::Push(DekiEditor::Toast::Level::Warning,
                                "Package restored", message, 12.0f);

        // The toast is what gets seen; the Console line is what can be scrolled
        // back to. Deliberately NOT LogBuildError - pinned entries are the
        // compiler's channel and feed the "Build failed" pill.
        if (m_ConsolePanel)
            m_ConsolePanel->LogWarning(message);

        // Headless has neither surface, and every CLI verb opens the project,
        // so on the command line this pass could revert an edit and say nothing
        // at all. Silently undoing someone's work is the worst thing this
        // function can do, so it always reaches a channel that exists.
        if (!m_ConsolePanel)
            DEKI_LOG_WARNING("%s", message.c_str());
    }
}

void EditorApp::ProcessDeferredTasks()
{
    // Hand a finished file dialog its answer first, outside the ImGui frame:
    // the handlers open projects and write scenes.
    FileDialogs::PumpResults();

    // A project the project list or a dialog asked for, opened here because
    // the progress card cannot draw from inside the ImGui frame they run in.
    if (!m_PendingOpenProject.empty())
    {
        const std::string path = std::move(m_PendingOpenProject);
        m_PendingOpenProject.clear();
        OpenProjectShowingProgress(path);
    }

    // Advance a running build first: a finished step may need the reload tail,
    // which has to run here, outside the ImGui frame, like the checks below.
    if (m_HotReloadService)
        m_HotReloadService->PumpBuild();

    // Restore non-editable packages BEFORE the reload check below: a restore
    // rewrites files, and the mtimes it produces are what that check compares
    // against. Running it second would rebuild from the edits we just reverted.
    RunPackageIntegrityCheck(false);

    // Process pending package reload check (per-package granular reload)
    if (m_HotReloadService && m_HotReloadService->HasPendingPackageCheck())
    {
        m_HotReloadService->ProcessPendingPackageCheck();
    }

    // Process pending build check after window is fully visible
    if (m_HotReloadService && m_HotReloadService->HasPendingBuild())
    {
        m_HotReloadService->ProcessPendingBuild();
    }

    // Process MCP server messages
    if (m_McpServer)
    {
        m_McpServer->ProcessMessages();
    }
}

bool EditorApp::StartMcpServer(int port)
{
    if (m_McpServer) return true;
    m_McpServer = std::make_unique<McpServer>(this);
    if (!m_McpServer->Start(port))
    {
        m_McpServer.reset();
        return false;
    }
    PublishLiveSession();
    return true;
}

void EditorApp::StopMcpServer()
{
    if (m_McpServer)
    {
        m_McpServer->Stop();
        m_McpServer.reset();
        LiveSession::Withdraw();
    }
}

void EditorApp::ReleaseImGuiContext()
{
    if (m_PackageLoader)
        m_PackageLoader->ReleaseImGuiContext();
    Deki::SetImGuiContext(nullptr);
}

void EditorApp::PublishLiveSession()
{
    if (m_McpServer && m_McpServer->IsRunning())
        LiveSession::Publish(m_McpServer->GetPort(), m_McpServer->GetToken(),
                             m_Project ? m_Project->GetPath() : std::string());
}

bool EditorApp::IsMcpServerRunning() const
{
    return m_McpServer && m_McpServer->IsRunning();
}

std::string EditorApp::GetMcpSseUrl() const
{
    return m_McpServer ? m_McpServer->GetSseUrl() : std::string{};
}

namespace
{

/// Pack a theme colour into EditorUI's RGBA word for PushStyleColor.
/// Same helper the panels keep locally (see PackageManagerPanel's Pack).
inline uint32_t PackColor(const ImVec4& c)
{
    return EditorUI::Rgba(static_cast<uint8_t>(c.x * 255.0f), static_cast<uint8_t>(c.y * 255.0f),
                          static_cast<uint8_t>(c.z * 255.0f), static_cast<uint8_t>(c.w * 255.0f));
}

/// "2026-08-16 14:32  (2 hours ago)" - absolute so it can be matched against
/// what the user remembers doing, relative because that is what actually tells
/// them whether it is worth taking.
std::string FormatRecoveryTime(std::time_t when)
{
    if (when <= 0)
        return "unknown time";

    char stamp[32] = {};
    std::tm tmBuf{};
#ifdef _WIN32
    localtime_s(&tmBuf, &when);
#else
    localtime_r(&when, &tmBuf);
#endif
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M", &tmBuf);

    const std::time_t now = std::time(nullptr);
    const long long secs = static_cast<long long>(now - when);

    // Singular where it applies: "1 days ago" is the kind of thing that makes a
    // dialog look unfinished.
    auto ago = [](long long n, const char* unit)
    {
        return std::to_string(n) + " " + unit + (n == 1 ? "" : "s") + " ago";
    };

    std::string rel;
    if (secs < 0)
        rel = "in the future";  // clock changed; say so rather than print nonsense
    else if (secs < 90)
        rel = "just now";
    else if (secs < 3600)
        rel = ago(secs / 60, "minute");
    else if (secs < 86400)
        rel = ago(secs / 3600, "hour");
    else
        rel = ago(secs / 86400, "day");

    return std::string(stamp) + "  (" + rel + ")";
}

}  // namespace

void EditorApp::DrawRecoveryDialog()
{
    if (!m_Recovery.DialogRequested())
        return;

    auto& ui = EditorUI::Get();

    ui.OpenPopup("Recover Scenes");

    if (BeginSchematicModal("Recover Scenes", 460.0f))
    {
        ui.PushStyleColor(EditorUI::Col::Text, PackColor(Palette::Dim));
        ui.TextWrapped("Saved when the editor crashed. Pick one to restore, "
                       "or double-click it to see what is inside.");
        ui.PopStyleColor();

        // Set while drawing the list, opened after it: OpenPopup inside the
        // child would nest the popup under it rather than the dialog.
        bool openPreview = false;

        ui.Spacing();

        if (m_Recovery.SelectedIndex() >= static_cast<int>(m_Recovery.Entries().size()))
            m_Recovery.SetSelectedIndex(0);

        // SelectableCard is the list row used everywhere else (package list,
        // platform list): title, dim subtitle, and a left accent bar on the
        // selected one. Hand-rolling this was what made the selection invisible.
        //
        // Height mirrors DrawSelectableCard's own layout (padY*2 + title + gap +
        // subtitle) rather than a guessed constant - guessing it clipped the last
        // card. Using the title height for the subtitle too rounds up slightly,
        // which is the safe direction.
        const float cardDpi = ImGui::GetStyle().FontScaleDpi;
        const float cardH = 6.0f * cardDpi * 2.0f + ImGui::GetTextLineHeight() + 2.0f * cardDpi + ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y;
        const float shown = ImMin(static_cast<float>(m_Recovery.Entries().size()), 4.0f);
        const float listH = cardH * shown + ImGui::GetStyle().WindowPadding.y;
        if (ui.BeginChild("##recovery_list", 0.0f, listH, /*border*/ true))
        {
            for (int i = 0; i < static_cast<int>(m_Recovery.Entries().size()); ++i)
            {
                const auto& rec = m_Recovery.Entries()[i];

                // One line, because a card takes a single subtitle: when it was
                // saved and what is in it, which together decide whether it is
                // worth keeping.
                char subtitle[192];
                if (rec.objectCount < 0)
                    snprintf(subtitle, sizeof(subtitle), "%s  \xc2\xb7  unreadable  \xc2\xb7  %llu bytes",
                             FormatRecoveryTime(rec.savedAt).c_str(),
                             static_cast<unsigned long long>(rec.fileSize));
                else
                    snprintf(subtitle, sizeof(subtitle), "%s  \xc2\xb7  %d object%s  \xc2\xb7  %llu bytes",
                             FormatRecoveryTime(rec.savedAt).c_str(),
                             rec.objectCount, rec.objectCount == 1 ? "" : "s",
                             static_cast<unsigned long long>(rec.fileSize));

                // An unreadable scene is flagged amber rather than left to read
                // like the others - it may still restore, but it is the one entry
                // worth hesitating over.
                const uint32_t subCol = (rec.objectCount < 0) ? PackColor(Palette::Amber) : 0u;

                if (ui.SelectableCard(rec.recoveryPath.c_str(), rec.displayName.c_str(),
                                      subtitle, m_Recovery.SelectedIndex() == i, subCol))
                {
                    m_Recovery.SetSelectedIndex(i);
                }

                // Double-click to look inside before committing. The card
                // reports what and how much; this answers whether it is the
                // work you were actually doing.
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    m_Recovery.SetSelectedIndex(i);
                    m_Recovery.BuildPreview(rec);
                    openPreview = true;
                }
            }
        }
        ui.EndChild();

        if (openPreview)
            ui.OpenPopup("Recovered contents");
        DrawRecoveryPreview();

        // Where it lands. Restoring rebinds to the original asset, so the next
        // Ctrl+S overwrites that file rather than the backup.
        ui.Spacing();
        if (!m_Recovery.Entries().empty())
        {
            const auto& sel = m_Recovery.Entries()[m_Recovery.SelectedIndex()];
            ui.PushStyleColor(EditorUI::Col::Text, PackColor(Palette::Dim));
            if (sel.originalPath.empty())
            {
                ui.TextWrapped("Restores as untitled \xe2\x80\x94 Save will ask where to put it.");
            }
            else
            {
                // Project-relative: the absolute path is mostly the project
                // directory repeated back, and at this dialog width it wrapped
                // across two lines and broke mid-word.
                //
                // Compared case- and separator-insensitively. A plain prefix
                // match silently never fired: the project path arrives as the
                // editor was launched with it ("c:/...") while the sidecar
                // recorded the scene's own ("C:\..."), so the two never
                // matched and every path stayed absolute.
                auto normalized = [](std::string s)
                {
                    for (char& c : s)
                    {
                        if (c == '\\') c = '/';
                        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    }
                    return s;
                };

                std::string dest = sel.originalPath;
                const std::string normRoot = normalized(m_Recovery.ScannedProjectPath());
                if (!normRoot.empty() && normalized(dest).rfind(normRoot, 0) == 0)
                {
                    dest.erase(0, normRoot.size());
                    while (!dest.empty() && (dest.front() == '/' || dest.front() == '\\'))
                        dest.erase(dest.begin());
                }
                ui.TextWrapped(("Restores over " + dest).c_str());
            }
            ui.PopStyleColor();
        }

        // Later only dismisses: the files stay on disk and are offered again
        // next time the project opens. Discard All asks first.
        switch (SchematicDialogButtons({ { "Discard All", DialogButton::Danger },
                                         { "Later" },
                                         { "Restore", DialogButton::Primary, !m_Recovery.Entries().empty() } }))
        {
            case 0:
                ui.OpenPopup("Discard recovered scenes?");
                break;
            case 1:
                m_Recovery.SetDialogRequested(false);
                ui.CloseCurrentPopup();
                break;
            case 2:
                RestoreSelectedRecovery();
                break;
            default:
                break;
        }

        DrawRecoveryDiscardConfirm();

        EndSchematicModal();
    }
}

void EditorApp::DrawRecoveryPreview()
{
    auto& ui = EditorUI::Get();

    if (BeginSchematicModal("Recovered contents", 440.0f))
    {
        ui.Text(m_Recovery.PreviewName().c_str());

        ui.Spacing();

        if (ui.BeginChild("##preview_tree", 0.0f, Dpi(240.0f), /*border*/ true))
        {
            if (m_Recovery.Preview().empty())
            {
                // Distinct from "no objects": the file could not be read at all,
                // which is worth saying plainly before someone restores it.
                ui.PushStyleColor(EditorUI::Col::Text, PackColor(Palette::Amber));
                ui.TextWrapped("Could not read this scene. Restoring may still work, "
                               "but there is nothing to preview.");
                ui.PopStyleColor();
            }
            else
            {
                // Plain indented text rather than tree nodes: this is a preview
                // of a file, not something to interact with, and a recovered
                // hierarchy is usually shallow enough to read at a glance.
                std::function<void(const std::vector<SceneOutlineNode>&, int)> draw =
                    [&](const std::vector<SceneOutlineNode>& nodes, int depth)
                {
                    for (const auto& node : nodes)
                    {
                        const std::string indent(static_cast<size_t>(depth) * 2, ' ');
                        ui.Text((indent + node.name).c_str());

                        // One component per line rather than a comma-joined
                        // run. Joined, an object with three or four of them
                        // overruns the popup: clipped it loses the tail
                        // entirely, and wrapped it splits mid-word
                        // ("AnimationCompon / ent"). A short line each stays
                        // inside the width whatever the class is called.
                        ui.PushStyleColor(EditorUI::Col::Text, PackColor(Palette::Dim));
                        for (const auto& c : node.components)
                            ui.Text((indent + "  " + c).c_str());
                        ui.PopStyleColor();

                        draw(node.children, depth + 1);
                    }
                };
                draw(m_Recovery.Preview(), 0);
            }
        }
        ui.EndChild();

        if (SchematicDialogButtons({ { "Close" } }) == 0)
            ui.CloseCurrentPopup();

        EndSchematicModal();
    }
}

bool EditorApp::WriteRecoverySnapshot()
{
    if (!m_Project)
        return false;

    std::string written;
    if (!m_Recovery.WriteSnapshot(m_CurrentScene.get(), m_CurrentScenePath,
                                  m_Project->GetPath(), &written))
        return false;

    // Reported here rather than inside EditorRecovery: the crash dialog lives in
    // the editor executable, and depending on it from a core unit would put SDL
    // and ImGui on the test binary's link line.
    CrashHandler::SetRecoveryMessage("Scene recovered to: " + written);
    return true;
}

void EditorApp::TickAutosave(float deltaSeconds)
{
    // Periodic recovery snapshot.
    //
    // The crash handler already writes one, but it only runs for faults the
    // handler catches. A hard kill, an OOM abort, a driver reset or power loss
    // skips it entirely and everything since the last Ctrl+S is gone. This
    // writes the same recovery file on a timer, so the existing recovery dialog
    // can offer it back with no further work.
    //
    // Not a save: it never touches m_CurrentScenePath and never marks the
    // history clean, so the user's own save remains the thing that publishes
    // their work.
    //
    // The eligibility verdict is computed here because EditorApp owns the state
    // it depends on; EditorRecovery owns only the clock.
    const bool ready = m_Project && m_CurrentScene
                       // Play mode edits a throwaway copy and the edit-mode history
                       // is stashed aside, so a snapshot there would capture state
                       // the user never authored.
                       && m_PlayModeState == PlayModeState::Stopped && CommandHistory::Instance().IsDirty();

    if (!m_Recovery.TickAutosaveTimer(deltaSeconds, ready))
        return;

    if (WriteRecoverySnapshot())
        DEKI_LOG_EDITOR("Autosave: wrote recovery snapshot for '%s'",
                        m_CurrentScenePath.empty() ? "<untitled>" : m_CurrentScenePath.c_str());
}

void EditorApp::RestoreSelectedRecovery()
{
    namespace fs = std::filesystem;
    auto& ui = EditorUI::Get();

    // Copy: the vector is edited below.
    const RecoveryEntry rec = m_Recovery.Entries()[m_Recovery.SelectedIndex()];

    // Deliberately not OpenScene(): that would bind m_CurrentScenePath to the
    // recovery file, so a subsequent Save would overwrite the backup instead of
    // the user's original asset. Load the recovered content and rebind to the
    // original path; the next Ctrl+S then writes to the right place.
    auto scene = std::make_unique<::Deki::Scene>();
    if (!scene->LoadFromFile(rec.recoveryPath))
    {
        DEKI_LOG_ERROR("Recovery: could not load '%s'; leaving it in place",
                       rec.recoveryPath.c_str());
        return;
    }

    CustomEditorRegistry::Instance().ClearCache();
    SetCurrentScene(std::move(scene));
    m_CurrentScenePath = rec.originalPath;  // empty if untitled - Save routes to Save-As
    ClearSelection();
    CommandHistory::Instance().Clear();
    // Recovered content differs from whatever is on disk - mark dirty so the
    // user is prompted to save before quit.
    CommandHistory::Instance().MarkDirty();

    // Consume only what was restored. This used to delete the whole Recovery
    // folder, so with two crashed scenes the second was destroyed without ever
    // being offered.
    // ConsumeEntry deletes this entry's files, drops it from the list, and
    // removes the directory only once nothing is left - so a second crashed
    // scene is still offered rather than destroyed alongside the first.
    m_Recovery.ConsumeEntry(m_Recovery.SelectedIndex());

    // Anything left is still unresolved, so keep offering it.
    if (!m_Recovery.HasPending())
        ui.CloseCurrentPopup();
}

void EditorApp::DrawRecoveryDiscardConfirm()
{
    namespace fs = std::filesystem;
    auto& ui = EditorUI::Get();

    if (BeginSchematicModal("Discard recovered scenes?", 380.0f))
    {
        // Deleting someone's only copy of unsaved work deserves a confirmation,
        // and a count so it is clear how much is going.
        char msg[192];
        snprintf(msg, sizeof(msg), "Permanently delete %zu recovered scene%s?",
                 m_Recovery.Entries().size(), m_Recovery.Entries().size() == 1 ? "" : "s");
        ui.Text(msg);

        ui.Spacing();
        ui.PushStyleColor(EditorUI::Col::Text, PackColor(Palette::Dim));
        ui.TextWrapped("This is the only copy of that work, and it cannot be undone.");
        ui.PopStyleColor();

        switch (SchematicDialogButtons({ { "Cancel" }, { "Delete", DialogButton::Danger } }))
        {
            case 0:
                ui.CloseCurrentPopup();
                break;
            case 1:
                m_Recovery.DiscardAll();
                ui.CloseCurrentPopup();
                break;
            default:
                break;
        }

        EndSchematicModal();
    }
}

void EditorApp::RegisterAssetTypes()
{
    // Register Scene as a creatable asset type
    AssetTypeHandler sceneHandler;
    sceneHandler.typeName = "Scene";
    sceneHandler.displayName = "Scene";
    sceneHandler.extensions = { ".scene" };
    // formatVersion: Deki::AuthoredFormat::SceneVersion (AuthoredFormat.h).
    sceneHandler.defaultContent = R"({
  "formatVersion": 1,
  "objects": [{"name": "Root", "children": []}]
})";
    sceneHandler.compilesTo = AssetCompileTarget::Data;
    sceneHandler.category = AssetCategory::Scene;
    AssetTypeRegistry::Instance().Register(sceneHandler);

    // Register .asset - the generic JSON envelope for procedural / custom asset types.
    // Packages that own a specific asset shape (e.g. ProceduralSprite) layer their own
    // AssetTypeEditor on top via REGISTER_EDITOR; this registration makes the bare
    // ".asset" extension resolve to the "Asset" type for AssetPipeline lookups.
    AssetTypeHandler assetHandler;
    assetHandler.typeName = "Asset";
    assetHandler.displayName = "Asset";
    assetHandler.extensions = { ".asset" };
    assetHandler.compilesTo = AssetCompileTarget::None;
    assetHandler.category = AssetCategory::Procedural;
    AssetTypeRegistry::Instance().Register(assetHandler);

    // Note: RenderPipeline is registered by deki-rendering package via RenderPipelineEditor
    // Note: FrameAnimation, Sprite, BitmapFont are registered by deki-2d package
}

void EditorApp::DrawMaintenanceConfirm()
{
    static bool s_Open = false;
    if (m_PendingMaintenance && !s_Open)
    {
        ImGui::OpenPopup("Confirm Maintenance");
        s_Open = true;
    }
    if (!s_Open)
        return;

    if (BeginSchematicModal("Confirm Maintenance", 480.0f))
    {
        const MaintenanceAction action = *m_PendingMaintenance;
        const MaintenanceInfo info = ProjectMaintenance::Describe(action);
        const std::string projectPath = m_Project ? m_Project->GetPath() : std::string{};

        ImGui::Text("%s", info.label);
        ImGui::Spacing();
        ImGui::TextWrapped("%s", info.cost);
        ImGui::Spacing();
        ImGui::TextColored(Palette::Dim, "This deletes:");
        for (const std::string& dir : ProjectMaintenance::Targets(action, projectPath))
            ImGui::BulletText("%s", dir.c_str());
        bool close = false;
        const int button = SchematicDialogButtons({ { "Cancel" }, { "Delete", DialogButton::Danger } });
        if (button == 0)
            close = true;
        if (button == 1)
        {
            const MaintenanceResult result = ProjectMaintenance::Run(action, projectPath);
            if (m_ConsolePanel)
            {
                if (result.ok)
                    m_ConsolePanel->LogEditor(result.message);
                else
                    m_ConsolePanel->LogError(result.message);
            }
            close = true;
        }

        if (close)
        {
            m_PendingMaintenance.reset();
            s_Open = false;
            ImGui::CloseCurrentPopup();
        }
        EndSchematicModal();
    }
    else
    {
        // Closed by other means (Escape, click outside): drop the request.
        m_PendingMaintenance.reset();
        s_Open = false;
    }
}

void EditorApp::DrawUnsavedChangesDialog()
{
    if (m_UnsavedPrompt.DialogVisible())
    {
        ImGui::OpenPopup("Unsaved Changes");
        m_UnsavedPrompt.SetDialogVisible(false);
    }

    if (BeginSchematicModal("Unsaved Changes", 420.0f))
    {
        // A programmatic answer behaves exactly like the matching button. It is
        // consumed here rather than where it was set because CloseCurrentPopup
        // is only valid inside the popup's draw scope.
        const DialogAnswer answer = m_UnsavedPrompt.TakeAnswer();

        if (answer == DialogAnswer::Save)
        {
            // No path yet means Save would need a file dialog, which a headless
            // caller cannot answer. Treat it as an error rather than silently
            // discarding: cancel, and leave the scene untouched.
            if (m_CurrentScenePath.empty())
            {
                DEKI_LOG_ERROR("AnswerUnsavedChangesDialog(Save): scene has no path - "
                               "save it with an explicit path first. Cancelling.");
                m_UnsavedPrompt.Cancel();
                m_NavigationSourcePath.clear();
                ImGui::CloseCurrentPopup();
                EndSchematicModal();
                return;
            }
            // Only discard the current scene once the save actually landed.
            // ExecutePendingAction replaces m_CurrentScene outright, so running
            // it after a failed write throws the work away - the exact outcome
            // the user chose "Save" to avoid.
            if (!SaveScene())
            {
                DEKI_LOG_ERROR("AnswerUnsavedChangesDialog(Save): write failed - "
                               "keeping the scene open and cancelling the pending action.");
                m_UnsavedPrompt.Cancel();
                m_NavigationSourcePath.clear();
                ImGui::CloseCurrentPopup();
                EndSchematicModal();
                return;
            }
            ImGui::CloseCurrentPopup();
            ExecutePendingAction();
            EndSchematicModal();
            return;
        }
        if (answer == DialogAnswer::Discard)
        {
            ImGui::CloseCurrentPopup();
            ExecutePendingAction();
            EndSchematicModal();
            return;
        }
        if (answer == DialogAnswer::Cancel)
        {
            m_UnsavedPrompt.Cancel();
            m_NavigationSourcePath.clear();
            ImGui::CloseCurrentPopup();
            EndSchematicModal();
            return;
        }

        // A Save As raised by this prompt finished on an earlier frame. Now
        // that we are inside the popup again, it can be closed for real.
        if (m_UnsavedPromptSaved)
        {
            m_UnsavedPromptSaved = false;
            ImGui::CloseCurrentPopup();
            ExecutePendingAction();
            EndSchematicModal();
            return;
        }

        ImGui::TextWrapped("Save the changes to the current scene?");
        ImGui::TextColored(Palette::Dim, "Not saved, they are lost.");

        // While the file dialog is up, the prompt stays on screen but inert:
        // a second click would stack another dialog behind the first.
        const bool ready = !FileDialogs::IsPending();
        const int button = SchematicDialogButtons({ { "Cancel", DialogButton::Normal, ready },
                                                    { "Don't Save", DialogButton::Danger, ready },
                                                    { "Save", DialogButton::Primary, ready } });
        if (button == 2)
        {
            // If no path, use Save As dialog
            if (m_CurrentScenePath.empty())
            {
                std::string defaultPath = m_Project ? m_Project->GetAssetsPath() : "";
                FileDialogs::SaveFile(
                    "Scene Files (*.scene)\0*.scene\0", "scene", defaultPath.c_str(),
                    [this](const std::string& path)
                    {
                        // A failed write must not fall through to
                        // ExecutePendingAction, which discards the scene. Saying
                        // nothing leaves the popup open so the user can pick
                        // another location.
                        if (!path.empty() && SaveSceneAs(path))
                            m_UnsavedPromptSaved = true;
                    });
            }
            else if (SaveScene())
            {
                ImGui::CloseCurrentPopup();
                ExecutePendingAction();
            }
        }
        else if (button == 1)
        {
            ImGui::CloseCurrentPopup();
            ExecutePendingAction();
        }
        else if (button == 0)
        {
            m_UnsavedPrompt.Cancel();
            m_NavigationSourcePath.clear();
            ImGui::CloseCurrentPopup();
        }

        EndSchematicModal();
    }
}

void EditorApp::RequestQuit()
{
    if (!CheckUnsavedChanges(PendingSceneAction::Quit))
        m_QuitRequested = true;
}

bool EditorApp::IsProtectedObject(Deki::Object* obj)
{
    if (!obj)
        return false;

    // The scene root can never be deleted.
    ::Deki::Scene* scene = GetCurrentScene();
    if (scene && obj->GetParent() == nullptr)
    {
        auto& roots = scene->GetObjects();
        if (!roots.empty() && roots[0] == obj)
            return true;
    }

    // "Setup" directly under the root of boot.scene carries the platform
    // setup component the runtime boots from.
    if (obj->GetName() != "Setup" || obj->GetParent() == nullptr || !scene)
        return false;
    if (std::filesystem::path(m_CurrentScenePath).filename().string() != "boot.scene")
        return false;
    auto& roots = scene->GetObjects();
    if (roots.empty())
        return false;
    for (auto* child : roots[0]->GetChildren())
        if (child == obj)
            return true;
    return false;
}

bool EditorApp::DeleteObject(Deki::Object* obj)
{
    ::Deki::Scene* scene = GetCurrentScene();
    if (!obj || !scene || IsProtectedObject(obj) || obj->IsPartOfInstance())
        return false;

    CommandHistory::Instance().Execute(
        std::make_unique<DeleteObjectCommand>(
            scene, obj,
            [this]()
            { SetSelectedObject(nullptr); },
            [this](Deki::Object* restored)
            { SetSelectedObject(restored); }));
    return true;
}

bool EditorApp::CheckUnsavedChanges(PendingSceneAction action, const std::string& path)
{
    // The prompt owns the suppression flag and the pending slot; EditorApp owns
    // the verdict, because it is what knows whether a scene is open and dirty.
    const bool dirty = m_CurrentScene && CommandHistory::Instance().IsDirty();
    return m_UnsavedPrompt.Request(action, path, dirty);
}

void EditorApp::ExecutePendingAction()
{
    // Delegate to the real NewScene/OpenScene rather than re-implementing
    // them.
    //
    // The duplicated versions that used to live here drifted: the new-scene
    // path never created the "Root" object NewScene creates, neither path
    // captured the inspector's saved-state baseline or ran EnsurePlatformSetup,
    // the open path never recorded lastScene, and a failed load was silent -
    // leaving the previous scene open after the user chose to discard it.
    //
    // m_SuppressUnsavedCheck stands in for "the dirty check already happened":
    // the user just answered the dialog, so asking again would loop.
    // TakePending clears the slot up front, so a re-entrant call (a failed open
    // that surfaces an error dialog) cannot see a stale pending action.
    std::string path;
    const PendingSceneAction action = m_UnsavedPrompt.TakePending(path);

    // A pending source path means NavigateIntoScene started this and was
    // interrupted by the dialog. OpenScene only pushes the breadcrumb while
    // m_IsNavigating is set, and NavigateIntoScene has long since reset it, so
    // restore it here or the trail is lost exactly when a dialog appeared.
    const bool resumingNavigation = !m_NavigationSourcePath.empty();

    m_UnsavedPrompt.SetSuppressed(true);
    m_IsNavigating = resumingNavigation;
    switch (action)
    {
        case PendingSceneAction::NewScene:
            NewScene();
            break;

        case PendingSceneAction::OpenScene:
            if (!OpenScene(path))
            {
                // The scene the user asked for could not be loaded, and their
                // current one has already been dealt with. Leaving them with a
                // broken half-state is worse than an empty scene.
                DEKI_LOG_ERROR("ExecutePendingAction: failed to open '%s'; creating a new scene",
                               path.c_str());
                NewScene();
            }
            break;

        case PendingSceneAction::CloseProject:
            // The unsaved scene has been saved or discarded by now, so the close
            // the dialog interrupted can go ahead. m_SuppressUnsavedCheck is still
            // set, so nothing re-asks.
            CloseProject();
            break;

        case PendingSceneAction::OpenProject:
            RequestOpenProject(path);
            break;

        case PendingSceneAction::Quit:
            m_QuitRequested = true;
            break;

        default:
            break;
    }
    m_IsNavigating = false;
    m_UnsavedPrompt.SetSuppressed(false);

    m_NavigationSourcePath.clear();
}

}  // namespace DekiEditor
