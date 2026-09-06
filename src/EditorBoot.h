#pragma once

#include <SDL3/SDL.h>

namespace DekiEditor
{

class EditorApp;

/// How the editor is being started.
///
/// There is one startup path. `headless` decides whether it puts a window on
/// the screen, and nothing else: the same project open runs either way, so a
/// CLI invocation and a double-click leave the process in the same state.
///
/// Everything that used to be reimplemented per CLI verb — restoring declared
/// packages, generating build files, compiling the plugin, promoting the built
/// DLLs, loading packages and plugins, importing assets — lives in
/// EditorApp::OpenProject and therefore now happens for both.
struct BootOptions
{
    /// No window, no GL context, no ImGui context. The editor still opens the
    /// project in full; it just never draws.
    bool headless = false;

    /// Project to open. Null starts the GUI on the project manager screen; a
    /// headless boot requires one (there is nothing for it to do without).
    const char* projectPath = nullptr;

    /// Bring the MCP server up as part of startup rather than from the
    /// Activity panel toggle.
    bool startMcp = false;
    int mcpPort = 3000;
};

/// What BootPlatform created, and the interactive loop needs back.
struct BootContext
{
    SDL_Window* window = nullptr;
    SDL_GLContext glContext = nullptr;
    float dpiScale = 1.0f;
    bool headless = false;
};

/// Stage 1: SDL, window, GL, ImGui, fonts, theme, profiling.
///
/// Must run before EditorApp is constructed — the constructor logs through
/// StartupProgress and builds panels that expect the theme to exist.
/// Headless skips everything display-related and initialises SDL for events
/// only, so it runs on a machine with no display at all.
bool BootPlatform(const BootOptions& opts, BootContext& ctx);

/// Stage 2: hand the editor its window, open the project, start MCP, and
/// finish any build the open deferred.
///
/// Returns false only when a project path was given and could not be opened —
/// which is a failure for a CLI run, and for the GUI too (it falls back to the
/// project manager screen rather than a half-open project).
bool BootEditor(const BootOptions& opts, BootContext& ctx, EditorApp& editor);

/// Tear down what BootPlatform created. No-op for a headless context.
void ShutdownPlatform(BootContext& ctx);

}  // namespace DekiEditor
