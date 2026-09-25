// Play Mode Implementation for Deki Editor
// Uses Deki::Engine and DekiDisplay for rendering (same as simulator)
//
// IMPORTANT: This file is COMPONENT-AGNOSTIC. It uses Deki::Component lifecycle
// methods (LoadAssets, UnloadAssets, RuntimeUpdate) and DekiInputSystem instead of
// hardcoded GetComponent<SpriteComponent> etc. calls.

#include <deki-editor/SceneView.h>
#include "EditorApp.h"
#include <deki-editor/build/ProjectPaths.h>
#include <deki-editor/CommandHistory.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <deki-editor/Paths.h>
#include "services/CrashHandler.h"
#include "services/PlayModeGuard.h"
#include "services/ProfilingService.h"
#include "panels/ConsolePanel.h"
#include <SDL3/SDL.h>
#include <filesystem>
#include <fstream>
#include <unordered_set>

#include <deki/Engine.h>
#include <deki/SetupComponent.h>  // play-phase auto setups
#include <deki/Component.h>
#include <deki/providers/IInputSystem.h>
#include <deki/ICamera.h>
#include <deki/providers/IRenderSystem.h>
#include <deki/SceneSystem.h>
#include <deki/providers/IDisplay.h>
#include "platforms/editor/EditorDisplay.h"

// PlayMode owns the EditorDisplay lifetime; engine-core just holds the
// pointer. File-scope unique_ptr keeps it alive between Start/Stop cycles.
static std::unique_ptr<Deki::EditorDisplay> s_EditorDisplay;
#include <deki-editor/AssetPipeline.h>
#include <deki/assets/AssetManager.h>
#include <deki-editor/TextureImporter.h>
#include <deki/LogSystem.h>
#include <deki/reflection/ComponentRegistry.h>
#include <deki/Time.h>
#include "LifecycleManager.h"
#include <deki/Guid.h>
#include "services/PackageLoader.h"
#include "services/AssetExporter.h"
#include <deki/assets/AssetLookupTable.h>

namespace fs = std::filesystem;

