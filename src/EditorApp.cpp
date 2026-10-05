#include "EditorApp.h"
#include "EditorVersion.h"
#include "core/SceneStats.h"
#include "services/PackageManager.h"
#include "services/PackageLifetime.h"
#include <deki-editor/build/BuilderRegistry.h>
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
// Services
#include "services/FileDialogs.h"
#include "core/Commands.h"
#include "core/commands/HistoryGuards.h"
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

EditorApp::EditorApp()
{
    // EditorSelection owns the selection; EditorApplication is what package
    // DLLs read. Mirror one into the other here, once, rather than in each
    // function that changes the selection, where it is easy to forget.
    m_Selection.SetOnChanged([](Deki::Object* active) { EditorApplication::Get().SetActiveObject(active); });

    // Open Editor.log first, so logging from the rest of the constructor
    // (package builds, panel setup) reaches disk as well as the Console panel.
    EditorLogFile::Initialize(EditorSettings::GetSettingsDir() + "/Editor.log",
                              EditorSettings::GetBool("editorLog.verbose", false));

    // The console panel and log callback come first, so every startup error
    // (package builds included) reaches the Console panel.
    m_ConsolePanel = std::make_unique<ConsolePanel>(this);
    Deki::LogSystem::SetLogCallback(
        [](Deki::LogLevel level, const std::string& message, const char* file, int line)
        {
            EditorLogFile::OnLog(level, message, file, line);
            ConsolePanel::EngineLogCallback(level, message, file, line);
        });

    // Loads plugins when a project opens.
    m_PluginLoader = std::make_unique<PluginLoader>();

    // Packages load in OpenProject, so each project gets its own set.
    StartupProgress::Update("Building packages...");
    m_PackageLoader = std::make_unique<PackageLoader>();
    m_PackageLoader->onPackageUnloading = [](const std::string& package)
    {
        PackageLifetime::ReleaseAll();
        BuilderRegistry::Instance().RemovePackageBuilders(package);
        ::PackageManager::ForgetPackageInstallers(package);
    };
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
        // Record the engine-core components before any package loads.
        // ClearNonEngineComponents() uses this on hot reload to remove every
        // package and plugin entry without leaving stale pointers.
        Deki::ComponentRegistry::Instance().FreezeEngineComponents();
    }

    // Records play mode output.
    m_FrameCaptureService = std::make_unique<FrameCaptureService>();

    StartupProgress::Update("Creating panels...");
    m_HierarchyPanel = std::make_unique<HierarchyPanel>(this);
    m_InspectorPanel = std::make_unique<InspectorPanel>(this);
    m_SceneViewPanel = std::make_unique<SceneViewPanel>(this);
    m_AssetBrowserPanel = std::make_unique<AssetBrowserPanel>(this);
    m_ProjectSettingsPanel = std::make_unique<ProjectSettingsPanel>(this);
    m_EditorSettingsPanel = std::make_unique<EditorSettingsPanel>(this);

    // DiscoverTools() runs in OpenProject, not here: packages there may add
    // windows, and that path rebuilds the list anyway.
    m_ToolHost = std::make_unique<ToolHost>(this);
    m_ToolHost->onToolDiscovered = [](const std::string& name)
    {
        if (!StartupProgress::IsComplete())
        {
            StartupProgress::Update("Discovering tools...", name.c_str());
        }
    };

    // Plugin rebuilds and package reloads.
    StartupProgress::Update("Setting up hot reload...");
    m_HotReloadService =
        std::make_unique<HotReloadService>(m_PluginLoader.get(), m_PackageLoader.get(), m_ConsolePanel.get());

    // These keep component state across a reload.
    m_HotReloadService->SetReloadCallbacks([this]() { OnBeforePluginReload(); }, [this]() { OnAfterPluginReload(); });

    StartupProgress::Update("Registering platforms...");
    StartupProgress::Update("Loading board packs...");
    PlatformRegistry::Instance().LoadPlatformsFromBoardPacks();

    StartupProgress::Update("Creating panels...");
    m_BuildPanel = std::make_unique<BuildPanel>(this);

    m_NewProjectDialog = std::make_unique<NewProjectDialog>(this);
    m_NewProjectDialog->SetOnProjectCreated(
        [this](const std::string& projectPath)
        {
            // Open the new project, first asking about unsaved scene edits as
            // Open Project does. Cancel leaves the new project on disk, unopened.
            if (!CheckUnsavedChanges(PendingSceneAction::OpenProject, projectPath))
            {
                RequestOpenProject(projectPath);
            }
        });

    // The project manager screen shows until a project is open, which on a
    // normal launch includes the very first frame.
    m_ProjectManagerScreen = std::make_unique<ProjectManagerScreen>(this);
    m_ProjectManagerScreen->OnShown();

    m_PackageManagerPanel = std::make_unique<PackageManagerPanel>(this);

    m_PlatformManagerPanel = std::make_unique<PlatformManagerPanel>(this);

    m_ProfilerPanel = std::make_unique<ProfilerPanel>(this);

    m_FrameCapturePanel = std::make_unique<FrameCapturePanel>(this);

    m_SerialMonitorPanel = std::make_unique<SerialMonitorPanel>(this);

    m_SDCardDeployPanel = std::make_unique<SDCardDeployPanel>(this);

    m_DeployAnalysisPanel = std::make_unique<DeployAnalysisPanel>(this);

    m_ActivityPanel = std::make_unique<ActivityPanel>(this);

    // The build code reports progress through a sink rather than calling the
    // panel, so it can live in deki-editor.dll and be linked by a third-party
    // builder. The GUI's sink is the panel; a headless run sets its own, or
    // none.
    static struct PanelActivitySink : IBuildActivitySink
    {
        void Log(const std::string& process, const std::string& message) override
        {
            ActivityPanel::Log(process, message);
        }
        void Complete(const std::string& process) override { ActivityPanel::CompleteProcess(process); }
        void Fail(const std::string& process, const std::string& error) override
        {
            ActivityPanel::FailProcess(process, error);
        }
    } s_PanelSink;
    SetBuildActivitySink(&s_PanelSink);

    m_PlayViewPanel = std::make_unique<PlayViewPanel>(this);

    m_Panels = { m_HierarchyPanel.get(),    m_InspectorPanel.get(),       m_SceneViewPanel.get(),
                 m_AssetBrowserPanel.get(), m_ProjectSettingsPanel.get(), m_ConsolePanel.get(),
                 m_BuildPanel.get(),        m_PackageManagerPanel.get(),  m_PlatformManagerPanel.get(),
                 m_ProfilerPanel.get(),     m_FrameCapturePanel.get(),    m_SerialMonitorPanel.get(),
                 m_SDCardDeployPanel.get(), m_DeployAnalysisPanel.get(),  m_ActivityPanel.get(),
                 m_PlayViewPanel.get(),     m_EditorSettingsPanel.get() };

    // Which panels are open, from settings.
    StartupProgress::Update("Restoring layout...");
    for (IPanel* panel : m_Panels)
    {
        panel->isOpen = panel->PersistOpenState() ? EditorSettings::GetPanelOpen(panel->Id(), panel->OpenByDefault())
                                                  : panel->OpenByDefault();
    }

    // The window that had focus last session, for RestoreTabFocus().
    m_FocusRestoreWindow = EditorSettings::GetFocusedWindow();

    // On a crash: flush the log and save the current scene. The flush runs
    // even with no scene open, so the log holds everything up to the crash
    // before the crash dialog blocks the main thread.
    CrashHandler::SetPreCrashCallback(
        [this]()
        {
            EditorLogFile::Flush();
            WriteRecoverySnapshot();
        });
}

