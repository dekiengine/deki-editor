// Play mode for the editor. Renders through Deki::Engine and a display, as
// the simulator does.
//
// Keep this file free of specific components: it goes through the
// Deki::Component lifecycle (LoadAssets, UnloadAssets, RuntimeUpdate) and
// the input system, never GetComponent<SpriteComponent> and the like.

#include <deki-editor/SceneView.h>
#include "EditorApp.h"
#include <deki-editor/build/ProjectPaths.h>
#include <deki-editor/CommandHistory.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <deki-editor/Paths.h>
#include "services/CrashHandler.h"
#include "services/EditorCapture.h"
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

// Play mode owns the EditorDisplay; engine-core only holds the pointer.
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
    if (!obj)
    {
        return nullptr;
    }
    if (Deki::ICamera* c = obj->FindInterface<Deki::ICamera>())
    {
        return c;
    }
    for (Deki::Object* child : obj->GetChildren())
    {
        if (Deki::ICamera* c = FindFirstCameraRecursive(child))
        {
            return c;
        }
    }
    return nullptr;
}

Deki::ICamera* FindFirstCameraInScene(Deki::Scene* scene)
{
    if (!scene)
    {
        return nullptr;
    }
    for (Deki::Object* obj : scene->GetObjects())
    {
        if (Deki::ICamera* c = FindFirstCameraRecursive(obj))
        {
            return c;
        }
    }
    return nullptr;
}

}  // namespace

