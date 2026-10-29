#pragma once

#include <SDL3/SDL.h>

namespace DekiEditor
{

class EditorApp;

/// How the editor is being started.
///
/// There is one startup path. `headless` decides only whether it puts a
/// window on the screen: the same project open runs either way, so a CLI run
/// and a double-click leave the process in the same state.
///
/// Restoring declared packages, generating build files, compiling the plugin,
/// promoting the built DLLs, loading packages and plugins, and importing
/// assets all live in EditorApp::OpenProject, so both paths do them.
struct BootOptions
{
    /// No window, GL context or ImGui context. The editor still opens the
    /// project in full and never draws.
    bool headless = false;

    /// Project to open. Null starts the GUI on the project manager screen; a
    /// headless boot needs one.
    const char* projectPath = nullptr;

    /// Start the MCP server during startup instead of from the Activity
    /// panel toggle.
    bool startMcp = false;
    int mcpPort = 3000;

    /// Come up minimized and never take focus (`--background`), as when
    /// started minimized on Windows. The command line opens its editor so.
    bool background = false;
};

/// What BootPlatform created, for the interactive loop to use.
struct BootContext
{
    SDL_Window* window = nullptr;
    SDL_GLContext glContext = nullptr;
    float dpiScale = 1.0f;
    bool headless = false;
    bool background = false;  // BootOptions::background
};

/// Stage 1: SDL, window, GL, ImGui, fonts, theme, profiling.
///
/// Must run before EditorApp is constructed: the constructor logs through
/// StartupProgress and builds panels that expect the theme to exist.
/// Headless skips everything display-related and starts SDL for events
/// only, so it runs on a machine with no display.
bool BootPlatform(const BootOptions& opts, BootContext& ctx);

/// Stage 2: hand the editor its window, open the project, start MCP, and
/// finish any build the open deferred.
///
/// Returns false only when a project path was given and could not be opened.
/// The GUI then shows the project manager screen rather than a half-open
/// project.
bool BootEditor(const BootOptions& opts, BootContext& ctx, EditorApp& editor);

/// Tears down what BootPlatform created. Does nothing for a headless context.
void ShutdownPlatform(BootContext& ctx);

/// Which CJK font draws the characters several of them have (Han characters,
/// CJK punctuation, full-width forms): Noto Sans SC for Chinese, KR for
/// Korean, JP for Japanese and every other language. Call between frames when
/// the language changes.
void SetCjkPreference(const std::string& language);

}  // namespace DekiEditor
