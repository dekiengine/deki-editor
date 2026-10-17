#pragma once

struct SDL_Window;

namespace DekiEditor
{

/// Startup progress on top of ProgressModal: tracks whether startup is done
/// and offers a small API for the startup phases.
class StartupProgress
{
public:
    /// Sets the SDL window to draw into. Call after ImGui setup.
    static void SetWindow(SDL_Window* window);

    /// Updates and draws the progress screen. `phase` is the current
    /// operation in English (e.g. "Loading project..."), shown in the editor's
    /// language, so callers mark it with TrMark rather than Tr. `detail` is
    /// optional extra text shown as it is (e.g. the file being loaded).
    static void Update(const char* phase, const char* detail = nullptr);

    /// Shows progress again for a new long operation (opening another
    /// project from the project list): Update draws until the next Complete().
    static void Begin();
    /// Marks startup complete; later Update calls draw nothing.
    static void Complete();

    static bool IsComplete();

private:
    static bool s_Complete;
};

}  // namespace DekiEditor