void EditorApp::SetHeadless(bool headless)
{
    m_Headless = headless;
    // The service exists before the boot knows whether this run has a window.
    if (m_HotReloadService)
    {
        m_HotReloadService->SetHeadless(headless);
    }
}

EditorApp::~EditorApp()
{
    CrashHandler::SetPreCrashCallback(nullptr);
    StopMcpServer();

    // Shut down here, while the editor's statics are alive. The registry is a
    // function-local static destroyed after main returns, when unloading a
    // builder plugin would run its detach against dead statics. This also
    // cancels any build still running in a plugin's code before that module
    // is freed, as PluginLoader::UnloadAllPlugins does.
    BuilderRegistry::Instance().Shutdown();
    SetBuildActivitySink(nullptr);

    // Remove the log callback before closing the log file, so a late log
    // does not touch a closed FILE*. The callback also refers to the
    // ConsolePanel, which dies with this EditorApp.
    Deki::LogSystem::SetLogCallback(nullptr);
    EditorLogFile::Shutdown();
}

void EditorApp::SaveLayout()
{
    for (IPanel* panel : m_Panels)
    {
        if (panel->PersistOpenState())
        {
            EditorSettings::SetPanelOpen(panel->Id(), panel->isOpen);
        }
    }

    // The focused window, so the next launch can select its tab again.
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (ctx && ctx->NavWindow)
    {
        EditorSettings::SetFocusedWindow(ctx->NavWindow->Name);
    }

    // imgui.ini holds the docking layout, selected tabs and window positions.
    ImGui::SaveIniSettingsToDisk(ImGui::GetIO().IniFilename);

    if (m_Project && m_Project->IsDirty())
    {
        m_Project->Save();
    }

    // One write for everything the setters above marked dirty.
    EditorSettings::FlushIfDirty();
}

void EditorApp::RestoreTabFocus()
{
    // Select last session's dock tab again. DockNodeUpdateTabBar() sets
    // SelectedTabId to NavWindow's tab every frame, so set NavWindow to the
    // target window and let it do the work.
    if (m_FocusRestoreWindow.empty())
    {
        return;
    }

    ImGuiWindow* win = ImGui::FindWindowByName(m_FocusRestoreWindow.c_str());
    if (!win || !win->DockId)
    {
        return;
    }

    ImGuiDockNode* node = ImGui::DockBuilderGetNode(win->DockId);
    if (!node || !node->TabBar || !win->TabId)
    {
        return;
    }

    node->SelectedTabId = win->TabId;
    node->TabBar->NextSelectedTabId = win->TabId;

    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (ctx)
    {
        ctx->NavWindow = win;
    }

    m_FocusRestoreWindow.clear();
}

