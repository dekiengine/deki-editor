#pragma once

struct SDL_Window;

namespace DekiEditor
{

/**
 * StartupProgress
 *
 * Wrapper around ProgressModal for editor startup progress.
 * Tracks startup state and provides a simple API for initialization phases.
 */
class StartupProgress
{
public:
    // Set the SDL window for rendering (must be called after ImGui setup)
    static void SetWindow(SDL_Window* window);

    // Update and render the progress screen
    // phase: Current operation (e.g., "Loading project...")
    // detail: Optional detail text (e.g., file being loaded)
    static void Update(const char* phase, const char* detail = nullptr);

    // Show progress again for a new long operation (opening another project
    // from the project list): Update draws until the next Complete().
    static void Begin();
    // Mark startup as complete (stops showing progress on subsequent Update calls)
    static void Complete();

    // Check if startup is complete
    static bool IsComplete();

private:
    static bool s_Complete;
};

}  // namespace DekiEditor
