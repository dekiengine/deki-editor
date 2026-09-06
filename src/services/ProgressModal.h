#pragma once

struct SDL_Window;

namespace DekiEditor
{

/**
 * ProgressModal
 *
 * Unified progress modal for displaying loading/building progress.
 * Used by both StartupProgress and HotReloadService.
 *
 * The modal shows:
 * - A title (e.g., "Deki Editor" or "Building Plugin")
 * - Current phase text
 * - Animated indeterminate progress bar
 * - Optional detail text
 */
class ProgressModal
{
public:
    // Set the SDL window for rendering (must be called after ImGui setup)
    static void SetWindow(SDL_Window* window);

    // Show the progress modal with given title, phase, and optional detail
    // This renders a complete ImGui frame and swaps buffers
    static void Show(const char* title, const char* phase, const char* detail = nullptr);

    // Draw the same card as a small window inside the CURRENT ImGui frame
    // (bottom-right, non-modal), for work that runs off the main thread. With
    // cancelRequested given, a Cancel button sets it when clicked.
    static void DrawInFrame(const char* title, const char* phase, const char* detail, bool* cancelRequested);

    // Reset the elapsed-time clock and recent-phase history. Call at the
    // start of a new logical operation (e.g., reopening a project) so the
    // displayed elapsed time and phase log don't carry over from a prior run.
    static void Reset();

    // Enable/disable the modal (when disabled, Show() does nothing)
    static void SetEnabled(bool enabled);
    static bool IsEnabled();

    // A window-close request that arrived while Show() was pumping events.
    // Show() cannot act on it (the operation it is reporting on is still
    // running), so it is parked here and the main loop replays it through
    // EditorApp::RequestQuit once the operation has finished. It used to be
    // discarded along with every other event.
    static bool TakeDeferredQuit();

private:
    static SDL_Window* s_Window;
    static bool s_Enabled;
    static bool s_QuitDeferred;
};

}  // namespace DekiEditor