void EditorApp::Update()
{
    DEKI_PROFILE_SCOPE_N("EditorApp::Update");

    // No project: the window belongs to the project manager screen, and the
    // work below has nothing to act on. Draw what the screen needs: its title
    // bar, itself, and the new-project wizard it opens.
    if (IsProjectManagerMode())
    {
        DrawProjectManagerMenuBar();

        DekiEditor::Guard("ProjectManagerScreen", [&] { m_ProjectManagerScreen->Draw(); });
        DekiEditor::Guard("NewProjectDialog", [&] { m_NewProjectDialog->Draw(); });

        // Opened here, not in the screen's draw: OpenProject loads package
        // DLLs and imports assets, which must not run inside a widget tree.
        const std::string requested = m_ProjectManagerScreen->ConsumePendingOpen();
        if (!requested.empty())
        {
            RequestOpenProject(requested);
        }

        // A refused open (the project is open in another editor) says so here.
        DekiEditor::Guard("Toast", [] { DekiEditor::Toast::DrawInFrame(); });

        return;
    }

    // HotReloadService starts and stops its file watchers when the project
    // changes, and drains source-watcher events into m_SourcesDirty.
    if (m_HotReloadService)
    {
        m_HotReloadService->Tick(m_Project ? m_Project->GetPath() : std::string{});
    }

    // Drain the asset file watcher every frame, so an asset edited in another
    // tool shows up without refocusing the window. With nothing pending this
    // is a mutex and an empty check.
    if (m_AssetPipeline)
    {
        m_AssetPipeline->Tick();
    }

    HandleKeyboardShortcuts();

    // Flush the editor log file twice a second (see EditorLogFile::OnLog).
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

    if (m_PlayModeState == PlayModeState::Playing)
    {
        UpdatePlayMode();
    }

    // Custom editors update every frame (ScrollComponent syncs its layout).
    if (m_PlayModeState != PlayModeState::Playing && m_SceneViewPanel)
    {
        m_SceneViewPanel->UpdateCustomEditors();
        m_SceneViewPanel->PlaceScreenAnchors();
    }

    DrawMenuBar();

    // Each panel is guarded, so a crash in one leaves the others drawing.
    for (IPanel* panel : m_Panels)
    {
        DekiEditor::Guard(panel->Id(), [&] { panel->Draw(); });
    }
    DekiEditor::Guard("NewProjectDialog",
                      [&]
                      {
                          if (m_NewProjectDialog)
                          {
                              m_NewProjectDialog->Draw();
                          }
                      });

    DrawUnsavedChangesDialog();
    DrawMaintenanceConfirm();
    if (m_HotReloadService)
    {
        m_HotReloadService->DrawBuildProgress();
    }
    DrawRecoveryDialog();

    // Toasts last: they stack above the build progress card in the
    // bottom-right corner, so that card must already be drawn.
    // The daily release check starts on the first frame of a windowed
    // session and shows a toast when its thread reports back.
    if (!m_Headless)
    {
        DekiEditor::Guard("UpdateCheck",
                          []
                          {
                              UpdateCheck::Start();
                              UpdateCheck::Poll();
                          });
    }

    DekiEditor::Guard("Toast", [] { DekiEditor::Toast::DrawInFrame(); });

    // Open tools, from the engine and plugins.
    m_ToolHost->Update(ImGui::GetIO().DeltaTime);

    // A package DLL's editor may ask for a tool window.
    {
        std::string toolFile, toolCache, toolTitle;
        if (EditorApplication::Get().PopOpenToolRequest(toolFile, toolCache, &toolTitle))
        {
            m_ToolHost->OpenToolForFile(toolFile.c_str(), toolCache.c_str(), toolTitle.c_str());
        }
    }

    // A package DLL's editor may switch the scene view tool (ClipComponent's
    // "Edit Rect" button picks the Rect tool).
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

    // A tool changed files and asked for them to show. Drain the watcher now
    // and rebuild the asset browser tree.
    if (EditorApplication::Get().PopAssetRefreshRequest())
    {
        if (m_AssetPipeline)
        {
            m_AssetPipeline->Tick();
        }
        m_AssetBrowserPanel->RefreshAssets();
    }

    // A tool asked to drop every loaded asset (after a font re-bake, say).
    if (EditorApplication::Get().PopAssetInvalidationRequest())
    {
        // Components' AssetRef pointers (TextComponent, SpriteComponent, ...).
        if (GetSceneViewPanel())
        {
            GetSceneViewPanel()->InvalidateAllAssets();
        }

        // The engine's fonts, loaded from now-stale .dfont files.
        Deki::AssetManager::Get()->ClearCache("BitmapFont");

        // The editor's GPU font atlases.
        EditorAssets::Get()->ClearFontCache();
    }

    if (m_ShowAboutWindow)
    {
        DrawAboutWindow();
    }

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
    {
        ImGui::OpenPopup("About Deki Editor");
    }
    if (BeginSchematicModal("About Deki Editor", 440.0f))
    {
        const std::string line = EditorVersionLine();
        ImGui::TextWrapped("%s", line.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, Palette::Dim);
        ImGui::TextWrapped("Compiler: %s", DEKI_EDITOR_CXX_COMPILER);
        const std::string logPath = EditorLogFile::GetLogPath();
        if (!logPath.empty())
        {
            ImGui::TextWrapped("Log: %s", logPath.c_str());
        }
        ImGui::PopStyleColor();
        // A bug report wants the exact line, not a retyped one.
        const int button = SchematicDialogButtons({ { "Copy Version" }, { "Close", DialogButton::Primary } });
        if (button == 0)
        {
            ImGui::SetClipboardText(line.c_str());
        }
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

    // No shortcuts while typing in a text field.
    if (io.WantTextInput)
    {
        return;
    }

    // Ctrl+Z: undo
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z))
    {
        if (io.KeyShift)
        {
            // Ctrl+Shift+Z: redo, like Ctrl+Y
            CommandHistory::Instance().Redo();
        }
        else
        {
            CommandHistory::Instance().Undo();
        }
    }

    // Ctrl+Y: redo
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y))
    {
        CommandHistory::Instance().Redo();
    }

    // File shortcuts: exactly the ones the File menu shows. Keep the two in step.
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S))
    {
        if (io.KeyShift)
        {
            SaveSceneAsDialog();
        }
        else if (m_CurrentScene && !m_CurrentScenePath.empty())
        {
            SaveScene();
        }
        else if (m_CurrentScene)
        {
            SaveSceneAsDialog();  // untitled: Ctrl+S needs a destination
        }
    }
    if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_O))
    {
        OpenProjectDialog();
    }
    if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_N))
    {
        ShowNewProjectDialog();
    }

    // Ctrl+Shift+P: stop
    if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_P) && !IsStopped())
    {
        Stop();
    }
    // Ctrl+P: play / pause
    else if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_P) && GetCurrentScene())
    {
        if (IsPlaying())
        {
            Pause();
        }
        else
        {
            Play();  // starts when stopped, resumes when paused
        }
    }
}

// The scene in use: during play, the play copy the runtime runs; when
// stopped, the edit scene.
Deki::Scene* EditorApp::GetCurrentScene()
{
    if (m_PlayModeState != PlayModeState::Stopped)
    {
        Deki::Scene* engineScene = Deki::Engine::GetInstance().GetRootScene();
        if (engineScene)
        {
            return engineScene;
        }
    }

    // The editor always has a scene to edit, so the rest of the editor never
    // checks for null. With none yet (no project open), set up an empty scene
    // exactly as New Scene does.
    if (!m_CurrentScene)
    {
        EnsureScene(m_CurrentScene);
        // EnsureScene assigned m_CurrentScene directly; pass it through
        // SetCurrentScene so the facade and callbacks follow.
        SetCurrentScene(std::move(m_CurrentScene));
    }
    return m_CurrentScene.get();
}

// Selection
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
    // The scene view keys its cached camera on the scene's address, and a new
    // scene can land at the old one's address, so the cache would serve the
    // old scene's freed camera.
    if (m_SceneViewPanel)
    {
        m_SceneViewPanel->InvalidateSceneCamera();
        m_SceneViewPanel->InvalidateFlatObjects();
    }
    // Panels let go of the old scene's objects (the Inspector's pin, the
    // Hierarchy's rename and drag state).
    if (m_InspectorPanel)
    {
        m_InspectorPanel->ClearSceneReferences();
    }
    if (m_HierarchyPanel)
    {
        m_HierarchyPanel->ClearSceneReferences();
    }
    // Publish before registering callbacks: both read m_CurrentScene, and the
    // facade must never name a scene that has been freed.
    EditorApplication::Get().SetActiveScene(m_CurrentScene.get());
    if (m_CurrentScene)
    {
        RegisterSceneCallbacks(m_CurrentScene.get());
    }
}

