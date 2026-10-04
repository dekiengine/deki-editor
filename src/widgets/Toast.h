#pragma once

// Toast: short-lived notification cards that dismiss themselves.
//
// For a passing message. The Console's pinned entries
// (ConsolePanel::LogBuildError) belong to the COMPILER: they feed the "Build
// failed - N errors" pill, so anything else posted there reads as a broken
// build. A plain log line is easy to miss when the Console is docked behind
// another panel.
//
// Pair the two: toast what the user must see now, log what they may want to
// scroll back to later.
//
// Like ProgressModal::DrawInFrame, a toast is a non-modal card drawn inside
// the editor's own frame, anchored to the main viewport and DPI-scaled, with
// no instance to pass around.

#include <cstddef>
#include <string>

namespace DekiEditor
{

class Toast
{
public:
    enum class Level
    {
        Info,     // neutral, accent-coloured
        Success,  // green
        Warning,  // amber
        Error,    // red - a real failure, NOT a compile error (that is the Console's)
    };

    /// Queues a toast. Safe to call from any thread: builds and package work run
    /// on a ProcessRunner thread, and the queue is drained on the main thread.
    /// The toast shows for `seconds` before it fades out; hovering pauses it.
    static void Push(Level level, const char* title, const std::string& message, float seconds = 8.0f);

    /// Draws and ages the stack. Call once per frame from EditorApp::Update,
    /// INSIDE the ImGui frame, after the build progress card so a toast never
    /// lands underneath it.
    static void DrawInFrame();

    /// Drops every queued toast (project close, entering play mode).
    static void DismissAll();

    /// The number of live toasts, for tests and diagnostics.
    static std::size_t Count();
};

}  // namespace DekiEditor
