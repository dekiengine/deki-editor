#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <imgui.h>  // ImGuiID

namespace Deki { class Scene; }
namespace Deki { class ICamera; }
namespace Deki { class LifecycleManager; }
namespace Deki { class IRenderSystem; }

namespace DekiEditor
{

/// Play/pause/stop, as seen by everything outside PlayMode.cpp.
enum class PlayModeState
{
    Stopped,  // Edit mode - normal editor behavior
    Playing,  // Play mode - running game logic and engine rendering
    Paused    // Paused - game logic frozen, can inspect state
};

/// Everything play mode needs while it is running.
///
/// This is a state bag, not a controller, and that is deliberate. PlayMode.cpp
/// reaches back into nine EditorApp collaborators - the current scene, project,
/// asset pipeline, package loader, hot reload, frame capture and three panels -
/// so a "PlayModeController" would either take a ten-field context (EditorApp
/// with extra steps) or hold an EditorApp back-pointer, which is the coupling
/// pattern we are trying to get away from. The coupling there is real, not
/// accidental, and hiding it behind a struct would not reduce it.
///
/// What was accidental was that these nineteen fields sat in EditorApp.h where
/// every panel reading that header had to scroll past them, even though only
/// PlayMode.cpp ever touches them. Grouping them here is the honest fix: the
/// header sheds nineteen fields and gains one, and the file that owns this
/// state is now the only file that names it.
///
/// m_PlayModeState is deliberately NOT here - six sites outside PlayMode.cpp
/// read it, so it stays a first-class member of EditorApp.
struct PlayModeRuntime
{
    Deki::ICamera* camera = nullptr;  // Resolved on Play, cleared on Stop
    uint32_t textureId = 0;
    int width = 0;
    int height = 0;

    // Fixed-timestep bookkeeping
    uint64_t lastFrameTime = 0;       // Last frame timestamp for accumulator delta
    uint64_t lastGameUpdateTime = 0;  // Last game update timestamp for real delta
    float timeAccumulator = 0.0f;     // Fixed timestep accumulator (ms)

    // Smoothed game FPS over a rolling window
    float gameFps = 0.0f;
    int fpsSubStepCounter = 0;  // Sub-steps accumulated this sample window
    float fpsWindowMs = 0.0f;   // Wall-clock ms accumulated this sample window

    int lastSwapInterval = 1;  // Last applied SDL_GL swap interval (1=vsync, 0=off)

    // Non-owning, from Deki::Engine
    Deki::LifecycleManager* lifecycleManager = nullptr;
    Deki::IRenderSystem* renderSystem = nullptr;

    std::unique_ptr<::Deki::Scene> scene;  // Isolated deep copy for play mode
    std::string sceneBackup;         // JSON backup of the scene before play

    bool ready = false;                // True after the first frame is rendered
    bool crashedThisFrame = false;     // A guarded play-mode call faulted; Stop() next frame
    ImGuiID prePlaySelectedTabId = 0;  // Tab to restore in Play View's dock node on Stop
};

}  // namespace DekiEditor