void EditorApp::RegisterSceneCallbacks(::Deki::Scene* scene)
{
    if (!scene)
    {
        return;
    }
    scene->SetOnObjectsAboutToBeDestroyed([this](const std::vector<Deki::Object*>& dying)
                                          { OnObjectsDestroyed(dying); });
}

void EditorApp::OnObjectsDestroyed(const std::vector<Deki::Object*>& dying)
{
    if (dying.empty())
    {
        return;
    }

    // The scene view's cached camera and its flattened object list may hold
    // dying objects; the Inspector's pin may too.
    if (m_SceneViewPanel)
    {
        m_SceneViewPanel->InvalidateSceneCamera();
        m_SceneViewPanel->InvalidateFlatObjects();
    }
    if (m_InspectorPanel)
    {
        m_InspectorPanel->OnObjectsDestroyed(dying);
    }

    // Drop undo/redo entries that point into the memory about to be freed.
    //
    // Commands hold raw pointers to their targets: objects, components, and
    // (through the EditorUI field helpers) addresses inside them like &obj->x.
    // A command left behind would write through a dangling pointer on its next
    // undo or redo.
    {
        // `dying` already lists the whole subtree; components die with their
        // object.
        DeadMemory dead;
        for (Deki::Object* obj : dying)
        {
            HistoryGuards::AddObject(dead, obj, /*recursive*/ false);
        }
        CommandHistory::Instance().DiscardCommandsReferencing(dead);
    }

    // EditorSelection's change callback updates the EditorApplication facade.
    m_Selection.RemoveObjects(dying);

    if (m_HierarchyPanel)
    {
        m_HierarchyPanel->OnObjectsDestroyed(dying);
    }
}

