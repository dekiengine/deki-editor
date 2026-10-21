#pragma once

struct SDL_Window;

namespace DekiEditor
{

/// The progress modal for loading and building, used by StartupProgress and
/// HotReloadService.
///
/// It shows:
/// - a title (e.g. "Deki Editor" or "Building Plugin")
/// - the current phase
/// - an animated progress bar with no fixed end
/// - optional detail text
///
/// The title and phase are drawn as given, so callers pass them already in
/// the editor's language.
class ProgressModal
{
public:
    /// Sets the SDL window to draw into. Call after ImGui setup.
    static void SetWindow(SDL_Window* window);

    /// Shows the modal with a title, phase and optional detail. Renders a
    /// complete ImGui frame and swaps buffers.
    static void Show(const char* title, const char* phase, const char* detail = nullptr);

    /// Draws the same card as a small window inside the CURRENT ImGui frame
    /// (bottom-right, non-modal), for work that runs off the main thread. With
    /// cancelRequested given, a Cancel button sets it when clicked.
    static void DrawInFrame(const char* title, const char* phase, const char* detail, bool* cancelRequested);

    /// Resets the elapsed-time clock and recent-phase history. Call at the
    /// start of a new operation (e.g. reopening a project) so the elapsed time
    /// and phase log do not carry over from the previous one.
    static void Reset();

    /// While disabled, Show() does nothing.
    static void SetEnabled(bool enabled);
    static bool IsEnabled();

    /// A window-close request that arrived while Show() was pumping events.
    /// Show() cannot act on it (the operation it reports on is still
    /// running), so it is kept here and the main loop replays it through
    /// EditorApp::RequestQuit once the operation has finished.
    static bool TakeDeferredQuit();

private:
    static SDL_Window* s_Window;
    static bool s_Enabled;
    static bool s_QuitDeferred;
};

}  // namespace DekiEditor