namespace DekiEditor
{

namespace
{

Deki::ICamera* FindFirstCameraRecursive(Deki::Object* obj)
{
    if (!obj) return nullptr;
    if (Deki::ICamera* c = obj->FindInterface<Deki::ICamera>())
        return c;
    for (Deki::Object* child : obj->GetChildren())
    {
        if (Deki::ICamera* c = FindFirstCameraRecursive(child))
            return c;
    }
    return nullptr;
}

Deki::ICamera* FindFirstCameraInScene(Deki::Scene* scene)
{
    if (!scene) return nullptr;
    for (Deki::Object* obj : scene->GetObjects())
    {
        if (Deki::ICamera* c = FindFirstCameraRecursive(obj))
            return c;
    }
    return nullptr;
}

}  // namespace

void EditorApp::Play()
{
    if (!m_CurrentScene)
        return;

    if (m_PlayModeState == PlayModeState::Paused)
    {
        m_PlayModeState = PlayModeState::Playing;
        return;
    }

    if (m_PlayModeState == PlayModeState::Playing)
        return;

    // Block play mode if there are compilation errors
    if ((m_HotReloadService && m_HotReloadService->HasBuildErrors()) ||
        (m_PackageLoader && m_PackageLoader->HasBuildErrors()))
    {
        DEKI_LOG_ERROR("Cannot enter play mode - fix compilation errors first");
        if (m_ConsolePanel)
        {
            m_ConsolePanel->m_Open = true;
            m_ConsolePanel->m_FocusFrames = 10;
        }
        return;
    }

    InitPlayMode();

    // Validate critical systems before entering play mode
    if (!m_Play.renderSystem || m_Play.textureId == 0 || !Deki::Engine::GetInstance().GetRootScene())
    {
        DEKI_LOG_ERROR("Play mode initialization failed - aborting");
        ShutdownPlayMode();
        if (m_ConsolePanel)
        {
            m_ConsolePanel->m_Open = true;
            m_ConsolePanel->m_FocusFrames = 10;
        }
        return;
    }

    m_PlayModeState = PlayModeState::Playing;
    m_Play.camera = FindFirstCameraInScene(Deki::Engine::GetInstance().GetRootScene());

    // Auto-open and focus Play View panel
    if (m_PlayViewPanel)
    {
        m_PlayViewPanel->m_Open = true;

        // Save the currently selected tab in Play View's dock node so we can restore on Stop
        ImGuiWindow* playWin = ImGui::FindWindowByName("Play View");
        if (playWin && playWin->DockId)
        {
            ImGuiDockNode* node = ImGui::DockBuilderGetNode(playWin->DockId);
            if (node && node->TabBar)
                m_Play.prePlaySelectedTabId = node->TabBar->SelectedTabId;
        }

        ImGui::SetWindowFocus("PLAY VIEW###Play View");
    }

    // Reset frame timing accumulator
    m_Play.timeAccumulator = 0.0f;
    m_Play.lastFrameTime = 0;
    m_Play.lastGameUpdateTime = 0;

    // Auto-start profiling on play
    ProfilingService::Get().StartRecording();
}

void EditorApp::Pause()
{
    if (m_PlayModeState == PlayModeState::Playing)
    {
        m_PlayModeState = PlayModeState::Paused;
    }
}

void EditorApp::Stop()
{
    if (m_PlayModeState == PlayModeState::Stopped)
        return;

    // Stop profiling when exiting play mode
    if (ProfilingService::Get().IsRecording())
    {
        ProfilingService::Get().StopRecording();
    }

    // Stop frame capture if active
    if (m_FrameCaptureService && m_FrameCaptureService->IsCapturing())
    {
        m_FrameCaptureService->StopCapture();
    }

    ShutdownPlayMode();
    m_PlayModeState = PlayModeState::Stopped;
    m_Play.camera = nullptr;

    // Restore the previously active tab (same approach as Play uses for "Play View")
    if (m_Play.prePlaySelectedTabId != 0)
    {
        ImGuiWindow* playWin = ImGui::FindWindowByName("Play View");
        if (playWin && playWin->DockId)
        {
            ImGuiDockNode* node = ImGui::DockBuilderGetNode(playWin->DockId);
            if (node && node->TabBar)
            {
                for (int i = 0; i < node->TabBar->Tabs.Size; i++)
                {
                    if (node->TabBar->Tabs[i].ID == m_Play.prePlaySelectedTabId)
                    {
                        ImGuiWindow* tabWin = node->TabBar->Tabs[i].Window;
                        if (tabWin)
                            ImGui::SetWindowFocus(tabWin->Name);
                        break;
                    }
                }
            }
        }
        m_Play.prePlaySelectedTabId = 0;
    }
}

void EditorApp::InitPlayMode()
{
    if (!m_CurrentScene)
        return;

    // UNITY-STYLE: Backup scene state to memory before play
    // This captures unsaved changes and allows restore on stop
    m_Play.sceneBackup = m_CurrentScene->SaveToJsonString();

    // Create isolated deep copy for play mode (JSON round-trip clone)
    m_Play.scene = std::make_unique<::Deki::Scene>();
    m_Play.scene->LoadFromJsonString(m_Play.sceneBackup);
    RegisterSceneCallbacks(m_Play.scene.get());

    // Setups that wait for Play, now that the user has asked for the game to
    // run. A package declares this with DEKI_REGISTER_PLAY_AUTO_SETUP, for
    // anything with a side effect outside the editor: deki-gps asks a third
    // party where the machine is, and doing that when a project merely OPENED
    // meant opening a project made a request nobody asked for.
    //
    // Idempotent per entry, so a second Play costs nothing, and a hot reload
    // that cleared and re-registered an entry lets it fire again on the next
    // Play.
    Deki::SetupComponent::RunEditorAutoSetups(
        Deki::SetupComponent::SetupPhase::PlayStart);

    // Save edit command history aside and start fresh for play mode
    CommandHistory::Instance().SaveAndClear();

    // Clear selection - we're switching to the play scene copy
    ClearSelection();

    // Play on the screen the Play view previews: the design size, or the
    // target picked in its toolbar. The camera fits the design area to it.
    m_Play.width = DekiEditor::SceneView::Get().GetPreviewWidth();
    m_Play.height = DekiEditor::SceneView::Get().GetPreviewHeight();

    // Create EditorDisplay directly (same pattern as Deki::Engine::Initialize editor path)
    // Memory, filesystem, logging, and timing are already initialized by OpenProject/editor startup
    s_EditorDisplay = std::make_unique<Deki::EditorDisplay>();
    // ... in the previewed screen's pixel format, which SetDisplay gives the framebuffer.
    s_EditorDisplay->SetColorFormat(DekiEditor::SceneView::Get().GetPreviewFormat());
    if (!s_EditorDisplay->Initialize(m_Play.width, m_Play.height))
    {
        DEKI_LOG_ERROR("Failed to initialize EditorDisplay");
        s_EditorDisplay.reset();
        return;
    }
    Deki::Engine::GetInstance().SetDisplay(s_EditorDisplay.get(), "Editor");

    // SetDisplay sized the engine's framebuffer to the display, as on a device.
    Deki::IRenderSystem* renderSystem = Deki::Engine::GetInstance().GetRenderSystem();
    if (!renderSystem || renderSystem->GetScreenWidth() != m_Play.width ||
        renderSystem->GetScreenHeight() != m_Play.height ||
        renderSystem->GetColorFormat() != s_EditorDisplay->GetColorFormat())
    {
        DEKI_LOG_ERROR("Play mode could not get a %dx%d framebuffer", m_Play.width, m_Play.height);
        Deki::Engine::GetInstance().SetDisplay(nullptr, "");
        s_EditorDisplay.reset();
        return;
    }
    m_Play.renderSystem = renderSystem;  // non-owning pointer

    // Get the EditorDisplay for the texture ID
    Deki::EditorDisplay* display = static_cast<Deki::EditorDisplay*>(Deki::Engine::GetInstance().GetDisplay());
    if (display)
    {
        m_Play.textureId = display->GetTextureId();
    }

    // Reuse engine's lifecycle manager (non-owning pointer)
    m_Play.lifecycleManager = Deki::Engine::GetInstance().GetLifecycleManager();

    // Set up Unity-style profiling callbacks for per-component lifecycle profiling
    // This profiles "TestComponent.Update", "EnemyAI.Start", etc.
    Deki::LifecycleManager::SetProfilingCallbacks(
        [](const char* name)
        {
            if (ProfilingService::IsRecordingStatic())
                ProfilingService::Get().PushEvent("behaviour", name);
        },
        []()
        {
            if (ProfilingService::IsRecordingStatic())
                ProfilingService::Get().PopEvent(0);
        });

    // Load binary lookup table from cache (same as embedded runtime)
    // This ensures editor play mode uses the exact same asset resolution path
    if (m_Project && m_AssetPipeline)
    {
        std::string projectPath = m_Project->GetPath();
        std::string tablePath = (ProjectPaths::Cache(projectPath) / "asset_table.bin").string();

        // Use existing asset_table.bin from cache (maintained by AssetPipeline)
        // If missing, regenerate it
        if (!fs::exists(tablePath))
        {
            DEKI_LOG_WARNING("PlayMode: No asset_table.bin in cache, regenerating...");
            m_AssetPipeline->UpdateAssetTable();
        }

        // Load into engine using standard file I/O
        std::ifstream tableFile(tablePath, std::ios::binary | std::ios::ate);
        if (tableFile.is_open())
        {
            size_t fileSize = static_cast<size_t>(tableFile.tellg());
            tableFile.seekg(0, std::ios::beg);

            // Static buffer to keep data alive for the duration of play mode
            static std::vector<uint8_t> s_PlayModeTableData;
            s_PlayModeTableData.resize(fileSize);

            if (tableFile.read(reinterpret_cast<char*>(s_PlayModeTableData.data()), fileSize))
            {
                Deki::AssetManager::Get()->LoadAssetLookupTable(s_PlayModeTableData.data(), s_PlayModeTableData.size());
            }
        }
        else
        {
            DEKI_LOG_ERROR("PlayMode: Failed to open asset_table.bin");
        }
    }

    // Set up asset loading for play mode
    if (m_Project && m_AssetPipeline)
    {
        std::string projectPath = m_Project->GetPath();

        // Notify packages that play mode is starting (e.g., deki-2d sets baked font GUIDs)
        if (m_PackageLoader)
        {
            m_PackageLoader->NotifyPlayModeStart(m_Play.scene.get());
        }

        // Assets are now auto-loaded via ProcessStart() in LifecycleManager
    }

    // Enter runtime mode - components follow device code paths (no editor GUID sync/baking)
    Deki::Engine::SetRuntimeMode(true);

    // Set the play mode COPY on Deki::Engine (not the edit scene)
    // Pass false for takeOwnership - editor owns m_Play.scene, not the engine
    Deki::Engine::GetInstance().SetRootScene(m_Play.scene.get(), false);

    DEKI_LOG_EDITOR("Play mode initialized: %dx%d", m_Play.width, m_Play.height);
}

void EditorApp::ShutdownPlayMode()
{
    // Notify packages that play mode is stopping
    if (m_PackageLoader)
        m_PackageLoader->NotifyPlayModeStop();

    // Exit runtime mode - components revert to editor code paths
    Deki::Engine::SetRuntimeMode(false);

    // Clear Deki::Engine's root scene reference.
    // If SceneSystem owns the current root (takeOwnership=true), it will be
    // queued for deferred deletion. Flush immediately since play mode is stopping.
    Deki::Engine::GetInstance().SetRootScene(nullptr, false);
    Deki::Engine::GetInstance().GetSceneSystem().FlushPendingDeletions();

    // Drop asset lookup cache so next play session loads fresh copies from disk.
    // Don't UnloadAll() - edit-mode components still reference loaded fonts/textures.
    Deki::AssetManager::Get()->DropCache();

    // Unload binary lookup table so edit mode reverts to m_GuidToPath map
    Deki::AssetLookupTable::Unload();

    // Clear Persistent objects (they only exist during play mode)
    Deki::Engine::GetInstance().GetSceneSystem().ClearPersistentObjects();

    // Clear selection - may point to play scene objects
    ClearSelection();

    // Clear any cached object/component references in panels
    if (m_HierarchyPanel)
        m_HierarchyPanel->ClearSceneReferences();
    if (m_InspectorPanel)
        m_InspectorPanel->ClearSceneReferences();

    // Don't delete the texture - it's owned by EditorDisplay
    m_Play.textureId = 0;

    // Clear non-owning pointers (engine owns these subsystems)
    m_Play.renderSystem = nullptr;

    // Clear profiling callbacks and release lifecycle manager reference
    Deki::LifecycleManager::SetProfilingCallbacks(nullptr, nullptr);
    m_Play.lifecycleManager = nullptr;

    // Shutdown display backend
    // NOTE: We do NOT shutdown FileSystem or MemoryManager here because
    // edit mode needs them for unified rendering (asset loading in edit view)
    Deki::Engine::GetInstance().SetDisplay(nullptr, "");
    s_EditorDisplay.reset();

    // Destroy the play mode scene copy - edit scene was never modified
    m_Play.scene.reset();
    m_Play.sceneBackup.clear();

    // Restore edit command history (discards play mode commands)
    CommandHistory::Instance().RestoreFromSave();

    m_Play.width = 0;
    m_Play.height = 0;
    m_Play.ready = false;

    // Reset frame timing accumulator
    m_Play.timeAccumulator = 0.0f;
    m_Play.lastFrameTime = 0;
    m_Play.lastGameUpdateTime = 0;
    m_Play.gameFps = 0.0f;
    m_Play.fpsSubStepCounter = 0;
    m_Play.fpsWindowMs = 0.0f;

    // Restore editor vsync (UpdatePlayMode may have disabled it for uncapped play).
    if (m_Play.lastSwapInterval != 1)
    {
        SDL_GL_SetSwapInterval(1);
        m_Play.lastSwapInterval = 1;
    }
}

namespace
{
// Returns true on success. On failure, logs to ConsolePanel and sets the crash flag.
bool GuardedInvoke(const char* phase, const char* sceneName,
                   bool& crashedFlag, const std::function<void()>& fn)
{
    PlayModeGuard::CrashInfo info;
    if (PlayModeGuard::Invoke(phase, fn, info))
        return true;

    std::string msg = "[Play Mode] Crash in ";
    msg += phase ? phase : "?";
    msg += " (scene '";
    msg += sceneName && *sceneName ? sceneName : "<unnamed>";
    msg += "'): ";
    msg += info.message;
    if (!info.accessDetails.empty())
    {
        msg += "\n  ";
        // Strip trailing newline FormatAccessViolationDetails appends.
        std::string ad = info.accessDetails;
        while (!ad.empty() && (ad.back() == '\n' || ad.back() == '\r'))
            ad.pop_back();
        msg += ad;
    }
    if (!info.stackTrace.empty())
    {
        msg += "\nStack trace:\n";
        msg += info.stackTrace;
    }

    if (auto* console = ConsolePanel::GetInstance())
        console->LogError(msg);
    else
        DEKI_LOG_ERROR("%s", msg.c_str());
    crashedFlag = true;
    return false;
}
}  // namespace

void EditorApp::UpdatePlayMode()
{
    // NOTE: Don't profile this function directly - profiler frames are defined by
    // BeginFrame/EndFrame below. Profiling here would span multiple profiler frames.

    if (m_PlayModeState != PlayModeState::Playing || !m_Play.scene)
        return;

    // If a guarded call faulted last frame, stop play mode now (out of the
    // crashing call's stack) and bail. Editor stays alive.
    if (m_Play.crashedThisFrame)
    {
        m_Play.crashedThisFrame = false;
        Stop();
        return;
    }

    // Frame pacing comes from the Core section: it is a frame-loop concern, not
    // framebuffer geometry, and applies with no rendering package loaded. capFps=true caps at
    // targetFps (fixed-timestep, smooth animations); capFps=false disables
    // editor vsync during play so the game can exceed the monitor refresh
    // rate. Without this, the editor's vsync (set in main.cpp) would block
    // SwapWindow at the refresh rate and the game would inherit that ceiling.
    auto* core = DekiEditor::GetCoreSettings();
    const bool capFps = core ? core->capFps : true;
    const int targetFps = (core && core->targetFps > 0) ? core->targetFps : 60;
    const float targetFrameTimeMs = 1000.0f / targetFps;

    // Apply desired swap interval (only when it changes). Uncapped play mode
    // turns vsync off; capped play mode keeps vsync on so the editor stays
    // smooth and we don't burn CPU running the editor at 1000+ FPS.
    const int desiredSwapInterval = capFps ? 1 : 0;
    if (desiredSwapInterval != m_Play.lastSwapInterval)
    {
        SDL_GL_SetSwapInterval(desiredSwapInterval);
        m_Play.lastSwapInterval = desiredSwapInterval;
    }

    // Calculate elapsed time since last call (accumulator pattern when capped)
    uint64_t currentTime = SDL_GetTicks();
    if (m_Play.lastFrameTime == 0)
        m_Play.lastFrameTime = currentTime;

    float deltaMs = static_cast<float>(currentTime - m_Play.lastFrameTime);
    m_Play.lastFrameTime = currentTime;

    // Accumulate wall-clock time. Sub-step loop below drains it.
    m_Play.timeAccumulator += deltaMs;

    // Spiral-of-death guard (capped only): if we fell catastrophically behind
    // drop the slack so we don't try to catch up over many frames. Game
    // appears to "pause" rather than chew CPU. Uncapped mode can't fall
    // behind - it drains the entire accumulator each frame.
    if (capFps)
    {
        const float kMaxAccumulator = targetFrameTimeMs * 4.0f;
        if (m_Play.timeAccumulator > kMaxAccumulator)
            m_Play.timeAccumulator = targetFrameTimeMs;
    }

    // Sub-step loop:
    //   capped:   drain accumulator in fixed targetFrameTimeMs increments,
    //             up to kMaxSubStepsPerEditorFrame ticks per editor frame
    //             (lets a high targetFps run multiple ticks per editor frame).
    //   uncapped: time-budgeted loop - keep ticking until kUncappedBudgetNs
    //             of real wall-clock has been consumed (or the sanity cap is
    //             hit). Each tick uses the actual elapsed wall-clock since
    //             the previous tick as its dt, so game time advances at real
    //             time regardless of how many ticks fit per editor frame.
    //             This decouples game FPS from editor FPS: a single editor
    //             frame can produce hundreds of game ticks.
    constexpr int kMaxSubStepsPerEditorFrame = 4;
    constexpr uint64_t kUncappedBudgetNs = 4'000'000ULL;  // 4 ms / editor frame
    constexpr int kUncappedMaxSubsteps = 1024;            // sanity cap
    const int loopMaxSubsteps = capFps ? kMaxSubStepsPerEditorFrame : kUncappedMaxSubsteps;
    const uint64_t uncapStartNs = SDL_GetTicksNS();
    uint64_t uncapLastTickNs = uncapStartNs;
    int subStepsThisFrame = 0;

    // Self-passing lambda: recursive without a std::function (which was
    // rebuilt on the heap every game tick and dispatched indirectly per object).
    auto updateComponentsRecursive = [&](auto&& self, Deki::Object* obj, float deltaSeconds) -> void
    {
        if (!obj) return;

        // Resolving the component's display name costs a registry lookup, and
        // it is only ever used as a profiler label. Hoisted behind the
        // recording check so an ordinary play session doesn't pay a hash lookup
        // per component per frame for a string nobody reads.
        const bool profiling = ProfilingService::IsRecordingStatic();

        for (Deki::Component* comp : obj->GetComponents())
        {
            if (comp->NeedsRuntimeUpdate())
            {
                if (profiling)
                {
                    const Deki::ComponentMeta* meta = Deki::ComponentRegistry::Instance().GetMeta(comp->GetType());
                    const char* compName = meta ? meta->name : "Unknown";
                    PROFILE_SCOPE("components", compName);
                    comp->RuntimeUpdate(deltaSeconds);
                }
                else
                {
                    comp->RuntimeUpdate(deltaSeconds);
                }
            }
        }

        for (Deki::Object* child : obj->GetChildren())
        {
            self(self, child, deltaSeconds);
        }
    };

    while (subStepsThisFrame < loopMaxSubsteps)
    {
        // Per-iteration loop condition + dt selection.
        float loopStepDtMs;
        if (capFps)
        {
            if (m_Play.timeAccumulator < targetFrameTimeMs) break;
            m_Play.timeAccumulator -= targetFrameTimeMs;
            if (m_Play.timeAccumulator < 0.0f) m_Play.timeAccumulator = 0.0f;
            loopStepDtMs = targetFrameTimeMs;
        }
        else
        {
            // Stop once the per-editor-frame budget is consumed (but always
            // run at least one tick so we make progress even if scheduling
            // is jittery).
            if (subStepsThisFrame > 0 && (SDL_GetTicksNS() - uncapStartNs) >= kUncappedBudgetNs)
                break;
            const uint64_t nowNs = SDL_GetTicksNS();
            const uint64_t deltaNs = nowNs - uncapLastTickNs;
            uncapLastTickNs = nowNs;
            loopStepDtMs = static_cast<float>(deltaNs) / 1'000'000.0f;
            if (loopStepDtMs <= 0.0f) loopStepDtMs = 0.001f;  // sub-µs ticks: floor at 1 µs
        }
        ++subStepsThisFrame;

        // Begin profiler frame (one per game tick, like Unity)
        ProfilingService::Get().BeginFrame();

        // Fixed timestep when capped (dt = targetFrameTimeMs); real elapsed
        // wall-clock when uncapped (sum across ticks ≈ real time, so animations
        // advance at real rate regardless of tick frequency).
        Deki::Time::UpdateF(loopStepDtMs);

        m_Play.lastGameUpdateTime = currentTime;

        // Flush deferred scene deletions from previous tick
        Deki::Engine::GetInstance().GetSceneSystem().FlushPendingDeletions();

        // Get the active scene from Deki::Engine (same as embedded runtime)
        Deki::Scene* activeScene = Deki::Engine::GetInstance().GetRootScene();
        if (!activeScene)
        {
            DEKI_LOG_ERROR("Play mode: root scene is null - stopping");
            ProfilingService::Get().EndFrame();
            Stop();
            return;
        }

        const char* sceneName = activeScene->GetName().c_str();

        // Process lifecycle: InternalAwake -> Awake -> Start -> Update (correct order)
        {
            CrashHandler::SetPlayModeContext("InternalAwake", sceneName);
            PROFILE_SCOPE("lifecycle", "ProcessInternalAwake");
            if (!GuardedInvoke("InternalAwake", sceneName, m_Play.crashedThisFrame,
                               [&]
                               { m_Play.lifecycleManager->ProcessInternalAwake(activeScene); }))
            {
                ProfilingService::Get().EndFrame();
                return;
            }
        }

        {
            CrashHandler::SetPlayModeContext("Awake", sceneName);
            PROFILE_SCOPE("lifecycle", "ProcessAwake");
            if (!GuardedInvoke("Awake", sceneName, m_Play.crashedThisFrame,
                               [&]
                               { m_Play.lifecycleManager->ProcessAwake(activeScene); }))
            {
                ProfilingService::Get().EndFrame();
                return;
            }
        }

        {
            CrashHandler::SetPlayModeContext("Start", sceneName);
            PROFILE_SCOPE("lifecycle", "ProcessStart");
            if (!GuardedInvoke("Start", sceneName, m_Play.crashedThisFrame,
                               [&]
                               { m_Play.lifecycleManager->ProcessStart(activeScene); }))
            {
                ProfilingService::Get().EndFrame();
                return;
            }
        }

        {
            CrashHandler::SetPlayModeContext("Update", sceneName);
            PROFILE_SCOPE("lifecycle", "ProcessUpdate");
            if (!GuardedInvoke("Update", sceneName, m_Play.crashedThisFrame,
                               [&]
                               { m_Play.lifecycleManager->ProcessUpdate(activeScene); }))
            {
                ProfilingService::Get().EndFrame();
                return;
            }
        }

        // Package systems registered with Deki::Engine::RegisterUpdate (tweens,
        // ...). On a device Deki::Engine::Update runs these; play mode drives the
        // lifecycle by hand, so it has to run them itself or programmatic tweens
        // only work outside the editor.
        {
            CrashHandler::SetPlayModeContext("UpdateCallbacks", sceneName);
            PROFILE_SCOPE("lifecycle", "UpdateCallbacks");
            if (!GuardedInvoke("UpdateCallbacks", sceneName, m_Play.crashedThisFrame,
                               [&]
                               { Deki::Engine::GetInstance().RunUpdateCallbacks(static_cast<uint32_t>(loopStepDtMs)); }))
            {
                ProfilingService::Get().EndFrame();
                return;
            }
        }

        // Update components that need runtime updates
        {
            CrashHandler::SetPlayModeContext("ComponentUpdates", sceneName);
            PROFILE_SCOPE("lifecycle", "ComponentUpdates");
            float deltaSeconds = loopStepDtMs / 1000.0f;
            if (!GuardedInvoke("ComponentUpdates", sceneName, m_Play.crashedThisFrame,
                               [&]
                               {
                                   for (Deki::Object* obj : activeScene->GetObjects())
                                       updateComponentsRecursive(updateComponentsRecursive, obj, deltaSeconds);
                               }))
            {
                ProfilingService::Get().EndFrame();
                return;
            }
        }

        // Check if scene changed during lifecycle/update (e.g., SetRootScene called)
        // If so, process lifecycle on new scene before next sub-step or render
        Deki::Scene* currentScene = Deki::Engine::GetInstance().GetRootScene();
        if (currentScene != activeScene && currentScene != nullptr)
        {
            const char* newSceneName = currentScene->GetName().c_str();

            // Set baked font GUIDs on new scene (same as initial play mode start)
            if (m_PackageLoader)
            {
                m_PackageLoader->NotifyPlayModeStart(currentScene);
            }

            {
                CrashHandler::SetPlayModeContext("NewSceneInternalAwake", newSceneName);
                PROFILE_SCOPE("lifecycle", "ProcessInternalAwake");
                if (!GuardedInvoke("NewSceneInternalAwake", newSceneName, m_Play.crashedThisFrame,
                                   [&]
                                   { m_Play.lifecycleManager->ProcessInternalAwake(currentScene); }))
                {
                    ProfilingService::Get().EndFrame();
                    return;
                }
            }
            {
                CrashHandler::SetPlayModeContext("NewSceneAwake", newSceneName);
                PROFILE_SCOPE("lifecycle", "ProcessAwake");
                if (!GuardedInvoke("NewSceneAwake", newSceneName, m_Play.crashedThisFrame,
                                   [&]
                                   { m_Play.lifecycleManager->ProcessAwake(currentScene); }))
                {
                    ProfilingService::Get().EndFrame();
                    return;
                }
            }
            {
                CrashHandler::SetPlayModeContext("NewSceneStart", newSceneName);
                PROFILE_SCOPE("lifecycle", "ProcessStart");
                if (!GuardedInvoke("NewSceneStart", newSceneName, m_Play.crashedThisFrame,
                                   [&]
                                   { m_Play.lifecycleManager->ProcessStart(currentScene); }))
                {
                    ProfilingService::Get().EndFrame();
                    return;
                }
            }
        }

        CrashHandler::ClearPlayModeContext();

        // End profiler frame for this sub-step
        ProfilingService::Get().EndFrame();
    }

    // Sample game FPS over a rolling 500 ms window: count sub-steps and
    // divide by elapsed wall-clock. Per-editor-frame sampling doesn't work
    // because some editor frames produce 0 sub-steps (game slower than
    // editor) and others produce multiple, so a single-frame estimate is too
    // noisy for a UI readout.
    m_Play.fpsSubStepCounter += subStepsThisFrame;
    m_Play.fpsWindowMs += deltaMs;
    if (m_Play.fpsWindowMs >= 500.0f)
    {
        m_Play.gameFps = (m_Play.fpsSubStepCounter > 0)
                             ? (m_Play.fpsSubStepCounter * 1000.0f) / m_Play.fpsWindowMs
                             : 0.0f;
        m_Play.fpsSubStepCounter = 0;
        m_Play.fpsWindowMs = 0.0f;
    }

    // Render once per editor frame using the latest simulated state. If the
    // accumulator didn't yield any sub-steps this editor frame, skip the
    // render too - nothing changed since the previous editor frame's render,
    // and PlayView keeps displaying the existing texture.
    if (subStepsThisFrame > 0)
    {
        Deki::Scene* renderScene = Deki::Engine::GetInstance().GetRootScene();
        const char* renderSceneName = renderScene ? renderScene->GetName().c_str() : "<unknown>";
        CrashHandler::SetPlayModeContext("Render", renderSceneName);
        if (!GuardedInvoke("Render", renderSceneName, m_Play.crashedThisFrame,
                           [&]
                           { RenderPlayMode(); }))
            return;
        CrashHandler::ClearPlayModeContext();
    }
}

void EditorApp::RenderPlayMode()
{
    PROFILE_SCOPE("playmode", "RenderPlayMode");

    if (!m_Play.renderSystem)
    {
        DEKI_LOG_ERROR("RenderPlayMode: render system is null - stopping");
        Stop();
        return;
    }

    // Get the active scene from Deki::Engine (same as embedded runtime)
    Deki::Scene* activeScene = Deki::Engine::GetInstance().GetRootScene();
    if (!activeScene)
    {
        DEKI_LOG_ERROR("RenderPlayMode: root scene is null - stopping");
        Stop();
        return;
    }

    // Use unified render system (same as embedded runtime)
    // DekiRenderSystem handles: finding camera, clearing buffer, rendering all objects via QuadBlit
    {
        PROFILE_SCOPE("rendering", "RenderSystem");
        m_Play.renderSystem->Render(activeScene);
    }

    // Capture frame if recording (F9 to toggle)
    if (m_FrameCaptureService && m_FrameCaptureService->IsCapturing())
    {
        m_FrameCaptureService->CaptureFrame(
            m_Play.renderSystem->GetFrameBuffer(),
            m_Play.width,
            m_Play.height,
            m_Play.renderSystem->GetColorFormat());
    }

    // Present the frame buffer through DekiDisplayBackend (editor-only: GL texture upload)
    {
        PROFILE_SCOPE("editor", "PresentFrame");
        if (Deki::Engine::GetInstance().GetDisplay())
        {
            // Partial upload of the changed rectangles when dirty tracking is
            // on, the whole frame otherwise (same path as the device).
            Deki::Engine::GetInstance().PresentFrame();
            m_Play.ready = true;  // First frame rendered, safe to show play mode texture
        }
    }
}

void EditorApp::HandlePlayModeInput(float gameX, float gameY, bool isDown, bool isMove, bool isUp)
{
    if (m_PlayModeState != PlayModeState::Playing)
        return;

    Deki::Scene* activeScene = Deki::Engine::GetInstance().GetRootScene();
    if (!activeScene)
        return;

    if (!m_Play.camera)
        return;

    float worldX, worldY;
    m_Play.camera->ScreenToWorld(gameX, gameY,
                                 m_Play.width, m_Play.height, worldX, worldY);

    if (auto* inputSystem = Deki::Engine::GetInstance().GetInputSystem())
        inputSystem->DispatchInput(activeScene, worldX, worldY, isDown, isMove, isUp);
}

void EditorApp::HandlePlayModeKey(uint32_t key, bool down)
{
    // Key-downs only while playing; key-ups always pass so a key held when
    // play stops or focus leaves the play view doesn't stay stuck pressed.
    if (down && m_PlayModeState != PlayModeState::Playing)
        return;

    if (auto* inputSystem = Deki::Engine::GetInstance().GetInputSystem())
        inputSystem->DispatchKey(key, down);
}

}  // namespace DekiEditor