void EditorApp::SetSelectedAsset(const std::string& path, const std::string& cachePath, bool hasCached,
                                 const std::string& guid)
{
    m_Selection.SetSelectedAsset(path, cachePath, hasCached, guid);
    EditorApplication::Get().SetSelectedAssetPath(path);

    // Compile the asset if it has no cached version yet.
    if (!hasCached && m_AssetPipeline && !path.empty())
    {
        m_AssetPipeline->RefreshAsset(path);

        auto* info = m_AssetPipeline->GetAssetInfo(path);
        if (info)
        {
            // Through SetSelectedAsset, so all the selection's fields stay in step.
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
    // Check for package DLL changes, even with no project open.
    if (m_HotReloadService)
    {
        m_HotReloadService->RequestPackageCheck();
    }

    if (!m_Project)
    {
        return;
    }

    // The build check runs next frame, once the window is visible, so its
    // progress bar shows during compilation.
    if (m_HotReloadService)
    {
        m_HotReloadService->RequestBuildCheck(m_Project->GetPath());
    }

    // No asset rescan here: Update() drains the asset watcher every frame,
    // and new, changed and deleted assets reach the browser through the
    // pipeline callbacks.
}

void EditorApp::ReportIncompatiblePackages()
{
    if (!m_Project)
    {
        return;
    }

    // The installer checks this when a package is added, but cloning a project
    // with committed packages, or restoring them from the lock, puts packages
    // on disk without an install. Checking here reports a mismatch once, naming
    // the package and both versions, instead of as a later compile error with
    // nothing pointing at the cause.
    //
    // Not part of RunPackageIntegrityCheck: that one is about git state and
    // returns early without the hot-reload service, so headless runs would
    // skip it.
    //
    // Editable packages are checked too, since an older working tree does not
    // build either. Nothing here changes the disk.
    const std::string projectPath = m_Project->GetPath();
    PackageManager mgr(this);
    mgr.LoadPackages(projectPath);  // also refreshes installed status

    const std::filesystem::path packagesDir = std::filesystem::path(DekiEditor::GetPackagesDirectory(projectPath));

    for (const PackageEntry& pkg : mgr.GetPackages())
    {
        if (!pkg.isInstalled)
        {
            continue;
        }
        const std::string dirName = DekiPackageInstaller::GetRepoName(pkg);
        if (dirName.empty())
        {
            continue;
        }
        std::string reason;
        if (DekiPackageInstaller::EngineSatisfiesPackage(packagesDir / dirName, reason))
        {
            continue;
        }

        const std::string message = "'" + dirName + "': " + reason;
        DEKI_LOG_ERROR("Package compatibility: %s", message.c_str());
        if (!m_Headless)
        {
            DekiEditor::Toast::Push(DekiEditor::Toast::Level::Error, "Package is not compatible with this editor",
                                    message, 15.0f);
            if (m_ConsolePanel)
            {
                m_ConsolePanel->LogError(message);
            }
        }
    }
}

void EditorApp::RunPackageIntegrityCheck(bool sweepAll)
{
    if (!m_Project || !m_HotReloadService)
    {
        return;
    }

    // Always drain, even when sweeping: entries left behind would make the
    // next focus check packages this pass already handled.
    bool allSuspect = false;
    const std::vector<std::string> dirty = m_HotReloadService->TakeDirtyPackageDirs(allSuspect);
    const bool checkEverything = sweepAll || allSuspect;

    // Return before any file IO: this is what every alt-tab hits.
    if (!checkEverything && dirty.empty())
    {
        return;
    }

    // Its own manager, not the Package Manager panel's: the panel loads its
    // list when it first draws, so headless (or before the panel opens) the
    // list is empty and every package would be skipped.
    const std::string projectPath = m_Project->GetPath();
    PackageManager mgr(this);
    mgr.LoadPackages(projectPath);  // also refreshes installed status

    const std::filesystem::path packagesDir = std::filesystem::path(DekiEditor::GetPackagesDirectory(projectPath));

    for (const PackageEntry& pkg : mgr.GetPackages())
    {
        if (!pkg.isInstalled)
        {
            continue;
        }

        // The developer's own package: left alone entirely.
        if (pkg.editable)
        {
            continue;
        }

        const std::string dirName = DekiPackageInstaller::GetRepoName(pkg);
        if (dirName.empty())
        {
            continue;
        }

        if (!checkEverything && std::find(dirty.begin(), dirty.end(), dirName) == dirty.end())
        {
            continue;
        }

        // The lock is the committed pin; installedVersion comes from
        // .deki-package.json on disk, so it follows whatever is checked out.
        const std::string pinned = !pkg.lockedVersion.empty() ? pkg.lockedVersion : pkg.installedVersion;

        const DekiEditor::PackageIntegrityResult result =
            DekiEditor::RestorePackage(packagesDir / dirName, dirName, pinned);

        if (!result.NeedsReport())
        {
            continue;
        }

        if (result.action == DekiEditor::PackageIntegrityResult::Action::Failed)
        {
            DekiEditor::Toast::Push(DekiEditor::Toast::Level::Error, "Package restore failed", result.detail, 12.0f);
            if (m_ConsolePanel)
            {
                m_ConsolePanel->LogWarning(result.detail);
            }
            else
            {
                DEKI_LOG_ERROR("Package restore failed: %s", result.detail.c_str());
            }
            continue;
        }

        // One sentence for both: a single pass can stash edits and move HEAD
        // back, and the user needs to hear about both.
        std::string message = "'" + dirName + "' is not marked editable, so it was restored";
        if (!result.restoredRef.empty())
        {
            message += " to " + result.restoredRef;
        }
        if (!result.stashMessage.empty())
        {
            message += ". Your changes are safe in git stash (\"" + result.stashMessage + "\")";
        }
        else
        {
            message += ".";
        }

        DekiEditor::Toast::Push(DekiEditor::Toast::Level::Warning, "Package restored", message, 12.0f);

        // The toast gets seen; the Console line can be scrolled back to. Not
        // LogBuildError: pinned entries belong to the compiler and feed the
        // "Build failed" pill.
        if (m_ConsolePanel)
        {
            m_ConsolePanel->LogWarning(message);
        }

        // Headless has neither, and every CLI verb opens the project. This
        // pass must never undo someone's work silently, so it always logs.
        if (!m_ConsolePanel)
        {
            DEKI_LOG_WARNING("%s", message.c_str());
        }
    }
}

void EditorApp::ProcessDeferredTasks()
{
    // Give a finished file dialog its answer first, outside the ImGui frame:
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

    // Advance a running build first: a finished step may need the reload,
    // which has to run here, outside the ImGui frame, like the checks below.
    if (m_HotReloadService)
    {
        m_HotReloadService->PumpBuild();
    }

    // Restore non-editable packages before the reload check below: a restore
    // rewrites files, and that check compares their modification times.
    // Run second, it would rebuild from the edits just reverted.
    RunPackageIntegrityCheck(false);

    // Reloads only the packages that changed.
    if (m_HotReloadService && m_HotReloadService->HasPendingPackageCheck())
    {
        m_HotReloadService->ProcessPendingPackageCheck();
    }

    // Runs once the window is fully visible.
    if (m_HotReloadService && m_HotReloadService->HasPendingBuild())
    {
        m_HotReloadService->ProcessPendingBuild();
    }

    if (m_McpServer)
    {
        m_McpServer->ProcessMessages();
    }
}

bool EditorApp::StartMcpServer(int port)
{
    if (m_McpServer)
    {
        return true;
    }
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
    {
        m_PackageLoader->ReleaseImGuiContext();
    }
    Deki::SetImGuiContext(nullptr);
}

void EditorApp::PublishLiveSession()
{
    if (m_McpServer && m_McpServer->IsRunning())
    {
        LiveSession::Publish(m_McpServer->GetPort(), m_McpServer->GetToken(),
                             m_Project ? m_Project->GetPath() : std::string());
    }
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

/// Packs a theme colour into EditorUI's RGBA word for PushStyleColor, like
/// PackageManagerPanel's Pack.
inline uint32_t PackColor(const ImVec4& c)
{
    return EditorUI::Rgba(static_cast<uint8_t>(c.x * 255.0f), static_cast<uint8_t>(c.y * 255.0f),
                          static_cast<uint8_t>(c.z * 255.0f), static_cast<uint8_t>(c.w * 255.0f));
}

/// "2026-08-16 14:32  (2 hours ago)". The date matches what the user
/// remembers doing; the age tells them whether it is worth restoring.
std::string FormatRecoveryTime(std::time_t when)
{
    if (when <= 0)
    {
        return "unknown time";
    }

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

    // Singular for 1: "1 day ago", not "1 days ago".
    auto ago = [](long long n, const char* unit)
    { return std::to_string(n) + " " + unit + (n == 1 ? "" : "s") + " ago"; };

    std::string rel;
    if (secs < 0)
    {
        rel = "in the future";  // clock changed; say so rather than print nonsense
    }
    else if (secs < 90)
    {
        rel = "just now";
    }
    else if (secs < 3600)
    {
        rel = ago(secs / 60, "minute");
    }
    else if (secs < 86400)
    {
        rel = ago(secs / 3600, "hour");
    }
    else
    {
        rel = ago(secs / 86400, "day");
    }

    return std::string(stamp) + "  (" + rel + ")";
}

}  // namespace

void EditorApp::DrawRecoveryDialog()
{
    if (!m_Recovery.DialogRequested())
    {
        return;
    }

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
        {
            m_Recovery.SetSelectedIndex(0);
        }

        // SelectableCard is the list row used elsewhere (package list, platform
        // list): title, dim subtitle, and an accent bar on the selected one.
        //
        // The height follows DrawSelectableCard's layout (padY*2 + title + gap +
        // subtitle); a guessed constant clips the last card. Using the title
        // height for the subtitle rounds up slightly, the safe direction.
        const float cardDpi = ImGui::GetStyle().FontScaleDpi;
        const float cardH = 6.0f * cardDpi * 2.0f + ImGui::GetTextLineHeight() + 2.0f * cardDpi +
                            ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y;
        const float shown = ImMin(static_cast<float>(m_Recovery.Entries().size()), 4.0f);
        const float listH = cardH * shown + ImGui::GetStyle().WindowPadding.y;
        if (ui.BeginChild("##recovery_list", 0.0f, listH, /*border*/ true))
        {
            for (int i = 0; i < static_cast<int>(m_Recovery.Entries().size()); ++i)
            {
                const auto& rec = m_Recovery.Entries()[i];

                // A card takes one subtitle line: when it was saved and what is
                // in it, which together decide whether it is worth keeping.
                char subtitle[192];
                if (rec.objectCount < 0)
                {
                    snprintf(subtitle, sizeof(subtitle), "%s  \xc2\xb7  unreadable  \xc2\xb7  %llu bytes",
                             FormatRecoveryTime(rec.savedAt).c_str(), static_cast<unsigned long long>(rec.fileSize));
                }
                else
                {
                    snprintf(subtitle, sizeof(subtitle), "%s  \xc2\xb7  %d object%s  \xc2\xb7  %llu bytes",
                             FormatRecoveryTime(rec.savedAt).c_str(), rec.objectCount, rec.objectCount == 1 ? "" : "s",
                             static_cast<unsigned long long>(rec.fileSize));
                }

                // An unreadable scene shows in amber: it may still restore, but
                // it is the one entry worth hesitating over.
                const uint32_t subCol = (rec.objectCount < 0) ? PackColor(Palette::Amber) : 0u;

                if (ui.SelectableCard(rec.recoveryPath.c_str(), rec.displayName.c_str(), subtitle,
                                      m_Recovery.SelectedIndex() == i, subCol))
                {
                    m_Recovery.SetSelectedIndex(i);
                }

                // Double-click to look inside before restoring, to check it is
                // the work you were doing.
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
        {
            ui.OpenPopup("Recovered contents");
        }
        DrawRecoveryPreview();

        // Where it lands. A restored scene saves to the original asset, so the
        // next Ctrl+S overwrites that file rather than the backup.
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
                // Project-relative: the absolute path mostly repeats the
                // project folder and wraps mid-word at this dialog width.
                //
                // Compared ignoring case and separators: the project path comes
                // as the editor was launched ("c:/...") while the sidecar
                // records the scene's own ("C:\...").
                auto normalized = [](std::string s)
                {
                    for (char& c : s)
                    {
                        if (c == '\\')
                        {
                            c = '/';
                        }
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
                    {
                        dest.erase(dest.begin());
                    }
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
            case 0: ui.OpenPopup("Discard recovered scenes?"); break;
            case 1:
                m_Recovery.SetDialogRequested(false);
                ui.CloseCurrentPopup();
                break;
            case 2: RestoreSelectedRecovery(); break;
            default: break;
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
                // Not "no objects": the file could not be read at all, which is
                // worth saying plainly before someone restores it.
                ui.PushStyleColor(EditorUI::Col::Text, PackColor(Palette::Amber));
                ui.TextWrapped("Could not read this scene. Restoring may still work, "
                               "but there is nothing to preview.");
                ui.PopStyleColor();
            }
            else
            {
                // Plain indented text, not tree nodes: a preview is for reading,
                // and a recovered hierarchy is usually shallow.
                std::function<void(const std::vector<SceneOutlineNode>&, int)> draw =
                    [&](const std::vector<SceneOutlineNode>& nodes, int depth)
                {
                    for (const auto& node : nodes)
                    {
                        const std::string indent(static_cast<size_t>(depth) * 2, ' ');
                        ui.Text((indent + node.name).c_str());

                        // One component per line: joined with commas, three or
                        // four overrun the popup and get clipped or split
                        // mid-word.
                        ui.PushStyleColor(EditorUI::Col::Text, PackColor(Palette::Dim));
                        for (const auto& c : node.components)
                        {
                            ui.Text((indent + "  " + c).c_str());
                        }
                        ui.PopStyleColor();

                        draw(node.children, depth + 1);
                    }
                };
                draw(m_Recovery.Preview(), 0);
            }
        }
        ui.EndChild();

        if (SchematicDialogButtons({ { "Close" } }) == 0)
        {
            ui.CloseCurrentPopup();
        }

        EndSchematicModal();
    }
}

bool EditorApp::WriteRecoverySnapshot()
{
    if (!m_Project)
    {
        return false;
    }

    std::string written;
    if (!m_Recovery.WriteSnapshot(m_CurrentScene.get(), m_CurrentScenePath, m_Project->GetPath(), &written))
    {
        return false;
    }

    // Reported here, not in EditorRecovery: the crash dialog lives in the
    // editor executable, and calling it from a core unit would make the test
    // binary link SDL and ImGui.
    CrashHandler::SetRecoveryMessage("Scene recovered to: " + written);
    return true;
}

void EditorApp::TickAutosave(float deltaSeconds)
{
    // Recovery snapshot on a timer.
    //
    // The crash handler writes one too, but only for faults it catches. A
    // hard kill, an out-of-memory abort, a driver reset or power loss skips
    // it, losing everything since the last Ctrl+S. This writes the same
    // recovery file on a timer, so the recovery dialog can offer it back.
    //
    // Not a save: it never touches m_CurrentScenePath or marks the history
    // clean, so only the user's own save writes their work.
    //
    // Whether a snapshot is due is decided here because EditorApp owns that
    // state; EditorRecovery owns only the clock.
    const bool ready = m_Project &&
                       m_CurrentScene
                       // Play mode edits a throwaway copy with the edit history
                       // set aside, so a snapshot there would hold state the
                       // user never made.
                       && m_PlayModeState == PlayModeState::Stopped && CommandHistory::Instance().IsDirty();

    if (!m_Recovery.TickAutosaveTimer(deltaSeconds, ready))
    {
        return;
    }

    if (WriteRecoverySnapshot())
    {
        DEKI_LOG_EDITOR("Autosave: wrote recovery snapshot for '%s'",
                        m_CurrentScenePath.empty() ? "<untitled>" : m_CurrentScenePath.c_str());
    }
}

void EditorApp::RestoreSelectedRecovery()
{
    namespace fs = std::filesystem;
    auto& ui = EditorUI::Get();

    // Copy: the vector is edited below.
    const RecoveryEntry rec = m_Recovery.Entries()[m_Recovery.SelectedIndex()];

    // Not OpenScene(): that would set m_CurrentScenePath to the recovery file,
    // so Save would overwrite the backup instead of the original asset. Load
    // the recovered content and point at the original path, so the next Ctrl+S
    // writes to the right place.
    auto scene = std::make_unique<::Deki::Scene>();
    if (!scene->LoadFromFile(rec.recoveryPath))
    {
        DEKI_LOG_ERROR("Recovery: could not load '%s'; leaving it in place", rec.recoveryPath.c_str());
        return;
    }

    CustomEditorRegistry::Instance().ClearCache();
    SetCurrentScene(std::move(scene));
    m_CurrentScenePath = rec.originalPath;  // empty if untitled: Save goes to Save As
    ClearSelection();
    CommandHistory::Instance().Clear();
    // The recovered content differs from what is on disk: mark it dirty so
    // the user is asked to save before quitting.
    CommandHistory::Instance().MarkDirty();

    // Remove only what was restored. ConsumeEntry deletes this entry's files,
    // drops it from the list, and removes the folder only once nothing is
    // left, so a second crashed scene is still offered.
    m_Recovery.ConsumeEntry(m_Recovery.SelectedIndex());

    // Anything left is still unresolved, so keep offering it.
    if (!m_Recovery.HasPending())
    {
        ui.CloseCurrentPopup();
    }
}

void EditorApp::DrawRecoveryDiscardConfirm()
{
    namespace fs = std::filesystem;
    auto& ui = EditorUI::Get();

    if (BeginSchematicModal("Discard recovered scenes?", 380.0f))
    {
        // This deletes the only copy of unsaved work: confirm, with a count.
        char msg[192];
        snprintf(msg, sizeof(msg), "Permanently delete %zu recovered scene%s?", m_Recovery.Entries().size(),
                 m_Recovery.Entries().size() == 1 ? "" : "s");
        ui.Text(msg);

        ui.Spacing();
        ui.PushStyleColor(EditorUI::Col::Text, PackColor(Palette::Dim));
        ui.TextWrapped("This is the only copy of that work, and it cannot be undone.");
        ui.PopStyleColor();

        switch (SchematicDialogButtons({ { "Cancel" }, { "Delete", DialogButton::Danger } }))
        {
            case 0: ui.CloseCurrentPopup(); break;
            case 1:
                m_Recovery.DiscardAll();
                ui.CloseCurrentPopup();
                break;
            default: break;
        }

        EndSchematicModal();
    }
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
    {
        return;
    }

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
        {
            ImGui::BulletText("%s", dir.c_str());
        }
        bool close = false;
        const int button = SchematicDialogButtons({ { "Cancel" }, { "Delete", DialogButton::Danger } });
        if (button == 0)
        {
            close = true;
        }
        if (button == 1)
        {
            const MaintenanceResult result = ProjectMaintenance::Run(action, projectPath);
            if (m_ConsolePanel)
            {
                if (result.ok)
                {
                    m_ConsolePanel->LogEditor(result.message);
                }
                else
                {
                    m_ConsolePanel->LogError(result.message);
                }
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

    // Open before and closed after: Escape dismissed it. That is a Cancel, so
    // the pending action (a quit, a scene switch) must not stay waiting.
    const bool wasOpen = ImGui::IsPopupOpen("Unsaved Changes");
    const bool open = BeginSchematicModal("Unsaved Changes", 420.0f);
    if (!open && wasOpen)
    {
        m_UnsavedPrompt.Cancel();
        m_NavigationSourcePath.clear();
    }
    if (open)
    {
        // An answer given from code acts exactly like the matching button. It
        // is taken here, not where it was set, because CloseCurrentPopup only
        // works inside the popup's draw scope.
        const DialogAnswer answer = m_UnsavedPrompt.TakeAnswer();

        if (answer == DialogAnswer::Save)
        {
            // With no path, Save needs a file dialog, which a headless caller
            // cannot answer. Treat it as an error, not a discard: cancel and
            // leave the scene untouched.
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
            // Move on only once the save succeeded. ExecutePendingAction
            // replaces m_CurrentScene, so after a failed write it would throw
            // away the work the user chose "Save" to keep.
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

        // A Save As started by this prompt finished on an earlier frame. Back
        // inside the popup, it can now close.
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

        // While the file dialog is up, the prompt stays on screen but disabled:
        // a second click would stack another dialog behind the first.
        const bool ready = !FileDialogs::IsPending();
        const int button = SchematicDialogButtons({ { "Cancel", DialogButton::Normal, ready },
                                                    { "Don't Save", DialogButton::Danger, ready },
                                                    { "Save", DialogButton::Primary, ready } });
        if (button == 2)
        {
            // No path yet: ask for one.
            if (m_CurrentScenePath.empty())
            {
                std::string defaultPath = m_Project ? m_Project->GetAssetsPath() : "";
                FileDialogs::SaveFile("Scene Files (*.scene)\0*.scene\0", "scene", defaultPath.c_str(),
                                      [this](const std::string& path)
                                      {
                                          // A failed write must not reach
                                          // ExecutePendingAction, which discards the scene. The
                                          // popup stays open so the user can pick another place.
                                          if (!path.empty() && SaveSceneAs(path))
                                          {
                                              m_UnsavedPromptSaved = true;
                                          }
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
    {
        m_QuitRequested = true;
    }
}

bool EditorApp::IsProtectedObject(Deki::Object* obj)
{
    if (!obj)
    {
        return false;
    }

    // The scene root can never be deleted.
    ::Deki::Scene* scene = GetCurrentScene();
    if (scene && obj->GetParent() == nullptr)
    {
        auto& roots = scene->GetObjects();
        if (!roots.empty() && roots[0] == obj)
        {
            return true;
        }
    }

    // "Setup" directly under the root of boot.scene carries the platform
    // setup component the runtime boots from.
    if (obj->GetName() != "Setup" || obj->GetParent() == nullptr || !scene)
    {
        return false;
    }
    if (std::filesystem::path(m_CurrentScenePath).filename().string() != "boot.scene")
    {
        return false;
    }
    auto& roots = scene->GetObjects();
    if (roots.empty())
    {
        return false;
    }
    for (auto* child : roots[0]->GetChildren())
    {
        if (child == obj)
        {
            return true;
        }
    }
    return false;
}

bool EditorApp::DeleteObject(Deki::Object* obj)
{
    ::Deki::Scene* scene = GetCurrentScene();
    if (!obj || !scene || IsProtectedObject(obj) || obj->IsPartOfInstance())
    {
        return false;
    }

    CommandHistory::Instance().Execute(std::make_unique<DeleteObjectCommand>(
        scene, obj, [this]() { SetSelectedObject(nullptr); },
        [this](Deki::Object* restored) { SetSelectedObject(restored); }));
    return true;
}

namespace
{
// `objects` without any whose ancestor is also in it: acting on a parent
// already covers its children, and acting on both would free a child twice.
std::vector<Deki::Object*> TopLevelOnly(const std::vector<Deki::Object*>& objects)
{
    std::vector<Deki::Object*> out;
    for (Deki::Object* obj : objects)
    {
        if (!obj)
        {
            continue;
        }
        bool coveredByAncestor = false;
        for (Deki::Object* p = obj->GetParent(); p && !coveredByAncestor; p = p->GetParent())
        {
            coveredByAncestor = std::find(objects.begin(), objects.end(), p) != objects.end();
        }
        if (!coveredByAncestor && std::find(out.begin(), out.end(), obj) == out.end())
        {
            out.push_back(obj);
        }
    }
    return out;
}
}  // namespace

std::vector<Deki::Object*> EditorApp::ActionTargets(Deki::Object* clicked) const
{
    const auto& selection = GetSelectedObjects();
    if (clicked && selection.size() > 1 && std::find(selection.begin(), selection.end(), clicked) != selection.end())
    {
        return selection;
    }
    return clicked ? std::vector<Deki::Object*>{ clicked } : std::vector<Deki::Object*>{};
}

bool EditorApp::DeleteObjects(const std::vector<Deki::Object*>& objects)
{
    ::Deki::Scene* scene = GetCurrentScene();
    if (!scene)
    {
        return false;
    }
    std::vector<Deki::Object*> targets;
    for (Deki::Object* obj : TopLevelOnly(objects))
    {
        if (!IsProtectedObject(obj) && !obj->IsPartOfInstance())
        {
            targets.push_back(obj);
        }
    }
    if (targets.empty())
    {
        return false;
    }
    if (targets.size() == 1)
    {
        return DeleteObject(targets[0]);
    }

    // Each delete records its index, and undo puts them back in reverse
    // order. Deleting from the last sibling up makes undo restore first to
    // last, so every index is right when its object returns; in selection
    // order, undo could reorder the siblings.
    auto indexOf = [scene](Deki::Object* obj)
    {
        Deki::Object* parent = obj->GetParent();
        return parent ? parent->GetChildIndex(obj) : scene->GetObjectIndex(obj);
    };
    std::stable_sort(targets.begin(), targets.end(),
                     [&](Deki::Object* a, Deki::Object* b) { return indexOf(a) > indexOf(b); });

    std::vector<std::unique_ptr<Command>> commands;
    for (Deki::Object* obj : targets)
    {
        commands.push_back(std::make_unique<DeleteObjectCommand>(
            scene, obj, [this]() { ClearSelection(); }, [this](Deki::Object* restored) { AddToSelection(restored); }));
    }
    ClearSelection();
    CommandHistory::Instance().Execute(std::make_unique<CompoundCommand>(
        "Delete " + std::to_string(targets.size()) + " objects", std::move(commands)));
    return true;
}

void EditorApp::DuplicateObjects(const std::vector<Deki::Object*>& objects)
{
    ::Deki::Scene* scene = GetCurrentScene();
    if (!scene)
    {
        return;
    }
    const std::vector<Deki::Object*> sources = TopLevelOnly(objects);
    if (sources.empty())
    {
        return;
    }

    std::vector<std::unique_ptr<Command>> commands;
    for (Deki::Object* obj : sources)
    {
        Deki::Object* parent = obj->GetParent();
        auto onCreated = [this](Deki::Object* created) { AddToSelection(created); };
        auto onUndo = [this]() { ClearSelection(); };
        if (obj->IsSceneInstance() && IsStopped())
        {
            // Edit mode: a linked instance duplicates into another linked instance.
            commands.push_back(std::make_unique<CreateInstanceCommand>(scene, obj->GetSourceSceneGuid(), obj->GetName(),
                                                                       parent, onCreated, onUndo));
        }
        else
        {
            // A plain copy (in play mode, with its runtime state).
            commands.push_back(std::make_unique<DuplicateObjectCommand>(scene, obj, parent, onCreated, onUndo));
        }
    }
    ClearSelection();
    if (commands.size() == 1)
    {
        CommandHistory::Instance().Execute(std::move(commands[0]));
    }
    else
    {
        CommandHistory::Instance().Execute(std::make_unique<CompoundCommand>(
            "Duplicate " + std::to_string(sources.size()) + " objects", std::move(commands)));
    }
}

bool EditorApp::CheckUnsavedChanges(PendingSceneAction action, const std::string& path)
{
    // The prompt owns the suppression flag and the pending action; EditorApp
    // decides whether to ask, since it knows whether a scene is open and dirty.
    const bool dirty = m_CurrentScene && CommandHistory::Instance().IsDirty();
    return m_UnsavedPrompt.Request(action, path, dirty);
}

void EditorApp::ExecutePendingAction()
{
    // Call the real NewScene/OpenScene; never copy them here, since a copy
    // drifts from what they do (Root object, inspector baseline, platform
    // setup, lastScene, load errors).
    //
    // Suppressing the unsaved check means "the dirty check already happened":
    // the user just answered the dialog, so asking again would loop.
    // TakePending clears the pending action first, so a re-entrant call (a
    // failed open that shows an error dialog) cannot see a stale one.
    std::string path;
    const PendingSceneAction action = m_UnsavedPrompt.TakePending(path);

    // A pending source path means NavigateIntoScene started this and the
    // dialog interrupted it. OpenScene pushes the breadcrumb only while
    // m_IsNavigating is set, which NavigateIntoScene has already reset, so set
    // it again here or the trail is lost whenever a dialog appeared.
    const bool resumingNavigation = !m_NavigationSourcePath.empty();

    m_UnsavedPrompt.SetSuppressed(true);
    m_IsNavigating = resumingNavigation;
    switch (action)
    {
        case PendingSceneAction::NewScene: NewScene(); break;

        case PendingSceneAction::OpenScene:
            if (!OpenScene(path))
            {
                // The requested scene did not load and the current one is
                // already saved or discarded. An empty scene beats a broken
                // half-state.
                DEKI_LOG_ERROR("ExecutePendingAction: failed to open '%s'; creating a new scene", path.c_str());
                NewScene();
            }
            break;

        case PendingSceneAction::CloseProject:
            // The unsaved scene is saved or discarded by now, so the close the
            // dialog interrupted can go ahead. The check is still suppressed,
            // so nothing asks again.
            CloseProject();
            break;

        case PendingSceneAction::OpenProject: RequestOpenProject(path); break;

        case PendingSceneAction::Quit: m_QuitRequested = true; break;

        default: break;
    }
    m_IsNavigating = false;
    m_UnsavedPrompt.SetSuppressed(false);

    m_NavigationSourcePath.clear();
}

}  // namespace DekiEditor
