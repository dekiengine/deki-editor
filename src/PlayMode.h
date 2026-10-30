#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>  // ImGuiID

namespace Deki
{
class Scene;
}
namespace Deki
{
class ICamera;
}
namespace Deki
{
class LifecycleManager;
}
namespace Deki
{
class IRenderSystem;
}

namespace DekiEditor
{

/// Play/pause/stop, as seen by everything outside PlayMode.cpp.
enum class PlayModeState
{
    Stopped,  // edit mode
    Playing,  // game logic and engine rendering run
    Paused    // game logic frozen; state can be inspected
};

/// Everything play mode needs while it is running.
///
/// Plain state, not a controller, on purpose. PlayMode.cpp uses nine EditorApp
/// parts (the current scene, project, asset pipeline, package loader, hot
/// reload, frame capture and three panels), so a controller would need a
/// ten-field context or an EditorApp back-pointer. That coupling is real, and
/// wrapping it would not reduce it.
///
/// Only PlayMode.cpp touches these fields, so they live here rather than in
/// EditorApp.h, which every panel reads.
///
/// m_PlayModeState is not here: code outside PlayMode.cpp reads it, so it is
/// an EditorApp member.
struct PlayModeRuntime
{
    Deki::ICamera* camera = nullptr;  // found on Play, cleared on Stop
    uint32_t textureId = 0;
    int width = 0;
    int height = 0;

    // Fixed-timestep bookkeeping
    uint64_t lastFrameTime = 0;       // last frame's time, for the accumulator delta
    uint64_t lastGameUpdateTime = 0;  // last game update's time, for the real delta
    float timeAccumulator = 0.0f;     // fixed timestep accumulator (ms)

    // Smoothed game FPS over a rolling window
    float gameFps = 0.0f;
    int fpsSubStepCounter = 0;  // sub-steps in this sample window
    float fpsWindowMs = 0.0f;   // wall-clock ms in this sample window

    int lastSwapInterval = 1;  // last vsync set through EditorRenderer (1 = on, 0 = off)

    // Non-owning, from Deki::Engine
    Deki::LifecycleManager* lifecycleManager = nullptr;
    Deki::IRenderSystem* renderSystem = nullptr;

    std::unique_ptr<::Deki::Scene> scene;  // separate deep copy for play mode
    std::string sceneBackup;               // the scene as JSON from before Play

    bool ready = false;                // true once the first frame is rendered
    bool crashedThisFrame = false;     // a guarded play-mode call faulted; Stop() next frame
    ImGuiID prePlaySelectedTabId = 0;  // tab to select again in Play View's dock node on Stop
    // Object ids selected when Play started: the play copy keeps the ids, so
    // the same objects are selected in it, and again in the scene on Stop.
    std::vector<uint32_t> prePlaySelectedIds;
};

}  // namespace DekiEditor