void EditorApp::Play()
{
    if (!m_CurrentScene)
    {
        return;
    }

    if (m_PlayModeState == PlayModeState::Paused)
    {
        m_PlayModeState = PlayModeState::Playing;
        return;
    }

    if (m_PlayModeState == PlayModeState::Playing)
    {
        return;
    }

    // No play mode while there are compile errors.
    if ((m_HotReloadService && m_HotReloadService->HasBuildErrors()) ||
        (m_PackageLoader && m_PackageLoader->HasBuildErrors()))
    {
        DEKI_LOG_ERROR("Cannot enter play mode - fix compilation errors first");
        if (m_ConsolePanel)
        {
            m_ConsolePanel->isOpen = true;
            m_ConsolePanel->focusFrames = 10;
        }
        return;
    }

    InitPlayMode();

    if (!m_Play.renderSystem || m_Play.textureId == 0 || !Deki::Engine::GetInstance().GetRootScene())
    {
        DEKI_LOG_ERROR("Play mode initialization failed - aborting");
        ShutdownPlayMode();
        if (m_ConsolePanel)
        {
            m_ConsolePanel->isOpen = true;
            m_ConsolePanel->focusFrames = 10;
        }
        return;
    }

    m_PlayModeState = PlayModeState::Playing;
    m_Play.camera = FindFirstCameraInScene(Deki::Engine::GetInstance().GetRootScene());

    // Open and focus the Play View panel.
    if (m_PlayViewPanel)
    {
        m_PlayViewPanel->isOpen = true;

        // Remember the selected tab in Play View's dock node, for Stop to restore.
        ImGuiWindow* playWin = ImGui::FindWindowByName("Play View");
        if (playWin && playWin->DockId)
        {
            ImGuiDockNode* node = ImGui::DockBuilderGetNode(playWin->DockId);
            if (node && node->TabBar)
            {
                m_Play.prePlaySelectedTabId = node->TabBar->SelectedTabId;
            }
        }

        ImGui::SetWindowFocus("PLAY VIEW###Play View");
    }

    m_Play.timeAccumulator = 0.0f;
    m_Play.lastFrameTime = 0;
    m_Play.lastGameUpdateTime = 0;

    // Profiling starts with play.
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
    {
        return;
    }

    if (ProfilingService::Get().IsRecording())
    {
        ProfilingService::Get().StopRecording();
    }

    if (m_FrameCaptureService && m_FrameCaptureService->IsCapturing())
    {
        m_FrameCaptureService->StopCapture();
    }

    ShutdownPlayMode();
    m_PlayModeState = PlayModeState::Stopped;
    m_Play.camera = nullptr;

    // Back on the edit scene: select what was selected before Play.
    ClearSelection();
    if (m_CurrentScene)
    {
        for (uint32_t id : m_Play.prePlaySelectedIds)
        {
            if (Deki::Object* obj = m_CurrentScene->FindDekiObject(id))
            {
                AddToSelection(obj);
            }
        }
    }
    m_Play.prePlaySelectedIds.clear();

    // Bring back the tab that was active before Play.
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
                        {
                            ImGui::SetWindowFocus(tabWin->Name);
                        }
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
    {
        return;
    }

    // Keep the scene, unsaved changes included, in memory for Stop.
    m_Play.sceneBackup = m_CurrentScene->SaveToJsonString();

    // Play runs on a separate copy, cloned through JSON.
    m_Play.scene = std::make_unique<::Deki::Scene>();
    m_Play.scene->LoadFromJsonString(m_Play.sceneBackup);
    RegisterSceneCallbacks(m_Play.scene.get());

    // Setups that wait until the user asks the game to run. A package
    // declares one with DEKI_REGISTER_PLAY_AUTO_SETUP for anything with a side
    // effect outside the editor, such as deki-gps asking a third party where
    // the machine is, which must not happen just because a project opened.
    //
    // Each entry runs once, so a second Play costs nothing; a hot reload that
    // re-registers an entry lets it run again on the next Play.
    Deki::SetupComponent::RunEditorAutoSetups(Deki::SetupComponent::SetupPhase::PlayStart);

    // Put the edit undo history aside; play mode starts with an empty one.
    CommandHistory::Instance().SaveAndClear();

    // The selection moves to the play copy's same objects (same ids).
    m_Play.prePlaySelectedIds.clear();
    for (Deki::Object* obj : GetSelectedObjects())
    {
        if (obj)
        {
            m_Play.prePlaySelectedIds.push_back(obj->GetId());
        }
    }
    ClearSelection();
    for (uint32_t id : m_Play.prePlaySelectedIds)
    {
        if (Deki::Object* copy = m_Play.scene->FindDekiObject(id))
        {
            AddToSelection(copy);
        }
    }

    // Play on the screen the Play view previews: the design size, or the
    // target picked in its toolbar. The camera decides what it shows there.
    m_Play.width = DekiEditor::SceneView::Get().GetPreviewWidth();
    m_Play.height = DekiEditor::SceneView::Get().GetPreviewHeight();

    // Memory, filesystem, logging and timing are already up from editor
    // startup and OpenProject; only the display is new.
    s_EditorDisplay = std::make_unique<Deki::EditorDisplay>();
    // In the previewed screen's pixel format, which SetDisplay gives the framebuffer.
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

    Deki::EditorDisplay* display = static_cast<Deki::EditorDisplay*>(Deki::Engine::GetInstance().GetDisplay());
    if (display)
    {
        m_Play.textureId = display->GetTextureId();
    }

    m_Play.lifecycleManager = Deki::Engine::GetInstance().GetLifecycleManager();  // non-owning

    // Profile each component lifecycle call ("EnemyAI.Update", "EnemyAI.Start").
    Deki::LifecycleManager::SetProfilingCallbacks(
        [](const char* name)
        {
            if (ProfilingService::IsRecordingStatic())
            {
                ProfilingService::Get().PushEvent("behaviour", name);
            }
        },
        []()
        {
            if (ProfilingService::IsRecordingStatic())
            {
                ProfilingService::Get().PopEvent(0);
            }
        });

    // Scenes opened during play load compiled from the cache. One compiled
    // before its packages were built or loaded, or before a component gained
    // a property, holds its components without their properties, so those are
    // compiled again first.
    if (m_AssetPipeline)
    {
        m_AssetPipeline->RecompileStaleScenes();
    }

    // Load the binary asset lookup table from the cache, as a device does, so
    // play mode resolves assets the same way.
    if (m_Project && m_AssetPipeline)
    {
        std::string projectPath = m_Project->GetPath();
        std::string tablePath = (ProjectPaths::Cache(projectPath) / "asset_table.bin").string();

        // AssetPipeline keeps asset_table.bin up to date; rebuild it if missing.
        if (!fs::exists(tablePath))
        {
            DEKI_LOG_WARNING("PlayMode: No asset_table.bin in cache, regenerating...");
            m_AssetPipeline->UpdateAssetTable();
        }

        std::ifstream tableFile(tablePath, std::ios::binary | std::ios::ate);
        if (tableFile.is_open())
        {
            size_t fileSize = static_cast<size_t>(tableFile.tellg());
            tableFile.seekg(0, std::ios::beg);

            // Static: the table points into this data for the whole play session.
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

    if (m_Project && m_AssetPipeline)
    {
        std::string projectPath = m_Project->GetPath();

        // Tell packages play mode is starting (deki-2d sets baked font GUIDs).
        if (m_PackageLoader)
        {
            m_PackageLoader->NotifyPlayModeStart(m_Play.scene.get());
        }

        // Assets load through LifecycleManager's ProcessStart().
    }

    // Runtime mode: components take device code paths (no editor GUID sync or baking).
    Deki::Engine::SetRuntimeMode(true);

    // The engine runs the play copy, not the edit scene. The editor owns
    // m_Play.scene, so the engine does not take ownership.
    Deki::Engine::GetInstance().SetRootScene(m_Play.scene.get(), false);

    DEKI_LOG_EDITOR("Play mode initialized: %dx%d", m_Play.width, m_Play.height);
}

void EditorApp::ShutdownPlayMode()
{
    if (m_PackageLoader)
    {
        m_PackageLoader->NotifyPlayModeStop();
    }

    // Components go back to editor code paths.
    Deki::Engine::SetRuntimeMode(false);

    // A root the SceneSystem owns is queued for deferred deletion; flush it
    // now, since play mode is stopping.
    Deki::Engine::GetInstance().SetRootScene(nullptr, false);
    Deki::Engine::GetInstance().GetSceneSystem().FlushPendingDeletions();

    // Drop the asset cache so the next play session loads fresh copies. Not
    // UnloadAll(): edit-mode components still use the loaded fonts and textures.
    Deki::AssetManager::Get()->DropCache();

    // Edit mode goes back to the m_GuidToPath map.
    Deki::AssetLookupTable::Unload();

    // Persistent objects exist only during play mode.
    Deki::Engine::GetInstance().GetSceneSystem().ClearPersistentObjects();

    // The selection may point at play scene objects.
    ClearSelection();

    // Panels may hold play scene objects and components.
    if (m_HierarchyPanel)
    {
        m_HierarchyPanel->ClearSceneReferences();
    }
    if (m_InspectorPanel)
    {
        m_InspectorPanel->ClearSceneReferences();
    }

    // EditorDisplay owns the texture.
    m_Play.textureId = 0;

    // The engine owns the render system.
    m_Play.renderSystem = nullptr;

    Deki::LifecycleManager::SetProfilingCallbacks(nullptr, nullptr);
    m_Play.lifecycleManager = nullptr;

    // Only the display goes. FileSystem and Memory stay: edit mode loads
    // assets through them too.
    Deki::Engine::GetInstance().SetDisplay(nullptr, "");
    s_EditorDisplay.reset();

    // Destroy the play copy; the edit scene was never touched.
    m_Play.scene.reset();
    m_Play.sceneBackup.clear();

    // Bring back the edit undo history; play mode's commands are dropped.
    CommandHistory::Instance().RestoreFromSave();

    m_Play.width = 0;
    m_Play.height = 0;
    m_Play.ready = false;

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
// Runs `fn` under the crash guard. On a crash, logs to the console, sets
// `crashedFlag` and returns false.
bool GuardedInvoke(const char* phase, const char* sceneName, bool& crashedFlag, const std::function<void()>& fn)
{
    PlayModeGuard::CrashInfo info;
    if (PlayModeGuard::Invoke(phase, fn, info))
    {
        return true;
    }

    std::string msg = "[Play Mode] Crash in ";
    msg += phase ? phase : "?";
    msg += " (scene '";
    msg += sceneName && *sceneName ? sceneName : "<unnamed>";
    msg += "'): ";
    msg += info.message;
    if (!info.accessDetails.empty())
    {
        msg += "\n  ";
        // Drop the newline FormatAccessViolationDetails appends.
        std::string ad = info.accessDetails;
        while (!ad.empty() && (ad.back() == '\n' || ad.back() == '\r'))
        {
            ad.pop_back();
        }
        msg += ad;
    }
    if (!info.stackTrace.empty())
    {
        msg += "\nStack trace:\n";
        msg += info.stackTrace;
    }

    if (auto* console = ConsolePanel::GetInstance())
    {
        console->LogError(msg);
    }
    else
    {
        DEKI_LOG_ERROR("%s", msg.c_str());
    }
    crashedFlag = true;
    return false;
}
}  // namespace

void EditorApp::UpdatePlayMode()
{
    // No profile scope for this function: it spans several profiler frames,
    // which BeginFrame/EndFrame below mark out.

    if (m_PlayModeState != PlayModeState::Playing || !m_Play.scene)
    {
        return;
    }

    // A guarded call crashed last frame: stop play mode now, outside the
    // crashing call's stack. The editor keeps running.
    if (m_Play.crashedThisFrame)
    {
        m_Play.crashedThisFrame = false;
        Stop();
        return;
    }

    // Frame pacing comes from the Core settings, since it belongs to the frame
    // loop and applies with no rendering package loaded. capFps caps at
    // targetFps with a fixed timestep. Without capFps, editor vsync is off
    // during play so the game can run faster than the monitor refresh rate;
    // the editor's vsync (set in main.cpp) would otherwise hold it there.
    auto* core = DekiEditor::GetCoreSettings();
    const bool capFps = core ? core->capFps : true;
    const int targetFps = (core && core->targetFps > 0) ? core->targetFps : 60;
    const float targetFrameTimeMs = 1000.0f / targetFps;

    // Set the swap interval when it changes. Capped play keeps vsync on, so
    // the editor stays smooth and does not burn CPU at 1000+ FPS.
    const int desiredSwapInterval = capFps ? 1 : 0;
    if (desiredSwapInterval != m_Play.lastSwapInterval)
    {
        SDL_GL_SetSwapInterval(desiredSwapInterval);
        m_Play.lastSwapInterval = desiredSwapInterval;
    }

    uint64_t currentTime = SDL_GetTicks();
    if (m_Play.lastFrameTime == 0)
    {
        m_Play.lastFrameTime = currentTime;
    }

    float deltaMs = static_cast<float>(currentTime - m_Play.lastFrameTime);
    m_Play.lastFrameTime = currentTime;

    // While capture_sequence records, the game advances one 60 fps frame per
    // editor frame, as the editor's own animations do (main.cpp). Each capture
    // is slow to write, and on the wall clock the recording would show the
    // game jumping ahead by however long that took.
    const bool recording = DekiEditor::EditorCapture::Get().IsRecordingSequence();
    if (recording)
    {
        deltaMs = 1000.0f / 60.0f;
    }

    // Wall-clock time; the sub-step loop below drains it.
    m_Play.timeAccumulator += deltaMs;

    // Capped only: when far behind, drop the backlog instead of catching up
    // over many frames. The game seems to pause rather than eat the CPU.
    // Uncapped play drains the whole accumulator each frame, so it cannot
    // fall behind.
    if (capFps)
    {
        const float kMaxAccumulator = targetFrameTimeMs * 4.0f;
        if (m_Play.timeAccumulator > kMaxAccumulator)
        {
            m_Play.timeAccumulator = targetFrameTimeMs;
        }
    }

    // Sub-step loop:
    //   capped:   drain the accumulator in fixed targetFrameTimeMs steps, up
    //             to kMaxSubStepsPerEditorFrame ticks per editor frame (so a
    //             high targetFps runs several ticks per editor frame).
    //   uncapped: tick until kUncappedBudgetNs of wall-clock time is used (or
    //             the sanity cap is hit). Each tick's dt is the real time
    //             since the previous tick, so game time follows real time
    //             however many ticks fit. One editor frame can run hundreds
    //             of game ticks, so game FPS does not depend on editor FPS.
    constexpr int kMaxSubStepsPerEditorFrame = 4;
    constexpr uint64_t kUncappedBudgetNs = 4'000'000ULL;  // 4 ms / editor frame
    constexpr int kUncappedMaxSubsteps = 1024;            // sanity cap
    const int loopMaxSubsteps = capFps ? kMaxSubStepsPerEditorFrame : kUncappedMaxSubsteps;
    const uint64_t uncapStartNs = SDL_GetTicksNS();
    uint64_t uncapLastTickNs = uncapStartNs;
    int subStepsThisFrame = 0;
    // A tick's profiler frame stays open until the next tick starts, so the
    // last tick's frame also holds the render after the loop. Otherwise the
    // render falls outside every frame and frames look far too cheap.
    bool profilerFrameOpen = false;

    // Self-passing lambda: recursive without a std::function, which would be
    // built on the heap every game tick and called indirectly per object.
    auto updateComponentsRecursive = [&](auto&& self, Deki::Object* obj, float deltaSeconds) -> void
    {
        if (!obj)
        {
            return;
        }

        // The component's name costs a registry lookup and is only a profiler
        // label, so it is looked up only while recording.
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
        // Whether to tick again, and this tick's dt.
        float loopStepDtMs;
        if (capFps)
        {
            if (m_Play.timeAccumulator < targetFrameTimeMs)
            {
                break;
            }
            m_Play.timeAccumulator -= targetFrameTimeMs;
            if (m_Play.timeAccumulator < 0.0f)
            {
                m_Play.timeAccumulator = 0.0f;
            }
            loopStepDtMs = targetFrameTimeMs;
        }
        else if (recording)
        {
            // One tick of one 60 fps frame, as above.
            if (subStepsThisFrame > 0)
            {
                break;
            }
            loopStepDtMs = 1000.0f / 60.0f;
        }
        else
        {
            // Stop once this editor frame's budget is used, but always run at
            // least one tick, so the game moves even when scheduling jitters.
            if (subStepsThisFrame > 0 && (SDL_GetTicksNS() - uncapStartNs) >= kUncappedBudgetNs)
            {
                break;
            }
            const uint64_t nowNs = SDL_GetTicksNS();
            const uint64_t deltaNs = nowNs - uncapLastTickNs;
            uncapLastTickNs = nowNs;
            loopStepDtMs = static_cast<float>(deltaNs) / 1'000'000.0f;
            if (loopStepDtMs <= 0.0f)
            {
                loopStepDtMs = 0.001f;  // sub-µs ticks: floor at 1 µs
            }
        }
        ++subStepsThisFrame;

        // One profiler frame per game tick.
        if (profilerFrameOpen)
        {
            ProfilingService::Get().EndFrame();
        }
        ProfilingService::Get().BeginFrame();
        profilerFrameOpen = true;

        // Fixed timestep when capped; real elapsed time when uncapped, so
        // animations run at real speed however often the game ticks.
        Deki::Time::UpdateF(loopStepDtMs);

        m_Play.lastGameUpdateTime = currentTime;

        // Deletions the previous tick deferred.
        Deki::Engine::GetInstance().GetSceneSystem().FlushPendingDeletions();

        Deki::Scene* activeScene = Deki::Engine::GetInstance().GetRootScene();
        if (!activeScene)
        {
            DEKI_LOG_ERROR("Play mode: root scene is null - stopping");
            ProfilingService::Get().EndFrame();
            Stop();
            return;
        }

        const char* sceneName = activeScene->GetName().c_str();

        // Lifecycle, in order: InternalAwake, Awake, Start, Update.
        {
            CrashHandler::SetPlayModeContext("InternalAwake", sceneName);
            PROFILE_SCOPE("lifecycle", "ProcessInternalAwake");
            if (!GuardedInvoke("InternalAwake", sceneName, m_Play.crashedThisFrame,
                               [&] { m_Play.lifecycleManager->ProcessInternalAwake(activeScene); }))
            {
                ProfilingService::Get().EndFrame();
                return;
            }
        }

        {
            CrashHandler::SetPlayModeContext("Awake", sceneName);
            PROFILE_SCOPE("lifecycle", "ProcessAwake");
            if (!GuardedInvoke("Awake", sceneName, m_Play.crashedThisFrame,
                               [&] { m_Play.lifecycleManager->ProcessAwake(activeScene); }))
            {
                ProfilingService::Get().EndFrame();
                return;
            }
        }

        {
            CrashHandler::SetPlayModeContext("Start", sceneName);
            PROFILE_SCOPE("lifecycle", "ProcessStart");
            if (!GuardedInvoke("Start", sceneName, m_Play.crashedThisFrame,
                               [&] { m_Play.lifecycleManager->ProcessStart(activeScene); }))
            {
                ProfilingService::Get().EndFrame();
                return;
            }
        }

        {
            CrashHandler::SetPlayModeContext("Update", sceneName);
            PROFILE_SCOPE("lifecycle", "ProcessUpdate");
            if (!GuardedInvoke("Update", sceneName, m_Play.crashedThisFrame,
                               [&] { m_Play.lifecycleManager->ProcessUpdate(activeScene); }))
            {
                ProfilingService::Get().EndFrame();
                return;
            }
        }

        // Package systems registered with Deki::Engine::RegisterUpdate, such
        // as tweens. On a device Engine::Update runs them; play mode drives
        // the lifecycle by hand, so it runs them itself.
        {
            CrashHandler::SetPlayModeContext("UpdateCallbacks", sceneName);
            PROFILE_SCOPE("lifecycle", "UpdateCallbacks");
            if (!GuardedInvoke(
                    "UpdateCallbacks", sceneName, m_Play.crashedThisFrame,
                    [&] { Deki::Engine::GetInstance().RunUpdateCallbacks(static_cast<uint32_t>(loopStepDtMs)); }))
            {
                ProfilingService::Get().EndFrame();
                return;
            }
        }

        {
            CrashHandler::SetPlayModeContext("ComponentUpdates", sceneName);
            PROFILE_SCOPE("lifecycle", "ComponentUpdates");
            float deltaSeconds = loopStepDtMs / 1000.0f;
            if (!GuardedInvoke("ComponentUpdates", sceneName, m_Play.crashedThisFrame,
                               [&]
                               {
                                   for (Deki::Object* obj : activeScene->GetObjects())
                                   {
                                       updateComponentsRecursive(updateComponentsRecursive, obj, deltaSeconds);
                                   }
                               }))
            {
                ProfilingService::Get().EndFrame();
                return;
            }
        }

        // A scene the game opened (Engine::OpenScene): the one left goes,
        // then the new one is loaded, as on a device.
        Deki::Engine::GetInstance().OpenRequestedScene();

        // The tick may have switched scenes (SetRootScene). Run the new
        // scene's lifecycle before the next sub-step or render.
        Deki::Scene* currentScene = Deki::Engine::GetInstance().GetRootScene();
        if (currentScene != activeScene && currentScene != nullptr)
        {
            const char* newSceneName = currentScene->GetName().c_str();

            // Baked font GUIDs for the new scene, as at play start.
            if (m_PackageLoader)
            {
                m_PackageLoader->NotifyPlayModeStart(currentScene);
            }

            {
                CrashHandler::SetPlayModeContext("NewSceneInternalAwake", newSceneName);
                PROFILE_SCOPE("lifecycle", "ProcessInternalAwake");
                if (!GuardedInvoke("NewSceneInternalAwake", newSceneName, m_Play.crashedThisFrame,
                                   [&] { m_Play.lifecycleManager->ProcessInternalAwake(currentScene); }))
                {
                    ProfilingService::Get().EndFrame();
                    return;
                }
            }
            {
                CrashHandler::SetPlayModeContext("NewSceneAwake", newSceneName);
                PROFILE_SCOPE("lifecycle", "ProcessAwake");
                if (!GuardedInvoke("NewSceneAwake", newSceneName, m_Play.crashedThisFrame,
                                   [&] { m_Play.lifecycleManager->ProcessAwake(currentScene); }))
                {
                    ProfilingService::Get().EndFrame();
                    return;
                }
            }
            {
                CrashHandler::SetPlayModeContext("NewSceneStart", newSceneName);
                PROFILE_SCOPE("lifecycle", "ProcessStart");
                if (!GuardedInvoke("NewSceneStart", newSceneName, m_Play.crashedThisFrame,
                                   [&] { m_Play.lifecycleManager->ProcessStart(currentScene); }))
                {
                    ProfilingService::Get().EndFrame();
                    return;
                }
            }
        }

        CrashHandler::ClearPlayModeContext();
        // The frame ends when the next tick begins, or after the render below.
    }

    // Game FPS over a 500 ms window: sub-steps divided by elapsed time. One
    // editor frame is too noisy, since some run no sub-steps (game slower
    // than editor) and others run several.
    m_Play.fpsSubStepCounter += subStepsThisFrame;
    m_Play.fpsWindowMs += deltaMs;
    if (m_Play.fpsWindowMs >= 500.0f)
    {
        m_Play.gameFps =
            (m_Play.fpsSubStepCounter > 0) ? (m_Play.fpsSubStepCounter * 1000.0f) / m_Play.fpsWindowMs : 0.0f;
        m_Play.fpsSubStepCounter = 0;
        m_Play.fpsWindowMs = 0.0f;
    }

    // Render once per editor frame from the latest state. With no sub-steps
    // this frame nothing changed, so skip it; the Play view keeps showing the
    // current texture.
    if (subStepsThisFrame > 0)
    {
        Deki::Scene* renderScene = Deki::Engine::GetInstance().GetRootScene();
        const char* renderSceneName = renderScene ? renderScene->GetName().c_str() : "<unknown>";
        CrashHandler::SetPlayModeContext("Render", renderSceneName);
        if (!GuardedInvoke("Render", renderSceneName, m_Play.crashedThisFrame, [&] { RenderPlayMode(); }))
        {
            if (profilerFrameOpen)
            {
                ProfilingService::Get().EndFrame();
            }
            return;
        }
        CrashHandler::ClearPlayModeContext();
    }
    if (profilerFrameOpen)
    {
        ProfilingService::Get().EndFrame();
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

    Deki::Scene* activeScene = Deki::Engine::GetInstance().GetRootScene();
    if (!activeScene)
    {
        DEKI_LOG_ERROR("RenderPlayMode: root scene is null - stopping");
        Stop();
        return;
    }

    // The same render system a device uses: it finds the camera, clears the
    // buffer and draws every object.
    {
        PROFILE_SCOPE("rendering", "RenderSystem");
        m_Play.renderSystem->Render(activeScene);
    }

    // Frame capture, toggled with F9.
    if (m_FrameCaptureService && m_FrameCaptureService->IsCapturing())
    {
        m_FrameCaptureService->CaptureFrame(m_Play.renderSystem->GetFrameBuffer(), m_Play.width, m_Play.height,
                                            m_Play.renderSystem->GetColorFormat());
    }

    // Present the framebuffer through the display (in the editor, a GL texture upload).
    {
        PROFILE_SCOPE("editor", "PresentFrame");
        if (Deki::Engine::GetInstance().GetDisplay())
        {
            // Partial upload of the changed rectangles when dirty tracking is
            // on, the whole frame otherwise (same path as the device).
            Deki::Engine::GetInstance().PresentFrame();
            m_Play.ready = true;  // a frame exists, so the play texture can be shown
        }
    }
}

void EditorApp::ResizePlayMode(int width, int height)
{
    if (m_PlayModeState == PlayModeState::Stopped || !s_EditorDisplay || width <= 0 || height <= 0 ||
        (width == m_Play.width && height == m_Play.height))
    {
        return;
    }

    // A new display of the new size, as play start makes one. SetDisplay shuts
    // the old one down and sizes the framebuffer to the new one.
    auto display = std::make_unique<Deki::EditorDisplay>();
    display->SetColorFormat(s_EditorDisplay->GetColorFormat());
    if (!display->Initialize(width, height))
    {
        DEKI_LOG_ERROR("Play mode could not make a %dx%d display; it stays at %dx%d", width, height, m_Play.width,
                       m_Play.height);
        return;
    }
    Deki::Engine::GetInstance().SetDisplay(display.get(), "Editor");
    s_EditorDisplay = std::move(display);

    Deki::IRenderSystem* renderSystem = Deki::Engine::GetInstance().GetRenderSystem();
    if (!renderSystem || renderSystem->GetScreenWidth() != width || renderSystem->GetScreenHeight() != height)
    {
        DEKI_LOG_ERROR("Play mode could not get a %dx%d framebuffer", width, height);
        Stop();
        return;
    }
    m_Play.width = width;
    m_Play.height = height;
    m_Play.textureId = s_EditorDisplay->GetTextureId();
    DEKI_LOG_EDITOR("Play mode resized: %dx%d", width, height);

    // Draw a frame at the new size now: paused play renders nothing by itself,
    // and the new texture is empty until a frame is presented. Before the first
    // frame the scene has not started, so the first update renders instead.
    Deki::Scene* renderScene = Deki::Engine::GetInstance().GetRootScene();
    if (m_Play.ready && renderScene)
    {
        const char* renderSceneName = renderScene->GetName().c_str();
        CrashHandler::SetPlayModeContext("Render", renderSceneName);
        if (GuardedInvoke("Render", renderSceneName, m_Play.crashedThisFrame, [&] { RenderPlayMode(); }))
        {
            CrashHandler::ClearPlayModeContext();
        }
    }
}

void EditorApp::HandlePlayModeInput(float gameX, float gameY, bool isDown, bool isMove, bool isUp)
{
    if (m_PlayModeState != PlayModeState::Playing)
    {
        return;
    }

    Deki::Scene* activeScene = Deki::Engine::GetInstance().GetRootScene();
    if (!activeScene)
    {
        return;
    }

    if (!m_Play.camera)
    {
        return;
    }

    float worldX, worldY;
    m_Play.camera->ScreenToWorld(gameX, gameY, m_Play.width, m_Play.height, worldX, worldY);

    if (auto* inputSystem = Deki::Engine::GetInstance().GetInputSystem())
    {
        inputSystem->DispatchInput(activeScene, worldX, worldY, isDown, isMove, isUp);
    }
}

void EditorApp::HandlePlayModeKey(uint32_t key, bool down)
{
    // Key-downs only while playing. Key-ups always pass, so a key held when
    // play stops or focus leaves the Play view does not stay pressed.
    if (down && m_PlayModeState != PlayModeState::Playing)
    {
        return;
    }

    if (auto* inputSystem = Deki::Engine::GetInstance().GetInputSystem())
    {
        inputSystem->DispatchKey(key, down);
    }
}

}  // namespace DekiEditor
